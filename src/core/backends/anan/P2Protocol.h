#pragma once

#include "core/backends/anan/P1Protocol.h"
#include "core/backends/anan/AnanModel.h"

#include <array>
#include "AnanReceiveFilterPlan.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// openHPSDR Ethernet Protocol 2 wire primitives for the ANAN-G2 backend; a port
// of the bench-validated anan/spike/phase1a.py spike (Saturn, protocol 4.3,
// gateware 27). Sources: openHPSDR Ethernet Protocol v4.4 (VK6PH, see
// THIRD_PARTY_LICENSES), Saturn FPGA RTL, and p2app, the radio-side server
// (provenance: docs/architecture/anan-p2-backend-design.md §2.9). Page numbers
// below are spec pages. Socket- and Qt-free; P2Client owns the sockets.
// RX-ONLY BY CONSTRUCTION: no DUC-Specific, TX drive or PTT encoder exists here
// (TX is RFC §2.11 Phase 3, behind the engine's TX arbiter). buildSpeakerAudio()
// is not transmit: it sends demodulated receiver audio to the radio's speaker
// codec, never reaches the DUC/PA/T-R relay, and cannot key anything.

namespace AetherSDR::anan {

inline constexpr std::uint16_t kRadioPort = 1024;

// The radio's fixed listening ports (spec p.19-20 default port table).
// Load-bearing: DDC-Specific and High-Priority carry no packet-type byte, so the
// radio identifies them by arrival port; sent to kRadioPort they are silently
// ignored by p2app.
inline constexpr std::uint16_t kDdcSpecificPort = 1025;
inline constexpr std::uint16_t kHighPriorityPort = 1027;

// The radio's listening port for DDC Audio -- demodulated receiver audio sent
// BACK to the radio so its own codec and speaker reproduce it (spec p.36; the
// default port table, p.19-20, indexes it 4). p2app receives it in
// IncomingSpkrAudio() and DMAs it to the speaker codec FIFO.
//
// Load-bearing for the same reason kDdcSpecificPort and kHighPriorityPort are:
// a speaker packet carries no type discriminator, only a 4-byte sequence, so
// the port IS the packet's identity. Sent anywhere else it is silently dropped.
inline constexpr std::uint16_t kSpeakerAudioPort = 1028;

// The port DDC0 IQ nominally originates FROM per the General Packet's byte
// 17-18 default (spec p.19-20). NOT load-bearing for where the PC receives
// it: Phase 1a measured DDC0 IQ arriving at the port the client's Discovery
// packet was SENT FROM, matching the spec's literal "Destination Port: the
// Source Port of the Host that initiated Discovery" rule for every radio->PC
// stream (p.43, p.51, p.54) -- not a fixed per-DDC port on the PC side.
// Kept here only as a documented fallback a client MAY also listen on; do
// not treat it as the primary demultiplexing port.
inline constexpr std::uint16_t kDdc0DefaultPort = 1035;

// Which DDC an inbound IQ datagram belongs to, by the port the RADIO sent it
// FROM; nullopt outside [basePort, basePort + numDdc) (Mic Data, HP Status and
// the Discovery reply share the socket, so that is ordinary). basePort + n
// matches p2app: generalpacket.c's SetPort(VPORTDDCIQ0+i, Port+i) for a non-zero
// General bytes 17-18, and DefaultPorts[] VPORTDDCIQ0..9 = 1035..1044 when zero
// (what buildGeneral() sends). The port is the ONLY DDC discriminator: the I&Q
// packet has no DDC index (pp.53-54), and p2app never emits the interleaved
// "Synchronous DDC" framing.
[[nodiscard]] std::optional<int> ddcIndexForSenderPort(
    std::uint16_t senderPort, int numDdc,
    std::uint16_t basePort = kDdc0DefaultPort) noexcept;

// The six DDC0 rates in ksps, ascending. DDC0 is stepped, not continuous
// (AnanBackend::nearestDdc0RateKsps() snaps zoom to these). The single source for
// capabilities, pan limits, AnanRxDsp's droop-table check, the calibrator sweep
// and ConnectionPanel's picker: a rate missing from any of them silently
// rejects that rate's correction tables.
inline constexpr std::array<int, 6> kDdc0RatesKsps{48, 96, 192, 384, 768, 1536};

// Saturn boards' DSP clock. RFC §2.4; saturnregisters.c VSAMPLERATE.
inline constexpr std::uint32_t kDspClockHz = 122'880'000;

// 24-bit signed full scale. (1<<23)-1, not 1<<23 -- see MetisProtocol.h's
// kFullScale for the reasoning (the largest magnitude a 24-bit two's
// complement sample can actually take, matching the reference clients'
// dBFS scale rather than landing a fraction of a dB adrift).
inline constexpr int kFullScale24Bit = (1 << 23) - 1;

// Phase word: delta = 2^32 * F / Fdsp (spec p.21, p.44; RFC §2.4). This
// board REQUIRES phase words, not Hz -- Discovery Reply byte 21 == 1 on
// every G2 observed -- and General Packet byte 37 bit[3] must agree
// (buildGeneral() sets it). Verified in the field: phaseWord(10e6) tuned
// DDC0 to WWV and the carrier read back at 0 Hz baseband, 80.0 dB above the
// noise floor.
constexpr std::uint32_t phaseWord(double freqHz, std::uint32_t dspClockHz = kDspClockHz) noexcept
{
    constexpr double kTwoTo32 = 4294967296.0;
    const double delta = kTwoTo32 * freqHz / static_cast<double>(dspClockHz);
    // Round to nearest. freqHz is always non-negative for a receive tune, so
    // there is no negative-rounding case to handle here.
    return static_cast<std::uint32_t>(delta + 0.5);
}

// ---- Discovery (spec p.42-45) ----

// 60-byte discovery request: seq(4)=0, byte 4 = 0x02 (Discovery Command).
std::array<std::uint8_t, 60> buildDiscovery() noexcept;

struct DiscoveryReply {
    //DH1KLM: Normalized identity can originate from either discovery format.
    int wireProtocol = 2;
    bool streaming = false;           // byte 4 == 0x03: hardware already has a session
    std::array<std::uint8_t, 6> mac{};// bytes 5-10, MSB first (p.43)
    std::uint8_t boardId = 0;         // byte 11; 10 = SATURN (ANAN-G2)
    std::uint8_t protoVersionRaw = 0; //DH1KLM: Opaque byte 12, Thetis ProtocolSupported.
                                       // Preserve raw values (e.g. 2 or 43); never gate discovery on a version range.
    std::uint8_t firmwareVer = 0;     // byte 13, an INTEGER gateware/build number, NOT
                                       // tenths despite the spec's worked example implying
                                       // one -- p2app writes GetFirmwareVersion() here
    std::uint8_t reportedReceivers = 0; // byte 20: Thetis calls this NumRxs.
                                         // Saturn's current client uses it as a conservative
                                         // active-stream/DDC ceiling, but it is NOT a universal
                                         // physical DDC-count field for other board families.
    bool freqIsPhaseWord = false;     // byte 21: 0 = Hz, 1 = phase word
    std::uint8_t endianByteRaw = 0;   // byte 22, RAW -- see discoveryDeclaresBigEndian3Byte()
    std::uint8_t p2appBuild = 0;      // byte 23. "Beta version" per spec; p2app overwrites
                                       // it with P2APPVERSION on Saturn

