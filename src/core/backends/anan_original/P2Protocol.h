#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

//DH1KLM openHPSDR Ethernet Protocol 2 wire primitives for the ANAN-G2 backend; a port
//DH1KLM of the bench-validated anan/spike/phase1a.py spike (Saturn, protocol 4.3,
//DH1KLM gateware 27). Sources: openHPSDR Ethernet Protocol v4.4 (VK6PH, see
//DH1KLM THIRD_PARTY_LICENSES), Saturn FPGA RTL, and p2app, the radio-side server
//DH1KLM (provenance: docs/architecture/anan-p2-backend-design.md §2.9). Page numbers
//DH1KLM below are spec pages. Socket- and Qt-free; P2Client owns the sockets.
//DH1KLM RX-ONLY BY CONSTRUCTION: no DUC-Specific, TX drive or PTT encoder exists here
//DH1KLM (TX is RFC §2.11 Phase 3, behind the engine's TX arbiter). buildSpeakerAudio()
//DH1KLM is not transmit: it sends demodulated receiver audio to the radio's speaker
//DH1KLM codec, never reaches the DUC/PA/T-R relay, and cannot key anything.

namespace AetherSDR::anan_original {

inline constexpr std::uint16_t kRadioPort = 1024;

//DH1KLM The radio's fixed listening ports (spec p.19-20 default port table).
//DH1KLM Load-bearing: DDC-Specific and High-Priority carry no packet-type byte, so the
//DH1KLM radio identifies them by arrival port; sent to kRadioPort they are silently
//DH1KLM ignored by p2app.
inline constexpr std::uint16_t kDdcSpecificPort = 1025;
inline constexpr std::uint16_t kHighPriorityPort = 1027;

//DH1KLM The radio's listening port for DDC Audio -- demodulated receiver audio sent
//DH1KLM BACK to the radio so its own codec and speaker reproduce it (spec p.36; the
//DH1KLM default port table, p.19-20, indexes it 4). p2app receives it in
//DH1KLM IncomingSpkrAudio() and DMAs it to the speaker codec FIFO.
//DH1KLM
//DH1KLM Load-bearing for the same reason kDdcSpecificPort and kHighPriorityPort are:
//DH1KLM a speaker packet carries no type discriminator, only a 4-byte sequence, so
//DH1KLM the port IS the packet's identity. Sent anywhere else it is silently dropped.
inline constexpr std::uint16_t kSpeakerAudioPort = 1028;

//DH1KLM The port DDC0 IQ nominally originates FROM per the General Packet's byte
//DH1KLM 17-18 default (spec p.19-20). NOT load-bearing for where the PC receives
//DH1KLM it: Phase 1a measured DDC0 IQ arriving at the port the client's Discovery
//DH1KLM packet was SENT FROM, matching the spec's literal "Destination Port: the
//DH1KLM Source Port of the Host that initiated Discovery" rule for every radio->PC
//DH1KLM stream (p.43, p.51, p.54) -- not a fixed per-DDC port on the PC side.
//DH1KLM Kept here only as a documented fallback a client MAY also listen on; do
//DH1KLM not treat it as the primary demultiplexing port.
inline constexpr std::uint16_t kDdc0DefaultPort = 1035;

//DH1KLM Which DDC an inbound IQ datagram belongs to, by the port the RADIO sent it
//DH1KLM FROM; nullopt outside [basePort, basePort + numDdc) (Mic Data, HP Status and
//DH1KLM the Discovery reply share the socket, so that is ordinary). basePort + n
//DH1KLM matches p2app: generalpacket.c's SetPort(VPORTDDCIQ0+i, Port+i) for a non-zero
//DH1KLM General bytes 17-18, and DefaultPorts[] VPORTDDCIQ0..9 = 1035..1044 when zero
//DH1KLM (what buildGeneral() sends). The port is the ONLY DDC discriminator: the I&Q
//DH1KLM packet has no DDC index (pp.53-54), and p2app never emits the interleaved
//DH1KLM "Synchronous DDC" framing.
[[nodiscard]] std::optional<int> ddcIndexForSenderPort(
    std::uint16_t senderPort, int numDdc,
    std::uint16_t basePort = kDdc0DefaultPort) noexcept;

//DH1KLM The six DDC0 rates in ksps, ascending. DDC0 is stepped, not continuous
//DH1KLM (AnanBackend::nearestDdc0RateKsps() snaps zoom to these). The single source for
//DH1KLM capabilities, pan limits, AnanRxDsp's droop-table check, the calibrator sweep
//DH1KLM and ConnectionPanel's picker: a rate missing from any of them silently
//DH1KLM rejects that rate's correction tables.
inline constexpr std::array<int, 6> kDdc0RatesKsps{48, 96, 192, 384, 768, 1536};

//DH1KLM Saturn boards' DSP clock. RFC §2.4; saturnregisters.c VSAMPLERATE.
inline constexpr std::uint32_t kDspClockHz = 122'880'000;

//DH1KLM 24-bit signed full scale. (1<<23)-1, not 1<<23 -- see MetisProtocol.h's
//DH1KLM kFullScale for the reasoning (the largest magnitude a 24-bit two's
//DH1KLM complement sample can actually take, matching the reference clients'
//DH1KLM dBFS scale rather than landing a fraction of a dB adrift).
inline constexpr int kFullScale24Bit = (1 << 23) - 1;

//DH1KLM Phase word: delta = 2^32 * F / Fdsp (spec p.21, p.44; RFC §2.4). This
//DH1KLM board REQUIRES phase words, not Hz -- Discovery Reply byte 21 == 1 on
//DH1KLM every G2 observed -- and General Packet byte 37 bit[3] must agree
//DH1KLM (buildGeneral() sets it). Verified in the field: phaseWord(10e6) tuned
//DH1KLM DDC0 to WWV and the carrier read back at 0 Hz baseband, 80.0 dB above the
//DH1KLM noise floor.
constexpr std::uint32_t phaseWord(double freqHz, std::uint32_t dspClockHz = kDspClockHz) noexcept
{
    constexpr double kTwoTo32 = 4294967296.0;
    const double delta = kTwoTo32 * freqHz / static_cast<double>(dspClockHz);
    //DH1KLM Round to nearest. freqHz is always non-negative for a receive tune, so
    //DH1KLM there is no negative-rounding case to handle here.
    return static_cast<std::uint32_t>(delta + 0.5);
}

//DH1KLM ---- Discovery (spec p.42-45) ----

//DH1KLM 60-byte discovery request: seq(4)=0, byte 4 = 0x02 (Discovery Command).
std::array<std::uint8_t, 60> buildDiscovery() noexcept;

struct DiscoveryReply {
    bool streaming = false;           // byte 4 == 0x03: hardware already has a session
    std::array<std::uint8_t, 6> mac{};// bytes 5-10, MSB first (p.43)
    std::uint8_t boardId = 0;         // byte 11; 10 = SATURN (ANAN-G2)
    std::uint8_t protoVersionRaw = 0; // byte 12, decimal-tenths (43 -> "4.3") -- the one
                                       //DH1KLM place the spec's own worked example is right
    std::uint8_t firmwareVer = 0;     // byte 13, an INTEGER gateware/build number, NOT
                                       //DH1KLM tenths despite the spec's worked example implying
                                       //DH1KLM one -- p2app writes GetFirmwareVersion() here
    std::uint8_t numDdc = 0;          // byte 20 -- CLAMP TO THIS, never probe past it. The
                                       //DH1KLM gateware may carry more DDC registers (VNUMDDC 10)
                                       //DH1KLM than p2app advertises (RFC Gap C).
    bool freqIsPhaseWord = false;     // byte 21: 0 = Hz, 1 = phase word
    std::uint8_t endianByteRaw = 0;   // byte 22, RAW -- see discoveryDeclaresBigEndian3Byte()
    std::uint8_t p2appBuild = 0;      // byte 23. "Beta version" per spec; p2app overwrites
                                       //DH1KLM it with P2APPVERSION on Saturn

