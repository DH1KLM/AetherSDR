#include "core/backends/anan/P2Protocol.h"

#include <algorithm>
#include <cstring>

namespace AetherSDR::anan {

std::optional<DiscoveryReply> parseOpenHpsdrDiscoveryReply(
    std::span<const std::uint8_t> data) noexcept
{
    if (const auto p2 = parseDiscoveryReply(data))
        return p2;
    const auto p1 = p1::parseDiscovery(data);
    if (!p1)
        return std::nullopt;
    const auto board = p1::normalizedBoardId(p1->boardId);
    if (!board)
        return std::nullopt;
    DiscoveryReply result;
    result.wireProtocol = 1;
    result.streaming = p1->streaming;
    result.mac = p1->mac;
    result.boardId = static_cast<std::uint8_t>(*board);
    result.firmwareVer = p1->firmware;
    result.reportedReceivers = data.size() > 20 ? data[20] : 0;
    return result;
}

namespace {

std::uint16_t readU16be(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t readU32be(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)  |  static_cast<std::uint32_t>(p[3]);
}

void writeU16be(std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

void writeU32be(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

// Sign-extend a 24-bit two's-complement value carried in the low 24 bits of
// a 32-bit int.
int signExtend24(std::int32_t raw) noexcept
{
    if (raw & 0x800000)
        raw -= (1 << 24);
    return raw;
}

//DH1KLM: Thetis console.cs setBPF1ForOrionIISaturn() defaults, with
//the exact 1.5/2.1/5.5/11/22/35/61.44 MHz edges. Thetis' selection byte
//is mapped to the Orion-MkII/Saturn Alex0 register by network.c:
//0x10->bit6, 0x08->bit5, 0x04->bit4, 0x01->bit1, 0x02->bit2,
//0x40->bit3. Thetis selects the same BPF1 on Orion MkII and Saturn;
//legacy ALEX HPF boards remain gated until their own profiles are ported.
std::uint32_t saturnBpf1RegisterBits(std::uint32_t phaseWord) noexcept
{
    constexpr double kPhaseScale = static_cast<double>(kDspClockHz) / 4294967296.0;
    const double hz = static_cast<double>(phaseWord) * kPhaseScale;
    //DH1KLM: The phase word has finite resolution (~0.0286 Hz per step).
    //Allow half a step around integer-Hz edges so an exact band-edge tune
    //does not fall through solely because phaseWord() rounded to its neighbour.
    constexpr double kHalfPhaseStepHz = 0.02;
    std::uint32_t bits = 0;
    if (hz >= 1'500'000.0 - kHalfPhaseStepHz && hz <= 2'099'999.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 6;  // Thetis BPF1 160 m row (0x10)
    } else if (hz >= 2'100'000.0 - kHalfPhaseStepHz && hz <= 5'499'999.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 5;  // Thetis BPF1 80/60 m row (0x08)
    } else if (hz >= 5'500'000.0 - kHalfPhaseStepHz && hz <= 10'999'999.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 4;  // Thetis BPF1 40/30 m row (0x04)
    } else if (hz >= 11'000'000.0 - kHalfPhaseStepHz && hz <= 21'999'999.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 1;  // Thetis BPF1 20/15 m row (0x01)
    } else if (hz >= 22'000'000.0 - kHalfPhaseStepHz && hz <= 34'999'999.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 2;  // Thetis BPF1 12/10 m row (0x02)
    } else if (hz >= 35'000'000.0 - kHalfPhaseStepHz && hz <= 61'440'000.0 + kHalfPhaseStepHz) {
        bits = std::uint32_t{1} << 3;  // Thetis BPF1 6 m row (0x40)
    }
    return bits; // zero means Thetis' bypass region (outside configured rows)
}

//DH1KLM: Thetis setAlexHPF / setup.designer.cs defaults, in Hz.
// Legacy 1.5 MHz HPF starts at 1.8 MHz, not the MkII BPF's 1.5 MHz.
std::uint32_t legacyHpfRegisterBits(std::uint32_t word) noexcept
{
    const double hz = static_cast<double>(word) * kDspClockHz / 4294967296.0;
    struct HpfBand { double lowHz; double highHz; int bit; };
    constexpr std::array<HpfBand, 6> bands{{
        {1'800'000, 6'499'999, 6}, {6'500'000, 9'499'999, 5},
        {9'500'000, 12'999'999, 4}, {13'000'000, 19'999'999, 1},
        {20'000'000, 49'999'999, 2}, {50'000'000, 61'440'000, 3}
    }};
    constexpr double kHalfPhaseStepHz = 0.02;
    for (const HpfBand& band : bands) {
        if (hz >= band.lowHz - kHalfPhaseStepHz && hz <= band.highHz + kHalfPhaseStepHz) {
            return std::uint32_t{1} << band.bit;
        }
    }
    return 0; // Thetis' bypass region
}

//DH1KLM: Thetis setup.designer.cs LPF defaults, console.cs setAlexLPF,
// and network.h rbpfilter relay positions. Only the Orion MkII RX profile
// selects these; the qualified Saturn packet bytes stay unchanged.
std::uint32_t receiveLpfRegisterBits(std::uint32_t word) noexcept
{
    const double hz = static_cast<double>(word) * kDspClockHz / 4294967296.0;
    struct LpfBand { double lowHz; double highHz; int bit; };
    constexpr std::array<LpfBand, 7> bands{{
        {0, 2'500'000, 23},
        {2'500'001, 5'000'000, 22},
        {5'000'001, 8'000'000, 21},
        {8'000'001, 16'500'000, 20},
        {16'500'001, 24'000'000, 31},
        {24'000'001, 35'600'000, 30},
        {35'600'001, 61'440'000, 29}
    }};
    constexpr double kHalfPhaseStepHz = 0.02;
    for (const LpfBand& band : bands) {
        if (hz >= band.lowHz - kHalfPhaseStepHz && hz <= band.highHz + kHalfPhaseStepHz) {
            return std::uint32_t{1} << band.bit;
        }
    }
    return std::uint32_t{1} << 29; // Thetis' out-of-band/bypass fallback: 6 m LPF
}

}  // namespace

std::array<std::uint8_t, 60> buildDiscovery() noexcept
{
    std::array<std::uint8_t, 60> pkt{};
    pkt[4] = 0x02;  // Discovery Command (spec p.42-43)
    return pkt;
}

std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> data) noexcept
{
    // Bounds-check BEFORE indexing (Principle VII) -- this parses
    // unauthenticated UDP. 24 bytes covers the header through byte 23.
    if (data.size() < 24)
        return std::nullopt;
    if (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 0)
        return std::nullopt;
    if (data[4] != 0x02 && data[4] != 0x03)
        return std::nullopt;

    //DH1KLM: High-Priority Status is also 60 bytes and can share sequence
    // zero plus command-looking byte 4. Require plausible Discovery identity
    // fields before treating a datagram as a board reply; otherwise a status
    // packet could accidentally authorize a Saturn-profile session.
    bool macAllZero = true;
    bool macAllOnes = true;
    for (std::size_t i = 5; i <= 10; ++i) {
        macAllZero = macAllZero && data[i] == 0;
        macAllOnes = macAllOnes && data[i] == 0xFF;
    }
    //DH1KLM: Thetis clsRadioDiscovery reads byte 12 as ProtocolSupported,
    // without a decimal-version range gate. Patroclus legitimately sends 2;
    // the P2 packet signature above, not this opaque value, selects P2.
    if (macAllZero || macAllOnes || (data[5] & 0x01) != 0) {
        return std::nullopt;
    }

    DiscoveryReply r;
    r.streaming = data[4] == 0x03;
    for (std::size_t i = 0; i < r.mac.size(); ++i)
        r.mac[i] = data[5 + i];
    r.boardId = data[11];
    r.protoVersionRaw = data[12];
    r.firmwareVer = data[13];
    r.reportedReceivers = data[20];
    r.freqIsPhaseWord = data[21] != 0;
    r.endianByteRaw = data[22];
    r.p2appBuild = data[23];
    return r;
}

std::array<std::uint8_t, 60> buildGeneral(int alexEnableMask, bool disablePa) noexcept
{
    std::array<std::uint8_t, 60> pkt{};
    pkt[4] = 0x00;  // Command byte for the General Packet (spec p.19)
    // Bytes 5-22 (port table) left zero: "if set to zero the default port
    // will be used" (p.19-20). No renegotiation in Phase 1b.
    pkt[37] = 0x08; // bit[3]: DDC/DUC frequency as PHASE WORD (p.21) --
                     // matches Discovery byte 21 == 1, not a guess.
    pkt[38] = 0x01; // bit[0]: hardware reset timer / watchdog ON (p.8, p.21).
                     // See buildGeneral()'s header comment for why this stays
                     // on deliberately.
    pkt[39] = 0x00; // BE + 3-byte -- the only format Discovery byte22==0
                     // declares (p.22, p.45). See discoveryDeclaresBigEndian3Byte().
    //DH1KLM: Thetis CmdGeneral enables the two ALEX words in byte 59.
    // Orion MkII RX also disables the PA in byte 58; no TX path exists here.
    pkt[58] = disablePa ? 0x01 : 0x00;
    pkt[59] = static_cast<std::uint8_t>(alexEnableMask & 0x03);
    return pkt;
}

std::array<std::uint8_t, 1444> buildDdcSpecific(std::span<const DdcConfig> ddcs, int numAdcs,
                                                bool ditherEnabled, bool randomEnabled,
                                                int firstDdcIndex) noexcept
{
    std::array<std::uint8_t, 1444> pkt{};  // full spec length (80 DDCs); unused DDCs
                                            // stay zero/disabled
    pkt[4] = static_cast<std::uint8_t>(numAdcs);  // "number of ADCs the hardware
                                                   // supports" (p.25)
    // Bytes 5/6 are per-ADC bitmaps (protocol p.25). Never enable a
    // nonexistent ADC1 on a one-ADC board. The two-ADC Saturn encoding
    // remains byte-identical (0x03 when enabled).
    const std::uint8_t adcMask = numAdcs == 1 ? 0x01 : 0x03;
    pkt[5] = ditherEnabled ? adcMask : 0x00;
    pkt[6] = randomEnabled ? adcMask : 0x00;
    //DH1KLM: Logical receiver 0 may occupy physical DDC2. Disabled rows
    // remain zero; never enable DDC0/1 just to reach the Orion MkII RX row.
    if (firstDdcIndex < 0 || firstDdcIndex >= kMaxDdcs) {
        return pkt;
    }

    // Clamp rather than trust the caller: writing rows for more DDCs than
    // kMaxDdcs would keep landing inside this 1444-byte packet (the spec has
    // room for 80), so this is not a buffer guard -- it is a refusal to
    // enable hardware receivers the rest of this backend is not driving.
    const std::size_t count =
        std::min(ddcs.size(), static_cast<std::size_t>(kMaxDdcs - firstDdcIndex));

    std::uint8_t enableBits = 0;
    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t physicalDdc = n + static_cast<std::size_t>(firstDdcIndex);
        enableBits |= static_cast<std::uint8_t>(1u << physicalDdc);
        // One 6-byte row per DDC: ADC select, 2-byte rate, CIC1, CIC2,
        // sample size. Offsets confirmed against the single-DDC layout this
        // replaced (DDC0: 17 / 18-19 / 20-21 / 22), which is bench-proven.
        const std::size_t row = 17 + 6 * physicalDdc;
        pkt[row] = static_cast<std::uint8_t>(ddcs[n].adcIndex);
        writeU16be(&pkt[row + 1], static_cast<std::uint16_t>(ddcs[n].rateKsps));
        // row+3, row+4 = CIC1/CIC2, "For Future use" (p.25) -- left zero.
        pkt[row + 5] = 24;   // sample size, 24 bits (default, made explicit)
    }
    pkt[7] = enableBits;   // bit[n] enables DDC n
    return pkt;
}

std::array<std::uint8_t, 1444> buildDdcSpecific(int ddc0RateKsps, int numAdcs,
                                                bool ditherEnabled, bool randomEnabled,
                                                int ddc0AdcIndex) noexcept
{
    // Delegates so the row layout has exactly one implementation; the
    // single-DDC output stays byte-identical to what this function built
    // before the multi-DDC overload existed (pinned by the existing tests).
    const DdcConfig one{ddc0RateKsps, ddc0AdcIndex};
    return buildDdcSpecific(std::span<const DdcConfig>(&one, 1),
                            numAdcs, ditherEnabled, randomEnabled);
}

std::array<std::uint8_t, 1444> buildHighPriority(bool run,
                                                  std::span<const std::uint32_t> ddcFreqWords,
                                                  bool bypassAdc0Filters,
                                                  bool bypassAdc1Filters,
                                                  int adc0AttenuationDb,
                                                  int adc1AttenuationDb,
                                                  int rxAntenna,
                                                  int firstDdcIndex,
                                                  bool selectReceiveLpf,
                                                  ReceiveFilterKind filterKind,
                                                  int numAdcs, const FilterConfig* filters) noexcept
{
    std::array<std::uint8_t, 1444> pkt{};  // full spec length
    pkt[4] = run ? 0x01 : 0x00;  // bit[0] = run. Bits[1..4] = PTT0..3 -- there is no
                                  // parameter to set them; see this header's own comment.
    // One 4-byte frequency/phase word per DDC at 9 + 4*n (p.32). Same
    // clamp-and-ignore contract as buildDdcSpecific().
    const bool validOffset = firstDdcIndex >= 0 && firstDdcIndex < kMaxDdcs;
    const std::size_t count = validOffset
        ? std::min(ddcFreqWords.size(), static_cast<std::size_t>(kMaxDdcs - firstDdcIndex))
        : 0;
    for (std::size_t n = 0; n < count; ++n) {
        writeU32be(&pkt[9 + 4 * (n + static_cast<std::size_t>(firstDdcIndex))], ddcFreqWords[n]);
    }
    // Alex0 register, bytes 1432-1435. Thetis netInterface.c maps RX ANT1,
    // ANT2, ANT3 to bits 24, 25, 26 respectively. Invalid values fail safe to
    // ANT1 rather than emitting no antenna selection.
    std::uint32_t alex0 = std::uint32_t{1} << (23 + ((rxAntenna >= 1 && rxAntenna <= 3)
                                                     ? rxAntenna : 1));
    // Apply Thetis' Saturn BPF1 selection from the tuned phase word unless the
    // caller explicitly requests bypass; outside the configured rows, bypass.
    //DH1KLM: Select the board's HPF/BPF table independently of DDC placement.
    const std::uint32_t rxWord = count > 0 ? ddcFreqWords[0] : 0;
    const double hz = static_cast<double>(rxWord) * kDspClockHz / 4294967296.0;
    const std::uint32_t bpfBits = filters ? receiveFilterBits(*filters, hz)
        : (filterKind == ReceiveFilterKind::LegacyHpf
            ? legacyHpfRegisterBits(rxWord) : saturnBpf1RegisterBits(rxWord));
    if (bypassAdc0Filters || bpfBits == 0)
        alex0 |= std::uint32_t{1} << 12;
    else
        alex0 |= bpfBits;
    if (selectReceiveLpf) {
        //DH1KLM: Select the antenna-chain LPF as well as the ADC0 BPF1.
        alex0 |= filters ? lowPassFilterBits(*filters, hz) : receiveLpfRegisterBits(rxWord);
    }
    //DH1KLM: Thetis CmdHighPriority byte 1401: OC1..7 occupy bits 1..7.
    pkt[1401] = run && filters ? static_cast<std::uint8_t>(receiveOcMask(*filters, hz) << 1) : 0;
    writeU32be(&pkt[1432], alex0);
    if (selectReceiveLpf) {
        //DH1KLM: Thetis SetAntBits/SetAlexLPFBits also populate Alex1's upper
        // relay word (P2 4.3+). RX-only operation follows the same antenna/LPF;
        // ADC1's lower word stays independent and no T/R or PTT bit is set.
        writeU32be(&pkt[1428], alex0 & 0xe7f00000u);
    }
    // Alex1 (RX2/BPF2) register, bytes 1430-1431 -- a separate 16-bit word,
    // bit 12 "HF Bypass 2" (p.90-91's own Alex1 bit table).
    if (numAdcs > 1 && bypassAdc1Filters) {
        writeU16be(&pkt[1430], std::uint16_t{1} << 12);
    } else if (numAdcs > 1 && filters) {
        //DH1KLM: One receiver: RX2 follows the same tuned frequency (Thetis SetAlex2HPFBits).
        writeU16be(&pkt[1430], static_cast<std::uint16_t>(bpfBits == 0
            ? std::uint32_t{1} << 12 : bpfBits));
    }
    // Step attenuators, pp.34,36: byte 1443 = ADC0, byte 1442 = ADC1, 0-31 dB.
    pkt[1443] = static_cast<std::uint8_t>(
        std::clamp(adc0AttenuationDb, 0, kMaxStepAttenuationDb));
    pkt[1442] = numAdcs > 1 ? static_cast<std::uint8_t>(
        std::clamp(adc1AttenuationDb, 0, kMaxStepAttenuationDb)) : 0;
    return pkt;
}

std::array<std::uint8_t, 1444> buildHighPriority(bool run, std::uint32_t ddc0FreqWord,
                                                  bool bypassAdc0Filters,
                                                  bool bypassAdc1Filters,
                                                  int adc0AttenuationDb,
                                                  int adc1AttenuationDb,
                                                  int rxAntenna) noexcept
{
    // Delegates, same reasoning as buildDdcSpecific()'s single-DDC overload:
    // one implementation of the row layout, and byte-identical output to
    // what this built before (pinned by the existing tests).
    return buildHighPriority(run, std::span<const std::uint32_t>(&ddc0FreqWord, 1),
                             bypassAdc0Filters, bypassAdc1Filters,
                             adc0AttenuationDb, adc1AttenuationDb, rxAntenna);
}

std::array<std::uint8_t, kSpeakerPacketBytes> buildSpeakerAudio(
    std::uint32_t sequence, std::span<const std::int16_t> interleavedLr,
    bool swapChannels) noexcept
{
    std::array<std::uint8_t, kSpeakerPacketBytes> pkt{};
    writeU32be(pkt.data(), sequence);
    // Zero-initialised above, so the short-input case needs no separate fill
    // path -- everything past `count` is already silence. See the header for why
    // silence is the right answer here and a refusal is not.
    constexpr std::size_t kMaxSamples = kSpeakerSampleBytes / sizeof(std::int16_t);
    const std::size_t count = std::min(interleavedLr.size(), kMaxSamples);
    for (std::size_t i = 0; i < kMaxSamples; ++i) {
        // Reinterpreted through the unsigned type, not shifted as signed: a
        // right shift of a negative value is implementation-defined, and every
        // sample below -1 dBFS is negative half the time. The two's-complement
        // bit pattern is what the wire wants and what the cast produces.
        //DH1KLM: Thetis LRAudioSwap exchanges Hermes/Hermes-II codec channels.
        const std::size_t source = swapChannels ? (i ^ std::size_t{1}) : i;
        if (source < count) {
            writeU16be(pkt.data() + 4 + i * sizeof(std::int16_t),
                       static_cast<std::uint16_t>(interleavedLr[source]));
        }
    }
    return pkt;
}

std::optional<HighPriorityStatus> parseHighPriorityStatus(
    std::span<const std::uint8_t> data) noexcept
{
    if (data.size() != kHighPriorityStatusBytes)
        return std::nullopt;
    HighPriorityStatus s;
    s.seq = readU32be(data.data());
    s.speakerUnderflow = (data[30] & 0b0000'1000) != 0;
    s.speakerFifoLevel = readU16be(data.data() + 37);
    // In bounds by the exact-size guard above, which admits only 60 bytes.
    s.supplyRailCounts = readU16be(data.data() + 57);
    return s;
}

std::optional<int> ddcIndexForSenderPort(std::uint16_t senderPort, int numDdc,
                                          std::uint16_t basePort) noexcept
{
    if (numDdc <= 0 || senderPort < basePort)
        return std::nullopt;
    // Widened to int before subtracting: both operands are uint16_t, and the
    // guard above already rules out the wrap case, but doing the arithmetic
    // in int keeps that correctness local rather than resting on the guard.
    const int index = static_cast<int>(senderPort) - static_cast<int>(basePort);
    if (index >= numDdc)
        return std::nullopt;
    return index;
}

std::optional<DdcFrame> parseDdcFrame(std::span<const std::uint8_t> data) noexcept
{
    // Bounds-check BEFORE indexing (Principle VII) -- the declared sample
    // count is used to REJECT a malformed/foreign datagram below, never to
    // index past what the buffer actually contains.
    if (data.size() < static_cast<std::size_t>(kDdcHeaderLen))
        return std::nullopt;

    const std::uint32_t seq = readU32be(&data[0]);
    // bytes 4-11 (timestamp) are unused in Phase 1b -- header field only, p.54
    const std::uint16_t bitsPerSample = readU16be(&data[12]);
    const std::uint16_t samplesDeclared = readU16be(&data[14]);

    // STRICT shape check, not a clamp -- see this function's declaration
    // comment in P2Protocol.h for why.
    const std::size_t expectedLen = static_cast<std::size_t>(kDdcHeaderLen)
        + static_cast<std::size_t>(samplesDeclared) * static_cast<std::size_t>(kDdcSampleBytes);
    if (bitsPerSample != 24 || data.size() != expectedLen)
        return std::nullopt;

    DdcFrame frame;
    frame.seq = seq;
    frame.samples = samplesDeclared;
    frame.iqRaw = data.subspan(static_cast<std::size_t>(kDdcHeaderLen),
                               data.size() - static_cast<std::size_t>(kDdcHeaderLen));
    return frame;
}

std::complex<float> decodeIqSample(const std::uint8_t* be6) noexcept
{
    const int iRaw = (static_cast<std::int32_t>(be6[0]) << 16)
                    | (static_cast<std::int32_t>(be6[1]) << 8)
                    |  static_cast<std::int32_t>(be6[2]);
    const int qRaw = (static_cast<std::int32_t>(be6[3]) << 16)
                    | (static_cast<std::int32_t>(be6[4]) << 8)
                    |  static_cast<std::int32_t>(be6[5]);
    const float scale = 1.0f / static_cast<float>(kFullScale24Bit);
    return {static_cast<float>(signExtend24(iRaw)) * scale,
            static_cast<float>(signExtend24(qRaw)) * scale};
}

void decodeIq(const DdcFrame& frame, std::vector<std::complex<float>>& out)
{
    out.reserve(out.size() + static_cast<std::size_t>(frame.samples));
    for (int i = 0; i < frame.samples; ++i)
        out.push_back(decodeIqSample(&frame.iqRaw[static_cast<std::size_t>(i) * kDdcSampleBytes]));
}

}  // namespace AetherSDR::anan