    //DH1KLM: Discovery accepts the complete Thetis HPSDRHW board-ID set.
    // The numeric values below come from Thetis enums.cs / ChannelMaster/network.h
    // and NereusSDR HpsdrModel.h. Model-specific operation still requires the
    // matching hardware profile; recognizing a board is not proof that its
    // ADC/DDC/filter behavior is interchangeable with Saturn.
    [[nodiscard]] constexpr bool isSupportedOpenHpsdrBoard() const noexcept
    {
        switch (boardId) {
        case 0:  // Atlas / Metis kit
        case 1:  // Hermes; ANAN-10 / ANAN-100
        case 2:  // Hermes II; ANAN-10E / ANAN-100B
        case 3:  // Angelia; ANAN-100D
        case 4:  // Orion; ANAN-200D
        case 5:  // Orion MkII family; exact SKU can be ambiguous
        case 6:  // Hermes Lite 2
        case 10: // Saturn; ANAN-G2 / G2 1K
        case 11: // Saturn MkII identifier used by NereusSDR
        case 20: // HermesC10; ANAN-G2E in Thetis v2.10.3.15
            return true;
        default:
            return false;
        }
    }

    //DH1KLM: Board ID 5 is shared by these Thetis model selections. Discovery
    // cannot distinguish the logical SKU, so present the documented family and
    // do not silently choose ANAN-7000DLE, ANAN-8000DLE, Anvelina, or Red Pitaya.
    [[nodiscard]] constexpr const char* boardDisplayName() const noexcept
    {
        switch (boardId) {
        case 0:  return "Atlas / Metis";
        case 1:  return "Hermes (ANAN-10 / ANAN-100)";
        case 2:  return "Hermes II (ANAN-10E / ANAN-100B)";
        case 3:  return "Angelia (ANAN-100D)";
        case 4:  return "Orion (ANAN-200D)";
        case 5:  return "Orion MkII (ANAN-7000DLE/8000DLE, Anvelina Pro 3, Red Pitaya)";
        case 6:  return "Hermes Lite 2";
        case 10: return "Saturn (ANAN-G2 / ANAN-G2 1K)";
        case 11: return "Saturn MkII";
        case 20: return "HermesC10 (ANAN-G2E)";
        default: return "Unknown OpenHPSDR board";
        }
    }
};

//DH1KLM: Normalize P1 wire board IDs into the existing Thetis/P2 registry.
[[nodiscard]] std::optional<DiscoveryReply> parseOpenHpsdrDiscoveryReply(
    std::span<const std::uint8_t> data) noexcept;

//DH1KLM: The ANAN-7000DLE capture (2026-10-17) answers directed P2
// discovery from UDP port 0. The caller must still match the requested IP;
// accept this observed P2 endpoint only after validating the reply bytes.
[[nodiscard]] inline std::optional<DiscoveryReply> parseDirectedDiscoveryReply(
    std::span<const std::uint8_t> data, std::uint16_t sourcePort) noexcept
{
    const auto reply = parseOpenHpsdrDiscoveryReply(data);
    if (!reply || (sourcePort != kRadioPort
                   && !(sourcePort == 0 && reply->wireProtocol == 2)))
        return std::nullopt;
    return reply;
}

//DH1KLM: Physical RX layout from Thetis console.cs UpdateDDCs/GetDDC and
// clsHardwareSpecific.cs. Saturn keeps its bench-qualified DDC0 path;
// Orion MkII (ANAN-7000DLE target) receives on DDC2 without diversity/PS.
//DH1KLM: Legacy ALEX HPFs and MkII BPFs have different frequency tables.
enum class ReceiveFilterKind { MkIIBpf, LegacyHpf };

struct ReceiveProfile {
    int numAdcs;
    int firstDdcIndex;
    int alexEnableMask;
    bool selectReceiveLpf;
    bool saturnDroopDefaults;
    ReceiveFilterKind filterKind = ReceiveFilterKind::MkIIBpf;
    int physicalDdcLimit = 4;
    bool swapSpeakerChannels = false;
    bool disablePa = false;
};

[[nodiscard]] constexpr std::optional<ReceiveProfile> receiveProfile(int boardId) noexcept
{
    switch (boardId) {
    case 1: //DH1KLM: Thetis Hermes/ANAN-10/100; normal RX1 is DDC0.
        return ReceiveProfile{1, 0, 3, true, false, ReceiveFilterKind::LegacyHpf, 4, true, true};
    case 2: //DH1KLM: Hermes II has only two physical DDC slots.
        return ReceiveProfile{1, 0, 3, true, false, ReceiveFilterKind::LegacyHpf, 2, true, true};
    case 3: //DH1KLM: Angelia/ANAN-100D uses legacy ALEX with RX1 on DDC2.
    case 4: //DH1KLM: Orion/ANAN-200D has P2 ID 4, distinct from P1 ID 5.
        return ReceiveProfile{2, 2, 3, true, false, ReceiveFilterKind::LegacyHpf, 7, false, true};
    case 5:
        return ReceiveProfile{2, 2, 3, true, false, ReceiveFilterKind::MkIIBpf, 7, false, true};
    case 10:
        return ReceiveProfile{2, 0, 0, false, true, ReceiveFilterKind::MkIIBpf, 7};
    case 20: //DH1KLM: ANAN-G2E/HermesC10 is single-ADC with MkII filtering.
        return ReceiveProfile{1, 0, 3, true, false, ReceiveFilterKind::MkIIBpf, 4, false, true};
    default:
        return std::nullopt;
    }
}

//DH1KLM: A second ADC on legacy ALEX does not imply a MkII Alex-2 filter board.
[[nodiscard]] constexpr bool hasAlex2Filters(const ReceiveProfile& profile) noexcept
{
    return profile.numAdcs > 1 && profile.filterKind == ReceiveFilterKind::MkIIBpf;
}

//DH1KLM: Keep the physical wire layout separate from the operator's logical
// model. Red Pitaya uses the Orion-class DDC/ADC layout even when it reports Hermes.
[[nodiscard]] constexpr std::optional<ReceiveProfile> receiveProfile(
    int boardId, AnanModel model, bool overrideDetection = false) noexcept
{
    //DH1KLM: Zeus-style override changes the entire effective hardware profile.
    // It never grants support to unknown boards or to the dedicated HL2 backend.
    const auto* descriptor = ananModelInfo(model);
    if (!descriptor || !receiveProfile(boardId)
        || (overrideDetection && model == AnanModel::Automatic)
        || (!overrideDetection && !modelMatchesBoard(model, boardId))) {
        return std::nullopt;
    }
    if (model == AnanModel::RedPitaya) {
        return ReceiveProfile{2, 2, 3, true, false, ReceiveFilterKind::LegacyHpf, 7, false, true};
    }
    return receiveProfile(model == AnanModel::Automatic ? boardId : descriptor->nativeBoardId);
}

//DH1KLM: Recognition alone never authorizes a session. The physical reply
// must have a receive profile and match picker metadata when supplied.
[[nodiscard]] constexpr bool discoveryMatchesVerifiedAnanProfile(
    std::uint8_t boardId, int expectedBoardId) noexcept
{
    return receiveProfile(boardId).has_value()
        && (expectedBoardId < 0 || expectedBoardId == static_cast<int>(boardId));
}

// Byte 22 per spec p.45: ZERO means big-endian 3-byte I&Q (not "none", not
// little-endian); nonzero is a bitmask with bit 0 = BE. A real G2 sends 0 and
// decodes BE at 80.0 dB SNR vs 26.0 dB byte-swapped (WWV 10 MHz); buildGeneral()
// requests this via byte 39.
[[nodiscard]] constexpr bool discoveryDeclaresBigEndian3Byte(std::uint8_t byte22) noexcept
{
    return byte22 == 0 || (byte22 & 0x01) != 0;
}

// Parse a Discovery Reply (>=24 bytes: header through byte 23). Bounds-checks
// before indexing: this parses unauthenticated UDP.
std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> data) noexcept;