    //DH1KLM Discovery-time PICKER filter only -- NOT for gating backend behaviour
    //DH1KLM once connected. The RFC is explicit that board type is a runtime
    //DH1KLM option on this platform (p2app.c:588 accepts -i saturn / -i orionmk2)
    //DH1KLM and must not decide what the backend does; the capability bytes and
    //DH1KLM firmware version are the load-bearing fields for that. This predicate
    //DH1KLM only answers "should a radio picker show this reply as an ANAN-G2".
    [[nodiscard]] constexpr bool isSaturn() const noexcept { return boardId == 10; }
};

//DH1KLM Byte 22 per spec p.45: ZERO means big-endian 3-byte I&Q (not "none", not
//DH1KLM little-endian); nonzero is a bitmask with bit 0 = BE. A real G2 sends 0 and
//DH1KLM decodes BE at 80.0 dB SNR vs 26.0 dB byte-swapped (WWV 10 MHz); buildGeneral()
//DH1KLM requests this via byte 39.
[[nodiscard]] constexpr bool discoveryDeclaresBigEndian3Byte(std::uint8_t byte22) noexcept
{
    return byte22 == 0 || (byte22 & 0x01) != 0;
}

//DH1KLM Parse a Discovery Reply (>=24 bytes: header through byte 23). Bounds-checks
//DH1KLM before indexing: this parses unauthenticated UDP.
std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> data) noexcept;