// ---- General Packet (spec p.18-22) ----

// 60-byte General Packet. Port fields (bytes 5-22) zero = defaults (p.19-20).
// Byte 37 bit[3] = 1 (phase word, matching Discovery byte 21). Byte 38 bit[0] = 1:
// hardware watchdog ON (p.8: without a C&C packet at least every second the
// radio drops out of RUN), which self-heals an abandoned session (a `kill -9`d
// client recovers cleanly). Byte 39 = 0x00 (BE + 3-byte, per byte 22 == 0).
std::array<std::uint8_t, 60> buildGeneral(int alexEnableMask = 0,
                                       bool disablePa = false) noexcept;

// ---- DDC-Specific Packet (spec p.23-26) ----

// 1444-byte DDC-Specific packet enabling DDC0 only at `ddc0RateKsps`
// (48/96/192/384/768/1536, p.24). `numAdcs` = 2, the G2's phase-synchronous ADCs.
// `ditherEnabled`/`randomEnabled` set bytes 5/6 (bit N = ADCn, p.25)
// for the physically present ADCs only (0x01 for one ADC, 0x03 for two).
// `ddc0AdcIndex` sets byte 17
// (p.25): 0 = ADC0 behind the Ant1/2/3 relay bank, 1 = ADC1 on its own RX2 jack
// (Appendix D, p.90).
std::array<std::uint8_t, 1444> buildDdcSpecific(int ddc0RateKsps = 48, int numAdcs = 2,
                                                bool ditherEnabled = true,
                                                bool randomEnabled = true,
                                                int ddc0AdcIndex = 0) noexcept;