//DH1KLM ---- General Packet (spec p.18-22) ----

//DH1KLM 60-byte General Packet. Port fields (bytes 5-22) zero = defaults (p.19-20).
//DH1KLM Byte 37 bit[3] = 1 (phase word, matching Discovery byte 21). Byte 38 bit[0] = 1:
//DH1KLM hardware watchdog ON (p.8: without a C&C packet at least every second the
//DH1KLM radio drops out of RUN), which self-heals an abandoned session (a `kill -9`d
//DH1KLM client recovers cleanly). Byte 39 = 0x00 (BE + 3-byte, per byte 22 == 0).
std::array<std::uint8_t, 60> buildGeneral() noexcept;

//DH1KLM ---- DDC-Specific Packet (spec p.23-26) ----

//DH1KLM 1444-byte DDC-Specific packet enabling DDC0 only at `ddc0RateKsps`
//DH1KLM (48/96/192/384/768/1536, p.24). `numAdcs` = 2, the G2's phase-synchronous ADCs.
//DH1KLM `ditherEnabled`/`randomEnabled` set bytes 5/6 (bit N = ADCn, p.25) for BOTH
//DH1KLM ADCs in lockstep -- the radio exposes one control. `ddc0AdcIndex` sets byte 17
//DH1KLM (p.25): 0 = ADC0 behind the Ant1/2/3 relay bank, 1 = ADC1 on its own RX2 jack
//DH1KLM (Appendix D, p.90).
std::array<std::uint8_t, 1444> buildDdcSpecific(int ddc0RateKsps = 48, int numAdcs = 2,
                                                bool ditherEnabled = true,
                                                bool randomEnabled = true,
                                                int ddc0AdcIndex = 0) noexcept;

//DH1KLM ---- Multi-DDC ----
//DH1KLM Codec safety bound on DDC count: a real G2 reports 4 (p2app's DiscoveryReply[]
//DH1KLM hardcodes it, though gateware VNUMDDC is 10); callers still clamp to the
//DH1KLM Discovery value. The one-byte enable bitmap caps the format at 8.
inline constexpr int kMaxDdcs = 4;

//DH1KLM One DDC's slot in the DDC-Specific packet. `rateKsps` must be one of
//DH1KLM 48/96/192/384/768/1536 (p.24); `adcIndex` selects which ADC feeds it,
//DH1KLM same meaning as buildDdcSpecific()'s ddc0AdcIndex above.
struct DdcConfig {
    int rateKsps = 48;
    int adcIndex = 0;
};

//DH1KLM Multi-DDC form: enables DDC 0..N-1 (N capped at kMaxDdcs; excess IGNORED),
//DH1KLM setting byte-7 enable bits and one 6-byte row per DDC at 17 + 6*n (ADC select,
//DH1KLM 2-byte rate, CIC1/CIC2 = 0 "for future use", sample size). An EMPTY span
//DH1KLM disables all DDCs -- how a session stops streaming without teardown. The
//DH1KLM single-DDC overload delegates here, so there is one row-layout implementation.
std::array<std::uint8_t, 1444> buildDdcSpecific(std::span<const DdcConfig> ddcs,
                                                int numAdcs = 2,
                                                bool ditherEnabled = true,
                                                bool randomEnabled = true) noexcept;