// ---- Multi-DDC ----
// Physical DDC0..6 cover the seven-slot dual-ADC gateware layout.
// Each validated board profile and the discovery count further bound a session.
inline constexpr int kMaxDdcs = 7;

// One DDC's slot in the DDC-Specific packet. `rateKsps` must be one of
// 48/96/192/384/768/1536 (p.24); `adcIndex` selects which ADC feeds it,
// same meaning as buildDdcSpecific()'s ddc0AdcIndex above.
struct DdcConfig {
    int rateKsps = 48;
    int adcIndex = 0;
};

//DH1KLM: Enables physical DDC firstDdcIndex..firstDdcIndex+N-1,
// capped at kMaxDdcs (excess ignored),
// setting byte-7 enable bits and one 6-byte row per DDC at 17 + 6*n (ADC select,
// 2-byte rate, CIC1/CIC2 = 0 "for future use", sample size). An EMPTY span
// disables all DDCs -- how a session stops streaming without teardown. The
// single-DDC overload delegates here, so there is one row-layout implementation.
std::array<std::uint8_t, 1444> buildDdcSpecific(std::span<const DdcConfig> ddcs,
                                                int numAdcs = 2,
                                                bool ditherEnabled = true,
                                                bool randomEnabled = true,
                                                int firstDdcIndex = 0) noexcept;