//DH1KLM ---- High Priority to Hardware (spec p.31-34) ----

//DH1KLM One selection of a receive band-pass bank, named as the spec's SATURN/ANAN-G2
//DH1KLM Alex0 and Alex1 bit tables name it (p.91). Both words use the same bits, so
//DH1KLM one enum serves ADC0 and ADC1. The names are the spec's, not band names: the
//DH1KLM spec publishes the bits but not the passbands (see alexRxFilterFor()).
enum class AlexRxFilter {
    Bypass,      // bit 12, "HF Bypass": the bank out of circuit
    Bpf1_5MHz,   // bit 6
    Bpf6_5MHz,   // bit 5
    Bpf9_5MHz,   // bit 4
    Bpf13MHz,    // bit 1
    Bpf20MHz,    // bit 2
    Preamp6m,    // bit 3, "6M Preamp"
};

//DH1KLM The 16-bit receive word selecting `filter` alone. Exactly one bit is set: the
//DH1KLM bank's relays are in series ahead of the ADC (p.90).
[[nodiscard]] std::uint16_t alexRxFilterBits(AlexRxFilter filter) noexcept;

//DH1KLM The filter for a receiver tuned to `tunedHz`. The edges are deskHPSDR's for
//DH1KLM SATURN and ORION2 boards (new_protocol.c): 1.5, 2.1, 5.5, 11, 22 and 35 MHz.
//DH1KLM They are not derivable from the filter names, and a filter that does not
//DH1KLM cover the tuned frequency removes the signal path, so the selection stays
//DH1KLM opt-in. Below 1.5 MHz, and for a non-finite frequency, it is Bypass.
[[nodiscard]] AlexRxFilter alexRxFilterFor(double tunedHz) noexcept;

//DH1KLM The High Priority fields that apply live, beyond the bypass flags. Defaults
//DH1KLM reproduce the bytes this client sends with no options.
struct HighPriorityOptions {
    //DH1KLM A band-pass selection for that ADC's bank. It replaces the matching
    //DH1KLM bypass flag: the bank takes one relay, so bypass and a filter are
    //DH1KLM alternatives. nullopt leaves the bypass flag in charge.
    std::optional<AlexRxFilter> adc0Filter;
    std::optional<AlexRxFilter> adc1Filter;
    //DH1KLM Byte 1400 bit 1: mutes the radio's speaker amplifier (p2app SetSpkrMute()).
    //DH1KLM Sending silent speaker audio leaves the amplifier running and hissing.
    //DH1KLM Byte 1400's bit 0 (XVTR route on PTT) and bit 2 (tuner start) stay clear.
    bool muteSpeakerAmp = false;
};

//DH1KLM 1444-byte High Priority packet. RX-ONLY: `run` sets byte 4 bit[0] only and
//DH1KLM there is no PTT parameter. `ddc0FreqWord` sets bytes 9-12 (p.32). Must be
//DH1KLM re-sent at least once a second (100 ms recommended, p.8) to stay in RUN;
//DH1KLM P2Client owns that cadence.
//DH1KLM
//DH1KLM The "32-bit Alex0" at 1432-1435 is two 16-bit words to the radio: p2app reads
//DH1KLM 1432-1433 as the TX filter and antenna word and 1434-1435 as ADC0's receive
//DH1KLM filter word (InHighPriority.c); 1430-1431 is ADC1's receive word (Alex1). A
//DH1KLM receive filter written into 1432-1433 would reach the TX relays, not the ADC.
//DH1KLM
//DH1KLM Alex0 is always written: ANT1 (bit 24 of the 32-bit word), plus ADC0's
//DH1KLM receive selection. Power-on Alex is all zero (p.79) and the RX1 BPF bank sits
//DH1KLM in series before the ADC (p.90), so with neither a band filter nor Bypass
//DH1KLM energised there is no signal path. ANT2/3 are not exposed; RX2 is a single
//DH1KLM dedicated jack with no antenna select.
//DH1KLM
//DH1KLM `bypassAdc0Filters`/`bypassAdc1Filters` set HF Bypass (bit 12) in ADC0's and
//DH1KLM ADC1's receive words, unless `opts` selects a filter for that ADC.
//DH1KLM `adc0AttenuationDb`/`adc1AttenuationDb`: step attenuators, byte 1443 (ADC0) and
//DH1KLM 1442 (ADC1), 0-31 dB in 1 dB steps (p.34, p.36), clamped; 0 = none.
std::array<std::uint8_t, 1444> buildHighPriority(bool run, std::uint32_t ddc0FreqWord,
                                                  bool bypassAdc0Filters = true,
                                                  bool bypassAdc1Filters = true,
                                                  int adc0AttenuationDb = 0,
                                                  int adc1AttenuationDb = 0,
                                                  const HighPriorityOptions& opts = {}) noexcept;