// ---- High Priority to Hardware (spec p.31-34) ----

// 1444-byte High Priority packet. RX-ONLY: `run` sets byte 4 bit[0] only and
// there is no PTT parameter. `ddc0FreqWord` sets bytes 9-12 (p.32). Must be
// re-sent at least once a second (100 ms recommended, p.8) to stay in RUN;
// P2Client owns that cadence.
// Alex0 (bytes 1432-1435, p.35, p.90-91) is always written: RX antenna
// selection ANT1/2/3 (bits 24/25/26) plus Thetis' Saturn BPF1 relay selected
// from DDC0's phase word. Invalid antenna values fall back to ANT1. The explicit
// `bypassAdc0Filters` option forces bypass; frequencies outside Thetis' configured
// 1.5-61.44 MHz rows also bypass. Orion MkII shares this BPF1 relay map.
// `bypassAdc1Filters` sets bit 12
// ("HF Bypass 2") of Alex1 (bytes 1430-1431); RX2 is a dedicated jack.
// `adc0AttenuationDb`/`adc1AttenuationDb`: step attenuators, byte 1443 (ADC0) and
// 1442 (ADC1), 0-31 dB in 1 dB steps (p.34, p.36), clamped; 0 = none.
std::array<std::uint8_t, 1444> buildHighPriority(bool run, std::uint32_t ddc0FreqWord,
                                                  bool bypassAdc0Filters = false,
                                                  bool bypassAdc1Filters = true,
                                                  int adc0AttenuationDb = 0,
                                                  int adc1AttenuationDb = 0,
                                                  int rxAntenna = 1) noexcept;

// Multi-DDC form: one 4-byte frequency/phase word per DDC at 9 + 4*n (p.32),
//DH1KLM: firstDdcIndex offsets physical tuning rows. selectReceiveLpf
// enables Thetis LPF selection for Orion MkII only. Still RX-only (byte 4 bit[0]
// run only). LPF-enabled profiles also mirror the antenna/LPF relay word into
// Alex1 bytes 1428-1429, independently of its ADC1 receive-filter low word.
std::array<std::uint8_t, 1444> buildHighPriority(bool run,
                                                  std::span<const std::uint32_t> ddcFreqWords,
                                                  bool bypassAdc0Filters = false,
                                                  bool bypassAdc1Filters = true,
                                                  int adc0AttenuationDb = 0,
                                                  int adc1AttenuationDb = 0,
                                                  int rxAntenna = 1,
                                                  int firstDdcIndex = 0,
                                                  bool selectReceiveLpf = false,
                                                  ReceiveFilterKind filterKind = ReceiveFilterKind::MkIIBpf,
                                                  int numAdcs = 2,
                                                  const FilterConfig* filters = nullptr,
                                                  int alexAttenuationDb = 0,
                                                  const ReceiveFilterPlan* filterPlan = nullptr,
                                                  double frequencyCorrection = 1.0,
                                                  bool muteSpeakerAmp = false) noexcept;

// The step attenuators' range (spec pp.34,36), shared by the encoder's clamp and
// by the backend that offers the control.
inline constexpr int kMaxStepAttenuationDb = 31;

// ---- DDC Audio, PC -> radio (spec p.36) ----
// The only outbound data path here. Fixed geometry: 64 stereo frames/packet,
// int16 big-endian L then R, 48 ksps -> 256 sample bytes + 4-byte BE sequence =
// 260 bytes every 1333 us. Independent of the DDC rate (this is demodulated
// audio); p2app DMAs fixed 256-byte blocks with no rate field.
inline constexpr int kSpeakerSampleRateHz = 48000;
inline constexpr int kSpeakerFramesPerPacket = 64;
inline constexpr int kSpeakerChannels = 2;
inline constexpr std::size_t kSpeakerSampleBytes = 256;  // 64 frames * 2ch * int16
inline constexpr std::size_t kSpeakerPacketBytes = 260;  // + seq(4)
inline constexpr int kSpeakerPacketIntervalUs = 1333;    // 64 / 48000

// Encode one speaker packet from host-order L,R,... int16. SHORT INPUT
// ZERO-FILLS (unlike parseDdcFrame()'s exact length): dropping the packet would
// stall the FIFO, and an unfilled tail would replay the previous 1.3 ms. Excess
// samples are ignored.
std::array<std::uint8_t, kSpeakerPacketBytes> buildSpeakerAudio(
    std::uint32_t sequence, std::span<const std::int16_t> interleavedLr,
    bool swapChannels = false) noexcept;

// ---- High Priority Status, radio -> PC (spec p.47) ----
// 60 bytes: 4-byte BE sequence, then hardware state. The two speaker-stream
// fields and the supply rail are decoded. Arrives on the same socket as DDC0 IQ (see
// kDdc0DefaultPort); parseDdcFrame() rejects it. p2app sends one every 200 ms
// while receiving (OutHighPriority.c).
inline constexpr std::size_t kHighPriorityStatusBytes = 60;