//DH1KLM Multi-DDC form: one 4-byte frequency/phase word per DDC at 9 + 4*n (p.32),
//DH1KLM N capped at kMaxDdcs, excess ignored. Still RX-only (no PTT; byte 4 is bit[0]
//DH1KLM run only). Alex0/Alex1 are per-ADC, written as in the single-DDC overload.
std::array<std::uint8_t, 1444> buildHighPriority(bool run,
                                                  std::span<const std::uint32_t> ddcFreqWords,
                                                  bool bypassAdc0Filters = true,
                                                  bool bypassAdc1Filters = true,
                                                  int adc0AttenuationDb = 0,
                                                  int adc1AttenuationDb = 0,
                                                  const HighPriorityOptions& opts = {}) noexcept;

//DH1KLM The step attenuators' range (spec pp.34,36), shared by the encoder's clamp and
//DH1KLM by the backend that offers the control.
inline constexpr int kMaxStepAttenuationDb = 31;

//DH1KLM ---- DDC Audio, PC -> radio (spec p.36) ----
//DH1KLM The only outbound data path here. Fixed geometry: 64 stereo frames/packet,
//DH1KLM int16 big-endian L then R, 48 ksps -> 256 sample bytes + 4-byte BE sequence =
//DH1KLM 260 bytes every 1333 us. Independent of the DDC rate (this is demodulated
//DH1KLM audio); p2app DMAs fixed 256-byte blocks with no rate field.
inline constexpr int kSpeakerSampleRateHz = 48000;
inline constexpr int kSpeakerFramesPerPacket = 64;
inline constexpr int kSpeakerChannels = 2;
inline constexpr std::size_t kSpeakerSampleBytes = 256;  // 64 frames * 2ch * int16
inline constexpr std::size_t kSpeakerPacketBytes = 260;  // + seq(4)
inline constexpr int kSpeakerPacketIntervalUs = 1333;    // 64 / 48000

//DH1KLM Encode one speaker packet from host-order L,R,... int16. SHORT INPUT
//DH1KLM ZERO-FILLS (unlike parseDdcFrame()'s exact length): dropping the packet would
//DH1KLM stall the FIFO, and an unfilled tail would replay the previous 1.3 ms. Excess
//DH1KLM samples are ignored.
std::array<std::uint8_t, kSpeakerPacketBytes> buildSpeakerAudio(
    std::uint32_t sequence, std::span<const std::int16_t> interleavedLr) noexcept;

//DH1KLM ---- High Priority Status, radio -> PC (spec p.47) ----
//DH1KLM 60 bytes: 4-byte BE sequence, then hardware state. The two speaker-stream
//DH1KLM fields and the supply rail are decoded. Arrives on the same socket as DDC0 IQ (see
//DH1KLM kDdc0DefaultPort); parseDdcFrame() rejects it. p2app sends one every 200 ms
//DH1KLM while receiving (OutHighPriority.c).
inline constexpr std::size_t kHighPriorityStatusBytes = 60;

//DH1KLM Volts per count of HighPriorityStatus::supplyRailCounts: a (22+1)/1.1 divider
//DH1KLM into the 12-bit slow ADC at 5 V, as Thetis (console.cs convertToVolts) and
//DH1KLM deskHPSDR (rx_panadapter.c, Orion2/Saturn) scale the same input. Checked
//DH1KLM against a meter at one rail voltage on a G2: docs/architecture/radio-capabilities-map.md.
inline constexpr double kSupplyRailVoltsPerCount = (23.0 / 1.1) * (5.0 / 4095.0);

struct HighPriorityStatus {
    std::uint32_t seq = 0;
    //DH1KLM Byte 30 bit 3: the radio ran out of speaker audio. LATCHED by p2app since the
    //DH1KLM last status packet (~200 ms) and cleared on send.
    bool speakerUnderflow = false;
    //DH1KLM Bytes 37-38: speaker FIFO fill in FIFO LOCATIONS, not samples (p2app's
    //DH1KLM "2 samples per location" doubling never reaches the send, OutHighPriority.c).
    //DH1KLM Not comparable to kSpeakerFramesPerPacket; use as a trend.
    std::uint16_t speakerFifoLevel = 0;
    //DH1KLM Bytes 57-58: the DC supply rail in raw 12-bit counts. p2app labels it
    //DH1KLM "AIN3 user_analog1"; Thetis and deskHPSDR read the rail here. NOT bytes
    //DH1KLM 49-50 (AIN6), which the spec calls "supply voltage" but which fits no
    //DH1KLM published scale on a G2. No field in this payload is a temperature.
    std::uint16_t supplyRailCounts = 0;
};

//DH1KLM Decode, or nullopt if this is not a status packet. Bounds-checked
//DH1KLM (unauthenticated UDP); requires exactly 60 bytes. A Discovery reply is also 60 bytes
//DH1KLM (bytes 0-3 zero, byte 4 0x02/0x03) and can collide only at sequence 0, so
//DH1KLM callers must try the Discovery parse FIRST ON EVERY DATAGRAM, or a late or
//DH1KLM duplicate reply decodes as a phantom underflow (P2Client::handleDatagram()).
[[nodiscard]] std::optional<HighPriorityStatus> parseHighPriorityStatus(
    std::span<const std::uint8_t> data) noexcept;

//DH1KLM ---- DDC I&Q Data (spec p.53-54) ----

inline constexpr int kDdcHeaderLen = 16;   // seq(4) + timestamp(8) + bitsPerSample(2) + samplesPerFrame(2)
inline constexpr int kDdcSampleBytes = 6;  // 3-byte I + 3-byte Q

struct DdcFrame {
    std::uint32_t seq = 0;
    int samples = 0;
    //DH1KLM Exactly samples * kDdcSampleBytes bytes, big-endian 24-bit I then Q
    //DH1KLM per sample. Borrows from the `data` span passed to parseDdcFrame() --
    //DH1KLM must not outlive it.
    std::span<const std::uint8_t> iqRaw;
};

//DH1KLM Parse and STRICTLY VALIDATE a DDC I&Q datagram: nullopt unless bitsPerSample
//DH1KLM == 24 and data.size() == kDdcHeaderLen + samples * kDdcSampleBytes EXACTLY.
//DH1KLM Reject, never clamp: Mic Data (p.51, 64 mono samples, streamed whenever run=1)
//DH1KLM shares this port and would otherwise misdecode as tiny DDC frames with fake
//DH1KLM sequence gaps and sample rate.
std::optional<DdcFrame> parseDdcFrame(std::span<const std::uint8_t> data) noexcept;

//DH1KLM Decode one 24-bit signed big-endian I/Q sample pair (6 bytes) to normalized
//DH1KLM [-1, 1). BE only (see discoveryDeclaresBigEndian3Byte()).
std::complex<float> decodeIqSample(const std::uint8_t* be6) noexcept;

//DH1KLM Decode every sample in `frame.iqRaw` and append to `out`. Cannot fail --
//DH1KLM `frame` came from a successful parseDdcFrame(), whose exact-length check
//DH1KLM already guarantees `iqRaw.size() == frame.samples * kDdcSampleBytes`.
void decodeIq(const DdcFrame& frame, std::vector<std::complex<float>>& out);

}  // namespace AetherSDR::anan_original