// Volts per count of HighPriorityStatus::supplyRailCounts: a (22+1)/1.1 divider
// into the 12-bit slow ADC at 5 V, as Thetis (console.cs convertToVolts) and
// deskHPSDR (rx_panadapter.c, Orion2/Saturn) scale the same input. Checked
// against a meter at one rail voltage on a G2: docs/architecture/radio-capabilities-map.md.
inline constexpr double kSupplyRailVoltsPerCount = (23.0 / 1.1) * (5.0 / 4095.0);

struct HighPriorityStatus {
    std::uint32_t seq = 0;
    // Byte 30 bit 3: the radio ran out of speaker audio. LATCHED by p2app since the
    // last status packet (~200 ms) and cleared on send.
    bool speakerUnderflow = false;
    // Bytes 37-38: speaker FIFO fill in FIFO LOCATIONS, not samples (p2app's
    // "2 samples per location" doubling never reaches the send, OutHighPriority.c).
    // Not comparable to kSpeakerFramesPerPacket; use as a trend.
    std::uint16_t speakerFifoLevel = 0;
    // Bytes 57-58: the DC supply rail in raw 12-bit counts. p2app labels it
    // "AIN3 user_analog1"; Thetis and deskHPSDR read the rail here. NOT bytes
    // 49-50 (AIN6), which the spec calls "supply voltage" but which fits no
    // published scale on a G2. No field in this payload is a temperature.
    std::uint16_t supplyRailCounts = 0;
};

// Decode, or nullopt if this is not a status packet. Bounds-checked
// (unauthenticated UDP); requires exactly 60 bytes. A Discovery reply is also 60 bytes
// (bytes 0-3 zero, byte 4 0x02/0x03) and can collide only at sequence 0, so
// callers must try the Discovery parse FIRST ON EVERY DATAGRAM, or a late or
// duplicate reply decodes as a phantom underflow (P2Client::handleDatagram()).
[[nodiscard]] std::optional<HighPriorityStatus> parseHighPriorityStatus(
    std::span<const std::uint8_t> data) noexcept;

// ---- DDC I&Q Data (spec p.53-54) ----

inline constexpr int kDdcHeaderLen = 16;   // seq(4) + timestamp(8) + bitsPerSample(2) + samplesPerFrame(2)
inline constexpr int kDdcSampleBytes = 6;  // 3-byte I + 3-byte Q

struct DdcFrame {
    std::uint32_t seq = 0;
    int samples = 0;
    // Exactly samples * kDdcSampleBytes bytes, big-endian 24-bit I then Q
    // per sample. Borrows from the `data` span passed to parseDdcFrame() --
    // must not outlive it.
    std::span<const std::uint8_t> iqRaw;
};

// Parse and STRICTLY VALIDATE a DDC I&Q datagram: nullopt unless bitsPerSample
// == 24 and data.size() == kDdcHeaderLen + samples * kDdcSampleBytes EXACTLY.
// Reject, never clamp: Mic Data (p.51, 64 mono samples, streamed whenever run=1)
// shares this port and would otherwise misdecode as tiny DDC frames with fake
// sequence gaps and sample rate.
std::optional<DdcFrame> parseDdcFrame(std::span<const std::uint8_t> data) noexcept;

// Decode one 24-bit signed big-endian I/Q sample pair (6 bytes) to normalized
// [-1, 1). BE only (see discoveryDeclaresBigEndian3Byte()).
std::complex<float> decodeIqSample(const std::uint8_t* be6) noexcept;

// Decode every sample in `frame.iqRaw` and append to `out`. Cannot fail --
// `frame` came from a successful parseDdcFrame(), whose exact-length check
// already guarantees `iqRaw.size() == frame.samples * kDdcSampleBytes`.
void decodeIq(const DdcFrame& frame, std::vector<std::complex<float>>& out);

}  // namespace AetherSDR::anan
