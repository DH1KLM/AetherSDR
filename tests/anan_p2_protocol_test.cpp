#include "core/backends/anan/AnanRfPower.h"
// aetherd ANAN P2 Phase 1b -- P2Protocol unit test. Pins the openHPSDR
// Protocol 2 wire encoding/decoding ported from the live-validated
// anan/spike/phase1a.py spike, run against a real ANAN-G2 on the bench:
// phase-word conversion, Discovery/General/DDC-Specific/High-Priority packet
// encoding, the strict DDC-frame shape validator, and the 24-bit signed
// big-endian IQ decode. Pure protocol -- no sockets, no hardware.

#include "core/backends/anan/P2Protocol.h"
#include "core/backends/anan/AnanSliceAudio.h"
#include "core/backends/anan/AnanSpeakerPacing.h"
#include "core/backends/anan/AnanDiscoveryPolicy.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// Encode a 24-bit signed value big-endian into three bytes.
static void put24be(std::uint8_t* p, std::int32_t v)
{
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>(v & 0xFF);
}

// Build a well-formed DDC I&Q datagram: 16-byte header + `n` BE 3-byte I/Q
// sample pairs, each sample n at I = 100*n, Q = -(100*n + 1).
static std::vector<std::uint8_t> makeDdcFrame(std::uint32_t seq, int n)
{
    std::vector<std::uint8_t> pkt(static_cast<std::size_t>(kDdcHeaderLen + n * kDdcSampleBytes), 0);
    pkt[0] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[1] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[3] = static_cast<std::uint8_t>(seq & 0xFF);
    // bytes 4-11: timestamp, left zero (unused by this layer)
    pkt[12] = 0x00; pkt[13] = 24;                 // bitsPerSample = 24
    pkt[14] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
    pkt[15] = static_cast<std::uint8_t>(n & 0xFF); // samplesPerFrame = n
    for (int i = 0; i < n; ++i) {
        std::uint8_t* s = pkt.data() + kDdcHeaderLen + i * kDdcSampleBytes;
        put24be(s, 100 * i);
        put24be(s + 3, -(100 * i + 1));
    }
    return pkt;
}

int main()
{
    for (const auto& info : kAnanModels) {
        const bool supported = info.model == AnanModel::Anan7000 || info.model == AnanModel::Anan8000
            || info.model == AnanModel::AnvelinaPro3 || info.model == AnanModel::RedPitaya
            || info.model == AnanModel::AnanG2 || info.model == AnanModel::AnanG21K;
        check(hasSpeakerAmpMute(2, info.model, info.nativeBoardId, false) == supported,
              "P2 amplifier eligibility follows the explicit Thetis model list");
        check(!hasSpeakerAmpMute(1, info.model, info.nativeBoardId, true),
              "P1 never encodes the P2 amplifier control");
        check(!hasSpeakerAmpMute(2, info.model, 11, true),
              "unqualified board 11 never enables amplifier control");
    }
    check(hasSpeakerAmpMute(2, AnanModel::Automatic, 10, false)
          && !hasSpeakerAmpMute(2, AnanModel::Automatic, 5, false),
          "automatic Saturn is identified but board 5 needs a specific amplifier model");
    check(!hasSpeakerAmpMute(2, AnanModel::Anan7000, 3, false)
          && hasSpeakerAmpMute(2, AnanModel::Anan7000, 3, true),
          "only an explicit hardware override permits mismatched supported model wiring");
    // Patroclus RadioProfile.CreateDiscoveryResponse at 8c275223:
    // every emulated profile shares MAC 00:00:00:00:00:01; P2 byte 12 is 2.
    // Feed its wire shape through the production parser, not a fake radio.
    {
        std::array<std::uint8_t, 60> p1{};
        p1[0] = 0xef; p1[1] = 0xfe; p1[2] = 2;
        p1[8] = 1; p1[9] = 30; p1[10] = 10;
        const auto previous = parseOpenHpsdrDiscoveryReply(p1);
        for (const std::uint8_t board : {1, 2, 3, 4, 5, 10, 20}) {
            std::array<std::uint8_t, 60> p2{};
            p2[4] = 2; p2[10] = 1; p2[11] = board; p2[12] = 2;
            p2[13] = 30; p2[20] = 7; p2[21] = 1;
            const auto next = parseOpenHpsdrDiscoveryReply(p2);
            check(next && next->wireProtocol == 2 && next->boardId == board
                  && next->protoVersionRaw == 2 && previous && next->mac == previous->mac,
                  "Patroclus P2 profile is accepted and remains P2 despite a preceding same-MAC P1 profile");
            check(parseDirectedDiscoveryReply(p2, 1024).has_value(), "directed Patroclus P2 discovery accepted");
            check(next && previous && acceptsDiscoveryProtocolUpdate(previous->wireProtocol, next->wireProtocol, 2, 0),
                  "default P2 preference immediately replaces stale same-MAC P1 row");
            check(next && !acceptsDiscoveryProtocolUpdate(1, next->wireProtocol, 1, 0)
                  && acceptsDiscoveryProtocolUpdate(1, next->wireProtocol, 1, 2),
                  "saved P1 preference is retained but can age out when only P2 answers");
        }
    }
    for (const int preferred : {1, 2}) {
        const int other = 3 - preferred;
        check(!acceptsDiscoveryProtocolUpdate(preferred, other, preferred, 0),
              "second format cannot duplicate/downgrade a responding preferred interface");
        check(!acceptsDiscoveryProtocolUpdate(preferred, other, preferred, 1),
              "one missed preferred sweep is tolerated");
        check(acceptsDiscoveryProtocolUpdate(preferred, other, preferred, 2),
              "protocol preference ages out after firmware change");
        check(acceptsDiscoveryProtocolUpdate(other, preferred, preferred, 0),
              "explicit Red Pitaya P1 or default P2 preference wins");
    }
    // The same wire ID has different meanings in P1 and P2.
    {
        std::array<std::uint8_t, 60> legacy{};
        legacy[0] = 0xef; legacy[1] = 0xfe; legacy[2] = 2;
        legacy[3] = 0x88; legacy[8] = 0x6a; legacy[9] = 27;
        for (const auto& [wire, normalized] : std::array<std::pair<int, int>, 8>{{
            {0, 0}, {1, 1}, {2, 2}, {4, 3}, {5, 4}, {6, 6}, {10, 5}, {20, 20}}}) {
            legacy[10] = static_cast<std::uint8_t>(wire);
            const auto parsed = parseOpenHpsdrDiscoveryReply(legacy);
            check(parsed && parsed->wireProtocol == 1 && parsed->boardId == normalized,
                  "P1 IDs normalize without borrowing P2 meanings");
        }
        legacy[10] = 3;
        check(!parseOpenHpsdrDiscoveryReply(legacy), "unknown P1 ID cannot become P2 Angelia");
        std::array<std::uint8_t, 60> modern{};
        modern[4] = 2; modern[5] = 0x88; modern[10] = 0x6a;
        modern[11] = 10; modern[12] = 43; modern[13] = 27;
        const auto parsed = parseOpenHpsdrDiscoveryReply(modern);
        check(parsed && parsed->wireProtocol == 2 && parsed->boardId == 10,
              "P2 ID 10 remains Saturn, not P1 Orion MkII");
    }
    // ---- phase word: delta = 2^32 * F / Fdsp ----
    {
        check(phaseWord(10'000'000.0) == 0x14D55555u,
              "phaseWord(10 MHz) matches the field-verified value");
        // Round-trip within 1 Hz.
        const double back = static_cast<double>(phaseWord(10'000'000.0))
                           * static_cast<double>(kDspClockHz) / 4294967296.0;
        check(std::fabs(back - 10'000'000.0) < 1.0, "phase word round-trips to within 1 Hz");
        check(phaseWord(0.0) == 0, "0 Hz -> phase word 0");
    }

    // ---- discovery request ----
    {
        const auto disc = buildDiscovery();
        check(disc.size() == 60, "discovery request is 60 bytes");
        check(disc[4] == 0x02, "discovery request byte 4 = 0x02 (Discovery Command)");
        bool restZero = true;
        for (std::size_t i = 0; i < disc.size(); ++i)
            if (i != 4 && disc[i] != 0) { restZero = false; break; }
        check(restZero, "discovery request has no other bytes set");
    }

    // ---- discovery reply parse, against the REAL captured bytes ----
    // anan/reference/notes/captures/hpsdr_discovery/hpsdrDiscovery.txt --
    // ANAN-G2, board type 10 (SATURN), protocol 4.3, firmware/gateware 27,
    // 4 DDCs, phase word, BE+3-byte (byte22==0), p2app build 46.
    {
        const std::array<std::uint8_t, 32> raw{
            0x00, 0x00, 0x00, 0x00, 0x02, 0x88, 0xA2, 0x9E, 0x6D, 0x9F, 0x6A, 0x0A,
            0x2B, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x2E,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        const auto r = parseDiscoveryReply(raw);
        check(r.has_value(), "real captured discovery reply parses");
        check(r && !r->streaming, "byte4=0x02 -> not streaming");
        check(r && r->mac[0] == 0x88 && r->mac[1] == 0xA2 && r->mac[2] == 0x9E
              && r->mac[3] == 0x6D && r->mac[4] == 0x9F && r->mac[5] == 0x6A,
              "MAC parsed (88:A2:9E:6D:9F:6A, matching the capture)");
        check(r && r->boardId == 10, "board id 10 (SATURN / ANAN-G2)");
        check(r && r->isSupportedOpenHpsdrBoard(),
              "board id 10 is in the recognized OpenHPSDR board registry");
        check(r && std::string(r->boardDisplayName()).find("Saturn") != std::string::npos,
              "board id 10 has the Saturn family display name");
        check(r && r->protoVersionRaw == 0x2B, "ProtocolSupported raw byte 43 is preserved");
        check(r && r->firmwareVer == 27,
              "firmware is the INTEGER 27, not tenths (discrepancy #1)");
        check(r && r->reportedReceivers == 4, "4 hardware receivers reported in discovery byte 20");
        check(r && r->freqIsPhaseWord, "byte21=1 -> phase word, not Hz");
        check(r && r->endianByteRaw == 0, "byte22 raw is 0");
        check(r && discoveryDeclaresBigEndian3Byte(r->endianByteRaw),
              "byte22==0 decodes as BE+3-byte declared (spec p.45 sentinel), "
              "NOT little-endian -- the hpsdr_discover.py truthiness bug must never "
              "regress into this codec");
        check(r && r->p2appBuild == 46,
              "byte23 is p2app's build number on Saturn, not a beta flag");

        // Bounds-check: a short reply must not be read out of bounds.
        const std::array<std::uint8_t, 10> shortReply{};
        check(!parseDiscoveryReply(shortReply).has_value(),
              "too-short reply rejected, not read OOB");
        // Non-discovery bytes rejected.
        const std::array<std::uint8_t, 24> junk{0x01, 0x02, 0x03, 0x04};
        check(!parseDiscoveryReply(junk).has_value(), "non-zero seq bytes rejected");
        const std::array<std::uint8_t, 60> zeroMacReply{
            0x00, 0x00, 0x00, 0x00, 0x02
        };
        check(!parseDiscoveryReply(zeroMacReply).has_value(),
              "status-shaped packet with zero MAC cannot validate a Discovery session");
        for (const std::uint8_t supported : {0, 1, 2, 43, 255}) {
            auto opaqueProtocol = raw;
            opaqueProtocol[12] = supported;
            const auto decoded = parseOpenHpsdrDiscoveryReply(opaqueProtocol);
            check(decoded && decoded->wireProtocol == 2 && decoded->protoVersionRaw == supported,
                  "Thetis ProtocolSupported is opaque and cannot misclassify a P2-shaped reply as P1");
        }

        // Keep the P2 physical-board registry pinned independently
        // from the implemented receive-profile gate in AnanDiscovery.
        // These are Thetis/Nereus HPSDRHW IDs, not the different P1 byte map.
        constexpr std::array<std::uint8_t, 10> supportedBoardIds{
            0, 1, 2, 3, 4, 5, 6, 10, 11, 20
        };
        for (const std::uint8_t id : supportedBoardIds) {
            auto other = raw;
            other[11] = id;
            const auto parsed = parseDiscoveryReply(other);
            check(parsed && parsed->isSupportedOpenHpsdrBoard(),
                  "known P2 board ID is recognized");
            check(parsed && std::string(parsed->boardDisplayName()) != "Unknown OpenHPSDR board",
                  "known P2 board ID has a display name");
        }
        auto unknown = raw;
        unknown[11] = 99;
        const auto unknownReply = parseDiscoveryReply(unknown);
        check(unknownReply && !unknownReply->isSupportedOpenHpsdrBoard(),
              "unknown P2 board ID is not classified as a supported board");

        // A board's presence in the registry must not grant it the Saturn
        // session profile. Unknown/manual targets still need an implemented RX profile.
        check(discoveryMatchesVerifiedAnanProfile(10, -1),
              "manual target is accepted only when Discovery identifies Saturn");
        check(discoveryMatchesVerifiedAnanProfile(10, 10),
              "selected Saturn ID matches the actual Discovery reply");
        // Every unimplemented board must remain fail-closed until its
        // own byte-verified session profile is implemented. Check both manual
        // connects and matching picker metadata, not just Orion examples.
        for (const std::uint8_t id : supportedBoardIds) {
            if (receiveProfile(id))
                continue;
            check(!discoveryMatchesVerifiedAnanProfile(id, -1),
                  "unimplemented P2 board is rejected for manual connection");
            check(!discoveryMatchesVerifiedAnanProfile(id, static_cast<int>(id)),
                  "unimplemented P2 board is rejected even with matching picker ID");
        }
        check(!discoveryMatchesVerifiedAnanProfile(10, 20),
              "Saturn reply cannot satisfy a selected HermesC10");
        check(discoveryMatchesVerifiedAnanProfile(4, -1),
              "Orion selects its own legacy-ALEX DDC2 profile");
        check(discoveryMatchesVerifiedAnanProfile(5, 5),
              "selected Orion MkII has its own receive profile");
        check(discoveryMatchesVerifiedAnanProfile(5, -1),
              "manual Orion MkII selects its own physical receive profile");
        check(!discoveryMatchesVerifiedAnanProfile(5, 10),
              "Orion MkII reply cannot satisfy a selected Saturn");
        check(!discoveryMatchesVerifiedAnanProfile(10, 4),
              "a Saturn reply is rejected when discovery selected a different board");
        check(!discoveryMatchesVerifiedAnanProfile(99, -1),
              "unknown board ID cannot use the Saturn session profile");
    }

    // ANAN-7000DLE RX layout from Thetis UpdateDDCs/GetDDC,
    // clsHardwareSpecific.cs and ChannelMaster CmdRx/CmdHighPriority.
    {
        const auto profile = receiveProfile(5);
        check(profile && profile->numAdcs == 2 && profile->firstDdcIndex == 2,
              "Orion MkII has two ADCs and normal RX1 on physical DDC2");
        check(profile && !profile->saturnDroopDefaults,
              "Orion MkII cannot inherit Saturn gateware droop defaults");
        const auto general = buildGeneral(3, true);
        check(general[37] == 8 && general[58] == 0 && general[59] == 3,
              "Orion MkII requests phase words, disables PA and enables both ALEX words");
        const std::array<DdcConfig, 1> ddcs{{{192, 0}}};
        const auto rx = buildDdcSpecific(ddcs, 2, true, true, 2);
        check(rx[4] == 2 && rx[5] == 3 && rx[6] == 3 && rx[7] == 4,
              "Orion MkII enables only physical DDC2 and both physical ADC linearisation bits");
        check(rx[29] == 0 && rx[30] == 0 && rx[31] == 192 && rx[34] == 24,
              "DDC2 rate, ADC selection and sample width occupy bytes 29..34");
        bool unusedRowsZero = true;
        for (std::size_t i = 17; i < 29; ++i) {
            unusedRowsZero = unusedRowsZero && rx[i] == 0;
        }
        check(unusedRowsZero, "Orion MkII leaves DDC0 and DDC1 rows disabled and zero");
        const std::array<std::uint32_t, 1> words{{phaseWord(14'200'000)}};
        const auto hp = buildHighPriority(true, words, false, true, 7, 9, 2, 2, true);
        check(hp[17] == (words[0] >> 24) && hp[18] == ((words[0] >> 16) & 255)
              && hp[19] == ((words[0] >> 8) & 255) && hp[20] == (words[0] & 255),
              "Orion MkII tune occupies DDC2 bytes 17..20");
        bool unusedWordsZero = true;
        for (std::size_t i = 9; i < 17; ++i) {
            unusedWordsZero = unusedWordsZero && hp[i] == 0;
        }
        check(unusedWordsZero, "DDC2 tuning does not accidentally tune DDC0/1");
        check(hp[1432] == 2 && hp[1433] == 0x10 && hp[1434] == 0 && hp[1435] == 2,
              "14.2 MHz ANT2 selects Thetis 20/15 m BPF1 and 30/20 m LPF");
        check(hp[1443] == 7 && hp[1442] == 9 && hp[4] == 1,
              "Orion MkII preserves per-ADC attenuation without PTT");
        check(ddcIndexForSenderPort(1037, 1, 1037) == 0
              && !ddcIndexForSenderPort(1035, 1, 1037)
              && !ddcIndexForSenderPort(1038, 1, 1037),
              "physical DDC2 port maps to logical RX0, other ports are rejected");
        constexpr std::array<double, 7> bandHz{{1'900'000, 3'600'000, 7'100'000,
            14'200'000, 18'100'000, 28'400'000, 50'100'000}};
        constexpr std::array<std::uint32_t, 7> lpfBits{{1u << 23, 1u << 22,
            1u << 21, 1u << 20, 1u << 31, 1u << 30, 1u << 29}};
        constexpr std::uint32_t lpfMask = 0xE0F00000u;
        for (std::size_t n = 0; n < bandHz.size(); ++n) {
            const std::array<std::uint32_t, 1> tune{{phaseWord(bandHz[n])}};
            const auto packet = buildHighPriority(true, tune, false, true, 0, 0, 1, 2, true);
            const std::uint32_t alex = (std::uint32_t(packet[1432]) << 24)
                | (std::uint32_t(packet[1433]) << 16)
                | (std::uint32_t(packet[1434]) << 8) | packet[1435];
            check((alex & lpfMask) == lpfBits[n], "Orion MkII selects exactly one Thetis LPF relay");
            check((alex & (1u << 27)) == 0 && packet[4] == 1 && packet[345] == 0,
                  "Orion MkII filter switching leaves T/R relay, PTT and drive inactive");
        }
        check(buildDdcSpecific(ddcs, 2, true, true, -1)[7] == 0 &&
                  buildDdcSpecific(ddcs, 2, true, true, kMaxDdcs)[7] == 0,
              "invalid physical DDC offsets cannot enable a receiver");
    }

    // Thetis clsHardwareSpecific/GetDDC and Nereus P2CodecHermes.
    {
        constexpr std::array<int, 5> ids{{1, 2, 3, 4, 20}};
        constexpr std::array<int, 5> adcCounts{{1, 1, 2, 2, 1}};
        constexpr std::array<int, 5> ddcOffsets{{0, 0, 2, 2, 0}};
        for (std::size_t n = 0; n < ids.size(); ++n) {
            const ReceiveProfile profile = *receiveProfile(ids[n]);
            check(profile.numAdcs == adcCounts[n] && profile.firstDdcIndex == ddcOffsets[n],
                  "new receive family has Thetis physical ADC/DDC assignment");
            const std::array<DdcConfig, 1> ddcs{{{48, 0}}};
            const auto rx = buildDdcSpecific(ddcs, profile.numAdcs, true, true, profile.firstDdcIndex);
            check(rx[4] == adcCounts[n] && rx[5] == (adcCounts[n] == 1 ? 1 : 3)
                  && rx[6] == rx[5] && rx[7] == (1 << ddcOffsets[n]),
                  "new receive family enables its actual ADCs and RX1 DDC only");
            const std::array<std::uint32_t, 1> tune{{phaseWord(7'100'000)}};
            const auto hp = buildHighPriority(true, tune, false, true, 4, 11, 1,
                profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind, profile.numAdcs);
            check(hp[1435] == (ids[n] == 20 ? 0x10 : 0x20),
                  "7.1 MHz distinguishes HermesC10 MkII BPF from legacy ALEX HPF");
            check(hp[1433] == 0x20 && hp[1432] == 1,
                  "new profile selects 60/40 m LPF and ANT1 in reception");
            if (profile.numAdcs == 1) {
                check(hp[1430] == 0 && hp[1431] == 0 && hp[1442] == 0,
                      "single-ADC profile writes no nonexistent ADC1 filter/attenuator");
            }
            check(hp[4] == 1 && hp[345] == 0 && (hp[1432] & 8) == 0,
                  "new profile's filter packet cannot key or drive TX");
        }
        check(receiveProfile(2)->physicalDdcLimit == 2,
              "Hermes II cannot be assigned physical DDC2 or DDC3");
        const std::array<std::int16_t, 2> lr{{0x1234, 0x2345}};
        const auto swapped = buildSpeakerAudio(0, lr, true);
        check(swapped[4] == 0x23 && swapped[5] == 0x45
              && swapped[6] == 0x12 && swapped[7] == 0x34,
              "Hermes codec speaker channels match Thetis LRAudioSwap");
        const std::array<std::int16_t, 1> one{{0x1234}};
        const auto shortSwapped = buildSpeakerAudio(0, one, true);
        check(shortSwapped[4] == 0 && shortSwapped[5] == 0
              && shortSwapped[6] == 0x12 && shortSwapped[7] == 0x34,
              "short swapped speaker input preserves its unpaired sample without out-of-bounds access");
        constexpr std::array<double, 7> hpfHz{{1'799'999, 1'800'000, 6'500'000,
            9'500'000, 13'000'000, 20'000'000, 50'000'000}};
        constexpr std::array<std::uint16_t, 7> hpfBits{{0x1000, 0x40, 0x20, 0x10, 2, 4, 8}};
        for (std::size_t n = 0; n < hpfHz.size(); ++n) {
            const std::array<std::uint32_t, 1> tune{{phaseWord(hpfHz[n])}};
            const auto hp = buildHighPriority(true, tune, false, true, 0, 0, 1,
                0, true, ReceiveFilterKind::LegacyHpf, 1);
            check((std::uint16_t(hp[1434]) << 8 | hp[1435]) == hpfBits[n],
                  "legacy ALEX uses exact Thetis HPF boundaries and bypass");
        }
    }

    // ---- General packet ----
    {
        const auto g = buildGeneral();
        check(g.size() == 60, "General packet is 60 bytes");
        check(g[4] == 0x00, "General packet command byte = 0x00");
        check(g[58] == 1 && buildGeneral(3,true)[58] == 0,
              "Thetis General byte 58 is PA enable, inverse of DisablePA");
        check(g[37] == 0x08, "byte37 bit3 set: phase word, matching discovery byte21==1");
        check(g[38] == 0x01, "byte38 bit0 set: hardware reset timer / watchdog ON");
        check(g[39] == 0x00,
              "byte39 = 0x00: BE + 3-byte, the only format byte22==0 declares -- "
              "not a guess, confirmed by the 80.0 dB vs 26.0 dB field measurement");
        bool portsZero = true;
        for (std::size_t i = 5; i <= 22; ++i)
            if (g[i] != 0) { portsZero = false; break; }
        check(portsZero, "port table (bytes 5-22) left at defaults");
    }

    // ---- DDC-Specific packet ----
    {
        const auto d = buildDdcSpecific(48);
        check(d.size() == 1444, "DDC-Specific packet is 1444 bytes (full spec length)");
        check(d[4] == 2, "byte4 = 2 ADCs (G2's two phase-synchronous ADCs)");
        check(d[7] == 0x01, "byte7 bit0: DDC0 enabled");
        check(d[17] == 0x00, "byte17: DDC0 -> ADC0");
        check((static_cast<int>(d[18]) << 8 | d[19]) == 48,
              "bytes 18-19: DDC0 rate = 48 (raw ksps, big-endian u16)");
        check(d[22] == 24, "byte22: DDC0 sample size = 24 bits");
        for (std::size_t i = 23; i < 1444; ++i)
            if (d[i] != 0) { check(false, "DDC1-79 fields must stay disabled/zero"); break; }

        const auto d96 = buildDdcSpecific(96, 1);
        check((static_cast<int>(d96[18]) << 8 | d96[19]) == 96, "rate field honours the argument");
        check(d96[4] == 1, "numAdcs argument is honoured");

        // Keep the one-ADC Hermes-class wire field testable separately
        // from Saturn; this validates encoding only, not a connectable profile.
        const auto dOneAdc = buildDdcSpecific(48, 1, true, true, 0);
        check(dOneAdc[5] == 0x01 && dOneAdc[6] == 0x01,
              "one-ADC board enables dither/random for ADC0 only");
        const auto dTwoAdcs = buildDdcSpecific(48, 2, true, true, 0);
        check(dTwoAdcs[5] == 0x03 && dTwoAdcs[6] == 0x03,
              "two-ADC Saturn retains both dither/random bits");
        const auto dOneAdcOff = buildDdcSpecific(48, 1, false, false, 0);
        check(dOneAdcOff[5] == 0 && dOneAdcOff[6] == 0,
              "disabled one-ADC dither/random has no active bits");
        const auto dHermes = buildDdcSpecific(48, 1, true, true, 0);
        check(dHermes[4] == 1 && dHermes[7] == 0x01 && dHermes[17] == 0,
              "one-ADC profile advertises one ADC and keeps DDC0 on ADC0");

        // Dither/Random (bytes 5/6) default on, both ADC0 and ADC1 bits
        // together -- one control on this radio, not a per-ADC pair.
        check(d[5] == 0x03 && d[6] == 0x03,
              "dither/random default on, ADC0+ADC1 bits both set");
        const auto dOff = buildDdcSpecific(48, 2, false, false);
        check(dOff[5] == 0x00 && dOff[6] == 0x00, "dither/random both off clears both bytes");

        // ADC select (byte 17): 0 = ADC0 (default, checked above), 1 = ADC1/RX2.
        const auto dAdc1 = buildDdcSpecific(48, 2, true, true, 1);
        check(dAdc1[17] == 0x01, "ddc0AdcIndex=1 routes DDC0 to ADC1/RX2");
    }

    // ---- High Priority: run bit ONLY, no PTT anywhere ----
    {
        const std::uint32_t word = phaseWord(10'000'000.0);
        const auto hp = buildHighPriority(true, word);
        check(hp.size() == 1444, "High Priority packet is 1444 bytes (full spec length)");
        check(hp[4] == 0x01, "run=true -> byte4 = 0x01 (bit0 only)");
        check((static_cast<std::uint32_t>(hp[9]) << 24 | static_cast<std::uint32_t>(hp[10]) << 16
              | static_cast<std::uint32_t>(hp[11]) << 8 | hp[12]) == word,
              "bytes 9-12: DDC0 frequency/phase word round-trips");

        const auto hpOff = buildHighPriority(false, 0);
        check(hpOff[4] == 0x00, "run=false -> byte4 = 0x00");

        // Structural guarantee: byte4 bits 1-4 (PTT0..3) are NEVER set by this
        // encoder, for either run state -- there is no parameter that could
        // set them.
        check((hp[4] & 0x1E) == 0 && (hpOff[4] & 0x1E) == 0,
              "PTT bits (byte4 bits 1-4) are never set -- no parameter exists to set them");

        // Alex0 (bytes 1432-1435): ANT1 (bit 24) plus Thetis' Saturn
        // BPF1 row selected from DDC0's phase word, unless bypass is requested.
        // Alex1 (bytes 1430-1431): HF Bypass 2 (bit 12) tracks the separate
        // bypassAdc1Filters argument.
        const auto readU32 = [](const std::array<std::uint8_t, 1444>& p, std::size_t i) {
            return (static_cast<std::uint32_t>(p[i]) << 24) | (static_cast<std::uint32_t>(p[i + 1]) << 16)
                 | (static_cast<std::uint32_t>(p[i + 2]) << 8) | p[i + 3];
        };
        const auto readU16 = [](const std::array<std::uint8_t, 1444>& p, std::size_t i) {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[i]) << 8) | p[i + 1]);
        };
        check(readU32(hp, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 4)),
              "10 MHz default selects Thetis Saturn BPF1 40/30 m row plus ANT1");
        const auto hp160m = buildHighPriority(true, phaseWord(1'900'000.0));
        check(readU32(hp160m, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 6)),
              "1.9 MHz selects Thetis Saturn BPF1 160 m row");
        const auto hp20m = buildHighPriority(true, phaseWord(14'200'000.0));
        check(readU32(hp20m, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 1)),
              "14.2 MHz selects Thetis Saturn BPF1 20/15 m row");
        const auto hp22m = buildHighPriority(true, phaseWord(22'000'000.0));
        check(readU32(hp22m, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 2)),
              "22 MHz selects Thetis Saturn BPF1 12/10 m row");
        const auto hp35m = buildHighPriority(true, phaseWord(35'000'000.0));
        check(readU32(hp35m, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 3)),
              "35 MHz selects Thetis Saturn BPF1 6 m row");
        const auto hp61m = buildHighPriority(true, phaseWord(61'440'000.0));
        check(readU32(hp61m, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 3)),
              "61.44 MHz upper edge selects Thetis Saturn BPF1 6 m row");
        const auto hpOutOfBand = buildHighPriority(true, phaseWord(1'000'000.0));
        check(readU32(hpOutOfBand, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 12)),
              "below Thetis BPF1 rows, Alex0 uses bypass");
        const auto hpAnt2 = buildHighPriority(true, phaseWord(14'200'000.0),
                                              false, true, 0, 0, 2);
        check(readU32(hpAnt2, 1432) == ((std::uint32_t{1} << 25) | (std::uint32_t{1} << 1)),
              "ANT2 selects Alex0 bit 25 while retaining the 20/15 m BPF1 row");
        const auto hpAnt3 = buildHighPriority(true, phaseWord(14'200'000.0),
                                              false, true, 0, 0, 3);
        check(readU32(hpAnt3, 1432) == ((std::uint32_t{1} << 26) | (std::uint32_t{1} << 1)),
              "ANT3 selects Alex0 bit 26 while retaining the 20/15 m BPF1 row");
        const auto hpInvalidAnt = buildHighPriority(true, phaseWord(14'200'000.0),
                                                    false, true, 0, 0, 9);
        check(readU32(hpInvalidAnt, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 1)),
              "invalid antenna value fails safe to ANT1");
        check(readU16(hp, 1430) == (std::uint16_t{1} << 12),
              "default: HF Bypass 2 (ADC1/RX2) set in Alex1");

        const auto hpNoBypass = buildHighPriority(true, word, false, false);
        check(readU32(hpNoBypass, 1432) == ((std::uint32_t{1} << 24) | (std::uint32_t{1} << 4)),
              "bypassAdc0Filters=false: ANT1 stays set and frequency-selected BPF1 is enabled");
        check(readU16(hpNoBypass, 1430) == 0,
              "bypassAdc1Filters=false: Alex1 stays zero");

        // Step attenuators (pp.34,36): byte 1443 = ADC0, byte 1442 = ADC1,
        // 0-31 dB. Default is none; values land on the right ADC's byte and
        // are clamped to the spec range at both ends.
        check(hp[1443] == 0 && hp[1442] == 0,
              "default: both step attenuators at 0 dB");
        const auto hpBypass = buildHighPriority(true, word, true, true);
        const auto hpAtt = buildHighPriority(true, word, true, true, 12, 20);
        check(hpAtt[1443] == 12, "adc0AttenuationDb=12 -> byte 1443 = 12");
        check(hpAtt[1442] == 20, "adc1AttenuationDb=20 -> byte 1442 = 20");
        const auto hpClamp = buildHighPriority(true, word, true, true, 40, -5);
        check(hpClamp[1443] == 31, "attenuation above 31 dB clamps to 31");
        check(hpClamp[1442] == 0, "negative attenuation clamps to 0");
        check(readU32(hpAtt, 1432) == readU32(hpBypass, 1432)
                  && readU16(hpAtt, 1430) == readU16(hpBypass, 1430)
                  && hpAtt[4] == hpBypass[4],
              "attenuation touches no other field (Alex0/Alex1/run unchanged)");
        const std::array<std::uint32_t, 1> oneWord{word};
        check(buildHighPriority(true, oneWord, true, true, 12, 20) == hpAtt,
              "multi-DDC overload carries the attenuators identically");
    }

    // ---- Multi-DDC encode (DDC-Specific rows at 17+6n, HP words at 9+4n) ----
    // The single-DDC overloads delegate to these, so the first thing to pin
    // is that delegation is byte-identical -- a drift there would silently
    // change the packets the bench-validated DDC0 path already sends.
    {
        const std::array<DdcConfig, 1> justDdc0{DdcConfig{48, 0}};
        check(buildDdcSpecific(justDdc0) == buildDdcSpecific(48),
              "buildDdcSpecific: 1-element list is byte-identical to the single-DDC overload");

        const std::uint32_t word = phaseWord(10'000'000.0);
        const std::array<std::uint32_t, 1> justWord0{word};
        check(buildHighPriority(true, justWord0) == buildHighPriority(true, word),
              "buildHighPriority: 1-element list is byte-identical to the single-DDC overload");
    }

    {
        // Four DDCs, deliberately all different so a row landing on the wrong
        // offset (or every row reading DDC0's values) cannot pass.
        const std::array<DdcConfig, 4> ddcs{
            DdcConfig{48, 0}, DdcConfig{192, 1}, DdcConfig{768, 0}, DdcConfig{1536, 1}};
        const auto d = buildDdcSpecific(ddcs);

        check(d[7] == 0x0F, "byte7 bits 0-3 set: DDC0-3 all enabled");

        bool rowsOk = true;
        for (std::size_t n = 0; n < ddcs.size(); ++n) {
            const std::size_t row = 17 + 6 * n;
            const int rate = static_cast<int>(d[row + 1]) << 8 | d[row + 2];
            if (d[row] != static_cast<std::uint8_t>(ddcs[n].adcIndex)
                || rate != ddcs[n].rateKsps
                || d[row + 3] != 0 || d[row + 4] != 0   // CIC1/CIC2 "For Future use"
                || d[row + 5] != 24) {
                rowsOk = false;
                std::fprintf(stderr,
                    "  DDC%zu row at %zu: adc=%u rate=%d cic=%u,%u size=%u "
                    "(expected adc=%d rate=%d cic=0,0 size=24)\n",
                    n, row, d[row], rate, d[row + 3], d[row + 4], d[row + 5],
                    ddcs[n].adcIndex, ddcs[n].rateKsps);
            }
        }
        check(rowsOk, "each DDC's 6-byte row at 17+6n carries its own ADC/rate/size, "
                      "with CIC1/CIC2 left zero");

        // Nothing beyond the last encoded row: DDC4-79 must stay disabled.
        bool tailZero = true;
        for (std::size_t i = 17 + 6 * ddcs.size(); i < 1444; ++i)
            if (d[i] != 0) { tailZero = false; break; }
        check(tailZero, "DDC4-79 rows stay zero/disabled");

        // High Priority: one 4-byte phase word per DDC at 9+4n.
        const std::array<std::uint32_t, 4> words{
            phaseWord(3'500'000.0), phaseWord(7'100'000.0),
            phaseWord(14'200'000.0), phaseWord(28'400'000.0)};
        const auto hp = buildHighPriority(true, words);
        bool wordsOk = true;
        for (std::size_t n = 0; n < words.size(); ++n) {
            const std::size_t at = 9 + 4 * n;
            const std::uint32_t got =
                static_cast<std::uint32_t>(hp[at]) << 24
              | static_cast<std::uint32_t>(hp[at + 1]) << 16
              | static_cast<std::uint32_t>(hp[at + 2]) << 8
              | hp[at + 3];
            if (got != words[n]) {
                wordsOk = false;
                std::fprintf(stderr, "  DDC%zu phase word at %zu: got %u expected %u\n",
                             n, at, got, words[n]);
            }
        }
        check(wordsOk, "each DDC's frequency/phase word round-trips at 9+4n");

        // Principle VI is structural, not incidental: adding DDCs must not
        // add a way to key. No parameter here can set the PTT bits.
        check((hp[4] & 0x1E) == 0,
              "multi-DDC High Priority still never sets the PTT bits (byte4 bits 1-4)");
    }

    // ---- Sender-port demux (the ONLY thing separating DDC streams) ----
    {
        // Base + n, verified against p2app's generalpacket.c and its
        // DefaultPorts[] table (both give 1035..1044 for DDC0..9).
        check(kDdc0DefaultPort == 1035, "DDC0's default source port is 1035");

        // In range: each port maps to its own index.
        bool mapped = true;
        for (int n = 0; n < 4; ++n) {
            const auto idx = ddcIndexForSenderPort(
                static_cast<std::uint16_t>(1035 + n), 4);
            if (!idx || *idx != n) {
                mapped = false;
                std::fprintf(stderr, "  port %d -> %s (expected %d)\n", 1035 + n,
                             idx ? std::to_string(*idx).c_str() : "nullopt", n);
            }
        }
        check(mapped, "ports 1035..1038 map to DDC 0..3");

        // Below the base, and at/above the active count: both rejected.
        check(!ddcIndexForSenderPort(1034, 4).has_value(),
              "a port below the base is not a DDC stream");
        check(!ddcIndexForSenderPort(1039, 4).has_value(),
              "a port past the active DDC count is rejected (DDC4 with only 4 active)");
        check(ddcIndexForSenderPort(1038, 4).has_value(),
              "the last active DDC's port IS accepted (boundary, not off-by-one)");

        // A single-DDC session must reject DDC1's port -- this is the case
        // that keeps a leftover stream from a previous multi-DDC session
        // from being fed to DDC0.
        check(ddcIndexForSenderPort(1035, 1).has_value(),
              "single-DDC session accepts DDC0's port");
        check(!ddcIndexForSenderPort(1036, 1).has_value(),
              "single-DDC session rejects DDC1's port");

        // Degenerate counts: no DDCs active means nothing is a DDC stream.
        check(!ddcIndexForSenderPort(1035, 0).has_value(),
              "numDdc=0 accepts nothing");
        check(!ddcIndexForSenderPort(1035, -1).has_value(),
              "a negative DDC count accepts nothing rather than underflowing");

        // The shared-socket traffic this must not misclassify: Mic Data and
        // High Priority Status arrive on the same PC-side socket.
        check(!ddcIndexForSenderPort(kRadioPort, 4).has_value(),
              "the radio's command port is never mistaken for a DDC stream");

        // An explicit non-default base still works, for a session that ever
        // negotiates ports via the General packet's bytes 17-18.
        const auto negotiated = ddcIndexForSenderPort(2003, 4, 2000);
        check(negotiated && *negotiated == 3,
              "an explicitly negotiated base port is honoured");
    }

    {
        // Empty list: a legitimate all-disabled packet, not a malformed one.
        const auto none = buildDdcSpecific(std::span<const DdcConfig>{});
        check(none[7] == 0x00, "empty DDC list disables every DDC (byte7 = 0)");
        check(none[4] == 2, "empty DDC list still carries the ADC-count field");

        // Over the cap: extras are ignored rather than encoded or truncating
        // the packet. kMaxDdcs is the codec's own bound (Principle VII).
        std::array<DdcConfig, kMaxDdcs + 1> tooMany;
        tooMany.fill({48, 0});
        const auto capped = buildDdcSpecific(tooMany);
        check(capped[7] == 0x7f, "generic codec bounds seven physical DDC rows");
        const auto eighth = 17 + 6 * kMaxDdcs;
        check(capped[eighth] == 0 && capped[eighth + 1] == 0 && capped[eighth + 5] == 0,
              "eighth physical DDC stays disabled");
        std::array<std::uint32_t, kMaxDdcs + 1> words;
        words.fill(0x12345678u);
        const auto hp = buildHighPriority(true, words);
        auto wordAt=[&](std::size_t offset){std::uint32_t value=0;for(int n=0;n<4;++n)value=(value<<8)|hp[offset+n];return value;};
        check(wordAt(9 + 4 * (kMaxDdcs - 1)) == 0x12345678u &&
                  wordAt(9 + 4 * kMaxDdcs) == 0,
              "seventh tuning word is encoded and eighth is refused");
    }

    // ---- DDC frame: strict shape validation ----
    {
        // A well-formed frame is accepted.
        const auto good = makeDdcFrame(0xDEADBEEF, 2);
        const auto frame = parseDdcFrame(good);
        check(frame.has_value(), "well-formed DDC frame accepted");
        check(frame && frame->seq == 0xDEADBEEF, "sequence number parsed");
        check(frame && frame->samples == 2, "declared sample count parsed");
        check(frame && frame->iqRaw.size() == 2 * static_cast<std::size_t>(kDdcSampleBytes),
              "iqRaw spans exactly samples * kDdcSampleBytes bytes");

        // Too-short datagram (below the 16-byte header) rejected outright.
        const std::array<std::uint8_t, 10> tooShort{};
        check(!parseDdcFrame(tooShort).has_value(), "sub-header-length datagram rejected");

        // Declared/actual length mismatch rejected, NOT clamped -- this is
        // the exact case that misparsed Mic Data as garbage DDC0 frames
        // during Phase 1a before this check existed.
        auto lying = makeDdcFrame(0, 2);
        lying[14] = 0x00; lying[15] = 100;  // declares 100 samples, only 2 present
        check(!parseDdcFrame(lying).has_value(),
              "declared/actual length mismatch rejected, not clamped");

        // Plausible-but-wrong bitsPerSample rejected -- the Mic-Data
        // regression case (a foreign packet that happens to be long enough
        // but isn't a 24-bit DDC frame at all).
        auto wrongBits = makeDdcFrame(0, 2);
        wrongBits[12] = 0x00; wrongBits[13] = 16;  // claims 16-bit samples
        check(!parseDdcFrame(wrongBits).has_value(),
              "non-24-bit bitsPerSample rejected (Mic Data regression case)");

        // An all-zero 60-byte datagram (the shape of a High Priority Status
        // packet) must not be misread as a tiny valid DDC frame.
        const std::array<std::uint8_t, 60> statusShaped{};
        check(!parseDdcFrame(statusShaped).has_value(),
              "a 60-byte all-zero datagram (Status-shaped) is rejected, "
              "not decoded as a garbage DDC frame");
    }

    // ---- IQ sample decode: BE, sign-extension, full scale ----
    {
        const auto good = makeDdcFrame(0, 2);
        const auto frame = parseDdcFrame(good);
        check(frame.has_value(), "frame for decode test parses");
        std::vector<std::complex<float>> iq;
        decodeIq(*frame, iq);
        check(iq.size() == 2, "decodeIq produces exactly `samples` pairs");
        const float s = 1.0f / static_cast<float>(kFullScale24Bit);
        check(approx(iq[0].real(), 0.0f) && approx(iq[0].imag(), -1.0f * s), "sample 0 I/Q");
        check(approx(iq[1].real(), 100.0f * s) && approx(iq[1].imag(), -101.0f * s), "sample 1 I/Q");

        // Full scale and sign-extension edges, mirroring the HL2 test's own
        // full-scale check.
        std::uint8_t fs[6] = {0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00};  // I=+full scale, Q=-full scale
        const auto sample = decodeIqSample(fs);
        check(sample.real() == 1.0f, "0x7FFFFF normalises to exactly +1.0");
        check(sample.imag() < -1.0f && sample.imag() > -1.0001f,
              "0x800000 normalises to just past -1.0 (two's-complement asymmetry)");
    }

    // ---- DDC Audio, PC -> radio: buildSpeakerAudio ----
    {
        std::vector<std::int16_t> full(kSpeakerFramesPerPacket * kSpeakerChannels);
        for (std::size_t i = 0; i < full.size(); ++i) {
            full[i] = static_cast<std::int16_t>(i);
        }
        const auto pkt = buildSpeakerAudio(0x01020304u, full);
        check(pkt.size() == kSpeakerPacketBytes, "speaker packet is exactly 260 bytes");
        check(pkt[0] == 0x01 && pkt[1] == 0x02 && pkt[2] == 0x03 && pkt[3] == 0x04,
              "sequence is big-endian in bytes 0-3");
        // Sample 1 is the value 1, so its two bytes are 0x00,0x01 -- high byte
        // first. A little-endian encoder passes a symmetric-value test and fails
        // this one, which is the whole point of using an asymmetric value.
        check(pkt[4] == 0x00 && pkt[5] == 0x00, "sample 0 == 0");
        check(pkt[6] == 0x00 && pkt[7] == 0x01, "sample 1 is big-endian 0x0001");
        check(pkt[8] == 0x00 && pkt[9] == 0x02, "sample 2 is big-endian 0x0002");
        // Last sample is index 127 == 0x007F, at bytes 258-259.
        check(pkt[258] == 0x00 && pkt[259] == 0x7F, "the 128th sample lands in the last two bytes");

        // Negative samples: two's complement on the wire, not a sign-shifted
        // value. -2 is 0xFFFE; an encoder that shifted the signed type right
        // would be implementation-defined here.
        const std::array<std::int16_t, 4> neg{-1, -2, -32768, 32767};
        const auto negPkt = buildSpeakerAudio(0, neg);
        check(negPkt[4] == 0xFF && negPkt[5] == 0xFF, "-1 encodes as 0xFFFF");
        check(negPkt[6] == 0xFF && negPkt[7] == 0xFE, "-2 encodes as 0xFFFE");
        check(negPkt[8] == 0x80 && negPkt[9] == 0x00, "-32768 encodes as 0x8000");
        check(negPkt[10] == 0x7F && negPkt[11] == 0xFF, "+32767 encodes as 0x7FFF");
        // Everything past a short input is silence, not stale content.
        bool tailSilent = true;
        for (std::size_t i = 12; i < kSpeakerPacketBytes; ++i) {
            if (negPkt[i] != 0) tailSilent = false;
        }
        check(tailSilent, "a short input zero-fills the rest of the packet");

        // Excess is ignored rather than overrunning the packet.
        std::vector<std::int16_t> tooMany(kSpeakerFramesPerPacket * kSpeakerChannels + 64, 9);
        const auto clipped = buildSpeakerAudio(7, tooMany);
        check(clipped.size() == kSpeakerPacketBytes, "excess input still yields one packet");
        check(clipped[258] == 0x00 && clipped[259] == 0x09, "the packet fills to its last byte");
    }

    // ---- High Priority Status: the two speaker fields and the supply rail ----
    {
        std::vector<std::uint8_t> pkt(kHighPriorityStatusBytes, 0);
        pkt[0] = 0x00; pkt[1] = 0x00; pkt[2] = 0x01; pkt[3] = 0x02;  // seq 258
        pkt[30] = 0b0000'1000;            // speaker underflow, bit 3
        pkt[37] = 0x01; pkt[38] = 0x40;   // level 320, big-endian
        pkt[57] = 0x02; pkt[58] = 0x18;   // AIN3 536 counts, big-endian
        // AIN6, the input the spec calls "supply voltage", carrying what the
        // same G2 sent there. It must not be what the rail decodes from.
        pkt[49] = 0x06; pkt[50] = 0x35;   // 1589 counts

        const auto st = parseHighPriorityStatus(pkt);
        check(st.has_value(), "a 60-byte status packet parses");
        if (st) {
            check(st->seq == 258, "the sequence is big-endian");
            check(st->speakerUnderflow, "byte 30 bit 3 is the speaker underflow");
            check(st->speakerFifoLevel == 320, "bytes 37-38 are the level, big-endian");
            check(st->supplyRailCounts == 536,
                  "bytes 57-58 are the supply rail in raw counts, big-endian --"
                  " not bytes 49-50, whose 1589 counts read ~40 V on a 13.65 V rail");
        }

        // The byte ORDER is the half a wrong decode gets silently right on small
        // values: 536 is 0x0218, so a byte-swapped read gives 0x1802 = 6146. A
        // single-byte read of 57 gives 2. Pinned so neither passes.
        check(st && st->supplyRailCounts != 6146 && st->supplyRailCounts != 2,
              "neither a byte-swapped nor a single-byte supply read passes");

        // A rail the radio reports as zero is a REAL reading (a dead supply), not
        // a missing one, and must decode as 0 rather than being treated as absent.
        std::vector<std::uint8_t> zeroRail(kHighPriorityStatusBytes, 0);
        const auto dead = parseHighPriorityStatus(zeroRail);
        check(dead.has_value() && dead->supplyRailCounts == 0,
              "a zero supply reading decodes as zero, not as no reading");

        // The scale against the one bench point: 536 counts with a meter at the
        // DC input reading 13.65 V. A 3.3 V reference would read 9.03 V here.
        check(std::fabs(536 * kSupplyRailVoltsPerCount - 13.65) < 0.05,
              "the bench median of 536 counts reads within 0.05 V of the 13.65 V"
              " the meter showed");

        // Bit 3 only. Bit 2 is the DUC's underflow and must not read as ours --
        // that is the mistake the bitmask exists to prevent.
        pkt[30] = 0b0000'0100;
        const auto duc = parseHighPriorityStatus(pkt);
        check(duc.has_value() && !duc->speakerUnderflow,
              "the DUC's underflow bit is not read as the speaker's");
        pkt[30] = 0b1111'0111;   // every bit BUT ours
        const auto others = parseHighPriorityStatus(pkt);
        check(others.has_value() && !others->speakerUnderflow,
              "no other overflow bit reads as the speaker's");
        pkt[30] = 0b1111'1111;
        const auto all = parseHighPriorityStatus(pkt);
        check(all.has_value() && all->speakerUnderflow,
              "our bit is still seen when every other one is set too");

        // Exact length, like parseDdcFrame: a different length is a different
        // packet, not a truncated status. Bounds are checked before indexing, so a
        // short buffer must not be read past (Principle VII).
        std::vector<std::uint8_t> shortPkt(kHighPriorityStatusBytes - 1, 0);
        check(!parseHighPriorityStatus(shortPkt).has_value(),
              "a 59-byte datagram is not a status packet");
        std::vector<std::uint8_t> longPkt(kHighPriorityStatusBytes + 1, 0);
        check(!parseHighPriorityStatus(longPkt).has_value(),
              "a 61-byte datagram is not a status packet");
        check(!parseHighPriorityStatus(std::span<const std::uint8_t>{}).has_value(),
              "an empty datagram is not a status packet");

        // A Discovery reply is also 60 bytes. It parses as BOTH, which is why the
        // caller must try the reply first -- pinned here so the ordering
        // requirement is visible from the test, not only from a comment.
        std::vector<std::uint8_t> reply(kHighPriorityStatusBytes, 0);
        reply[4] = 0x02;
        // Use a plausible identity so this fixture passes Discovery
        // validation as well as the intentionally overlapping status parser.
        reply[5] = 0x88;
        reply[10] = 0x6A;
        reply[11] = 10;
        reply[12] = 43;
        check(parseDiscoveryReply(reply).has_value()
              && parseHighPriorityStatus(reply).has_value(),
              "a discovery reply satisfies both parsers, so reply-first ordering matters");
    }

    // ---- the receiver's audio stage ----
    {
        check(sliceAudioAmplitude(100) == 1.0f, "gain 100 is unity");
        check(sliceAudioAmplitude(0) == 0.0f, "gain 0 is exactly silent, not -40 dB");
        check(sliceAudioAmplitude(150) == 1.0f, "gain above 100 clamps to unity");
        check(sliceAudioAmplitude(-5) == 0.0f, "gain below 0 clamps to silence");
        // Midpoint is -20 dB by construction: 10^(0.05 * -20) == 0.1.
        check(approx(sliceAudioAmplitude(50), 0.1f), "gain 50 is -20 dB");
        // Monotonic, and NOT linear -- a linear law would put 50 at 0.5. This is
        // the check that fails if the dB curve is replaced by a linear one.
        check(sliceAudioAmplitude(50) < 0.2f, "the law is dB, not linear");
        check(sliceAudioAmplitude(25) < sliceAudioAmplitude(50)
              && sliceAudioAmplitude(50) < sliceAudioAmplitude(75),
              "gain is monotonic across its range");

        // Balance attenuates one side and never boosts the other.
        check(sliceAudioLeftPanGain(50) == 1.0f && sliceAudioRightPanGain(50) == 1.0f,
              "centred balance leaves both channels alone");
        check(sliceAudioLeftPanGain(0) == 1.0f && sliceAudioRightPanGain(0) == 0.0f,
              "hard left silences the right channel");
        check(sliceAudioRightPanGain(100) == 1.0f && sliceAudioLeftPanGain(100) == 0.0f,
              "hard right silences the left channel");
        check(sliceAudioLeftPanGain(75) <= 1.0f && sliceAudioRightPanGain(75) <= 1.0f,
              "no balance setting boosts either channel above unity");

        // Applied to a real block.
        float block[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        applySliceAudioInPlace(block, 4, true, 100, 50);
        bool muted = true;
        for (float v : block) { if (v != 0.0f) muted = false; }
        check(muted, "mute zeroes every sample in the block");

        float panned[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        applySliceAudioInPlace(panned, 2, false, 100, 0);
        check(panned[0] == 1.0f && panned[1] == 0.0f && panned[2] == 1.0f && panned[3] == 0.0f,
              "hard-left balance keeps L and silences R, frame by frame");

        float scaled[2] = {1.0f, 1.0f};
        applySliceAudioInPlace(scaled, 1, false, 50, 50);
        check(approx(scaled[0], 0.1f) && approx(scaled[1], 0.1f),
              "gain 50 scales both channels by -20 dB");

        // Mute wins over a live gain, and does not consume it.
        float both[2] = {1.0f, 1.0f};
        applySliceAudioInPlace(both, 1, true, 50, 0);
        check(both[0] == 0.0f && both[1] == 0.0f, "mute wins over gain and balance");

        // Quantising to the wire format. NaN is the case a clamp cannot catch;
        // infinity pins the predicate as isfinite rather than isnan.
        check(sliceAudioToInt16(0.0f) == 0, "silence quantises to zero");
        check(sliceAudioToInt16(1.0f) == 32767, "full scale is the positive peak");
        check(sliceAudioToInt16(-1.0f) == -32767, "negative full scale mirrors it");
        check(sliceAudioToInt16(2.0f) == 32767, "an overshoot clamps, not wraps");
        check(sliceAudioToInt16(-2.0f) == -32767, "and so does a negative overshoot");
        check(sliceAudioToInt16(std::numeric_limits<float>::quiet_NaN()) == 0,
              "a NaN quantises to silence, not to undefined behaviour");
        check(sliceAudioToInt16(std::numeric_limits<float>::infinity()) == 0,
              "an infinity quantises to silence, not a full-scale click");
        check(sliceAudioToInt16(-std::numeric_limits<float>::infinity()) == 0,
              "including a negative infinity");

        // Degenerate input must not walk off anything.
        applySliceAudioInPlace(nullptr, 4, false, 50, 50);
        float untouched[2] = {1.0f, -1.0f};
        applySliceAudioInPlace(untouched, 0, false, 0, 0);
        check(untouched[0] == 1.0f && untouched[1] == -1.0f,
              "a zero-frame block is left alone");
    }

    // ---- speaker send pacing ----
    {
        SpeakerAudioPacer pacer;
        check(pacer.estimatedFifoFrames() == 0.0, "a fresh pacer believes the FIFO is empty");
        // Empty FIFO, plenty queued: release up to the target, not everything.
        const int first = pacer.packetsToSend(100);
        check(first > 0, "an empty FIFO releases packets");
        // The target IS the burst bound -- see kTargetFifoFrames. A separate cap
        // was removed because it could not bind, and the assertion that it did
        // was removed with it.
        check(first == 16, "an empty FIFO fills exactly to the sixteen-packet target");

        // THE CHECK THE BENCH HAD TO FIND FOR US. The target must cover more than
        // one drain tick's worth of audio, or the stream delivers less than real
        // time and the radio plays a gap every few milliseconds. A 5 ms tick
        // removes 5 * 48 = 240 frames; whole-packet truncation then releases
        // floor(240/64) = 3 packets, 192 frames, so a target of only 240 frames
        // can never catch up. This is stated as frames-per-tick rather than as a
        // packet count so it stays true if the packet geometry changes.
        {
            constexpr double kDrainTickSeconds = 0.005;   // P2Client::kSpeakerDrainMs
            constexpr double kFramesPerTick = kDrainTickSeconds * kSpeakerSampleRateHz;
            check(SpeakerAudioPacer::kTargetFifoFrames > kFramesPerTick,
                  "the target holds more than one drain tick drains");
            // And by enough that truncation plus a late tick cannot empty it: two
            // ticks is the margin, which four packets did not have and sixteen does.
            check(SpeakerAudioPacer::kTargetFifoFrames >= 3.0 * kFramesPerTick,
                  "the target has margin for a late tick, not just the nominal one");
            // Releasing a full target must be worth at least one tick of real time,
            // or no burst can ever restore the deficit a late tick created.
            SpeakerAudioPacer fresh;
            const double releasedFrames =
                fresh.packetsToSend(1000) * static_cast<double>(kSpeakerFramesPerPacket);
            check(releasedFrames > kFramesPerTick,
                  "one burst delivers more audio than one tick consumes");

            // AND AT LEAST ONE SOURCE BLOCK. Audio arrives in blocks, not
            // samples: at the default 48 ksps DDC, WdspChannel's output block is
            // 1024 * 24000/48000 = 512 frames of 24 kHz audio, which is 1024
            // frames at the stream's rate. A target below one block cannot release
            // a block before the next one arrives, so the queue grows to its cap
            // and drops -- audible as garbled speech, and worst at the DEFAULT
            // rate. Spelled out rather than hardcoded so the arithmetic is
            // checkable against the DSP config.
            constexpr double kWdspInputBlock = 1024.0;
            constexpr double kDefaultDdcRate = 48000.0;
            constexpr double kAudioRate = 24000.0;
            constexpr double kSourceBlockFramesAtStreamRate =
                (kWdspInputBlock * kAudioRate / kDefaultDdcRate)
                * (static_cast<double>(kSpeakerSampleRateHz) / kAudioRate);
            check(SpeakerAudioPacer::kTargetFifoFrames >= kSourceBlockFramesAtStreamRate,
                  "the target holds at least one whole source block");
        }

        for (int i = 0; i < first; ++i) pacer.onPacketSent();
        check(pacer.estimatedFifoFrames() == first * kSpeakerFramesPerPacket,
              "each sent packet credits its own frames");
        // At target with no time passed, nothing more may go.
        check(pacer.packetsToSend(100) == 0, "a full FIFO releases nothing");

        // One packet's worth of time drains one packet's worth of frames.
        pacer.advance(static_cast<double>(kSpeakerFramesPerPacket) / kSpeakerSampleRateHz);
        check(pacer.packetsToSend(100) == 1, "one packet interval makes room for one packet");

        // Nothing queued, nothing sent, however much room there is.
        check(pacer.packetsToSend(0) == 0, "an empty queue sends nothing");

        // A long stall cannot bank negative depth and then over-release.
        pacer.advance(10.0);
        check(pacer.estimatedFifoFrames() == 0.0, "the estimate floors at empty");
        check(pacer.packetsToSend(1000) == 16,
              "recovery from a stall still respects the target, not a debt");

        // Backwards or zero elapsed time is inert, not corrupting.
        SpeakerAudioPacer p2;
        p2.onPacketSent();
        const double before = p2.estimatedFifoFrames();
        p2.advance(0.0);
        p2.advance(-1.0);
        check(p2.estimatedFifoFrames() == before, "zero or negative elapsed time changes nothing");

        p2.reset();
        check(p2.estimatedFifoFrames() == 0.0, "reset clears the estimate for the next session");

        // ---- catch-up: a backlog on OUR side must be recoverable ----
        //
        // THE G2 BENCH FOUND THIS. With only the target governing releases the
        // queue settled at 26-45 packets of 64 and stayed there: in steady state
        // the pacer releases exactly what time consumes, so a backlog picked up
        // during the connect transient is never recovered. Nominal rate is
        // break-even by definition.
        SpeakerAudioPacer p3;
        constexpr int kCap = 64;
        check(!p3.catchingUp(), "a fresh pacer is not catching up");

        // At target, and idle: nothing goes, which is the behaviour that stranded
        // the backlog.
        for (int i = 0; i < 16; ++i) p3.onPacketSent();
        check(p3.packetsToSend(40) == 0, "at target with no drain time, nothing is released");

        // ONE SOURCE BLOCK IS NOT A BACKLOG, and this is the boundary that matters
        // most here. Audio arrives one whole block at a time -- sixteen packets at
        // the default DDC rate, by the same arithmetic the target invariant above
        // spells out -- so a threshold at or below one block latches on every
        // ordinary block. Catch-up would be the steady state, the credit target
        // would govern no release at all, and a release bounded only by the queue
        // would run at four times nominal for as long as any backlog lasted.
        // Derived here rather than hardcoded, so a change of block size or packet
        // geometry moves the check with it.
        constexpr double kWdspInputBlock = 1024.0;
        constexpr double kDefaultDdcRate = 48000.0;
        constexpr double kAudioRate = 24000.0;
        constexpr int kPacketsPerSourceBlock = static_cast<int>(
            ((kWdspInputBlock * kAudioRate / kDefaultDdcRate)
             * (static_cast<double>(kSpeakerSampleRateHz) / kAudioRate))
            / kSpeakerFramesPerPacket);
        p3.setBacklog(kPacketsPerSourceBlock, kCap);
        check(!p3.catchingUp(),
              "one whole source block queued is a block arriving, not a backlog");
        p3.setBacklog(2 * kPacketsPerSourceBlock - 1, kCap);
        check(!p3.catchingUp(),
              "and anything short of a second block is still not one");

        // Two blocks is audio that arrived while we were not draining.
        p3.setBacklog(2 * kPacketsPerSourceBlock, kCap);
        check(p3.catchingUp(), "two whole source blocks queued enters catch-up");
        check(p3.packetsToSend(40) > 0,
              "catch-up releases despite the target being met -- the whole point");

        // Hysteresis: it does not drop out at the same level it entered.
        p3.setBacklog(2 * kPacketsPerSourceBlock - 1, kCap);
        check(p3.catchingUp(), "catch-up persists through the band, so it cannot flap");
        p3.setBacklog(kPacketsPerSourceBlock + 1, kCap);
        check(p3.catchingUp(), "and still persists one packet above the exit");
        p3.setBacklog(kPacketsPerSourceBlock, kCap);
        check(!p3.catchingUp(),
              "down to the block in flight, the backlog is gone and catch-up ends");
        p3.setBacklog(1, kCap);
        check(!p3.catchingUp(), "a drained queue leaves catch-up");

        // Bounded. A recovery that released without limit would overrun the very
        // FIFO this class protects.
        SpeakerAudioPacer p4;
        p4.setBacklog(kCap, kCap);
        check(p4.catchingUp() && p4.packetsToSend(10000) <= 16,
              "catch-up is bounded, not an unlimited burst");

        // A zero or negative capacity must not decide anything.
        SpeakerAudioPacer p5;
        p5.setBacklog(100, 0);
        check(!p5.catchingUp(), "an unknown capacity cannot trigger catch-up");

        // reset() clears it, or a reconnect would start mid-recovery.
        SpeakerAudioPacer p6;
        p6.setBacklog(kCap, kCap);
        p6.reset();
        check(!p6.catchingUp(), "reset clears catch-up for the next session");
    }

    // The discovery registry must preserve Thetis wire IDs and
    // recognize every documented OpenHPSDR family without guessing an SKU.
    {
        struct BoardCase { std::uint8_t id; const char* label; };
        constexpr BoardCase boards[] = {
            {0, "Atlas / Metis"},
            {1, "Hermes (ANAN-10 / ANAN-100)"},
            {2, "Hermes II (ANAN-10E / ANAN-100B)"},
            {3, "Angelia (ANAN-100D)"},
            {4, "Orion (ANAN-200D)"},
            {5, "Orion MkII (ANAN-7000DLE/8000DLE, Anvelina Pro 3, Red Pitaya)"},
            {6, "Hermes Lite 2"},
            {10, "Saturn (ANAN-G2 / ANAN-G2 1K)"},
            {11, "Saturn MkII"},
            {20, "HermesC10 (ANAN-G2E)"},
        };
        for (const auto& b : boards) {
            DiscoveryReply reply;
            reply.boardId = b.id;
            check(reply.isSupportedOpenHpsdrBoard(),
                  "known Thetis/NereusSDR board ID is accepted by discovery");
            check(std::string(reply.boardDisplayName()) == b.label,
                  "board ID uses its evidence-backed family label");
        }
        DiscoveryReply unknown;
        unknown.boardId = 7;
        check(!unknown.isSupportedOpenHpsdrBoard(),
              "reserved/unknown board IDs are not silently classified");
        check(std::string(unknown.boardDisplayName()) == "Unknown OpenHPSDR board",
              "unknown board IDs receive an explicit label");
    }

    // ANAN-7000DLE capture, 2026-10-17: the directed request to
    // UDP/1024 gets a valid reply from UDP/0 after 94 us. MAC anonymized.
    // The former manual-connect port guard discarded this exact reply shape.
    {
        std::array<std::uint8_t, 60> captured{
            0, 0, 0, 0, 2, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
            5, 44, 22, 0, 0, 0, 0, 0, 0, 5, 1, 16, 15
        };
        const auto directed = parseDirectedDiscoveryReply(captured, 0);
        check(directed && directed->boardId == 5 && directed->wireProtocol == 2
                  && !directed->streaming,
              "captured Orion MkII P2 reply from UDP/0 reaches manual discovery");
        check(parseDirectedDiscoveryReply(captured, 1024).has_value(),
              "P2 discovery still accepts the usual UDP/1024 source");
        check(!parseDirectedDiscoveryReply(captured, 1025),
              "status-source port is not accepted as a directed discovery endpoint");
        check(!parseDirectedDiscoveryReply(std::span(captured).first(10), 0),
              "UDP/0 does not bypass discovery length validation");
        captured[0] = 1;
        check(!parseDirectedDiscoveryReply(captured, 0),
              "UDP/0 does not bypass discovery signature validation");
        const std::array<std::uint8_t, 63> legacy{
            0xef, 0xfe, 2, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 22, 5
        };
        check(parseDirectedDiscoveryReply(legacy, 1024).has_value(),
              "P1 discovery retains its UDP/1024 endpoint");
        check(!parseDirectedDiscoveryReply(legacy, 0),
              "observed P2 UDP/0 exception does not broaden P1 discovery");
    }

    {
        // Editable defaults preserve all existing qualified profile packets.
        for (int board : {1, 2, 3, 4, 5, 10, 20}) {
            const auto profile = receiveProfile(board);
            const auto defaults = defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf);
            for (double hz : {0.0, 1500000.0, 1800000.0, 2100000.0, 5000000.0,
                              5500000.0, 6500000.0, 9500000.0, 11000000.0,
                              13000000.0, 20000000.0, 22000000.0, 35000000.0,
                              50000000.0, 61440000.0}) {
                const std::array<std::uint32_t, 1> words{phaseWord(hz)};
                check(buildHighPriority(true, words, false, true, 0, 0, 1, profile->firstDdcIndex,
                          profile->selectReceiveLpf, profile->filterKind, profile->numAdcs)
                      == buildHighPriority(true, words, false, true, 0, 0, 1, profile->firstDdcIndex,
                          profile->selectReceiveLpf, profile->filterKind, profile->numAdcs, &defaults),
                      "editable model defaults preserve existing qualified P2 packet bytes");
            }
        }
        // Thetis legacy HPF and MkII BPF differ at 5 MHz (bits 6 and 5).
        FilterConfig legacy = defaultFilterConfig(false);
        FilterConfig mkII = defaultFilterConfig(true);
        const std::array<std::uint32_t, 1> frequency{phaseWord(5000000)};
        const auto oldBoard = buildHighPriority(true, frequency, false, false, 0, 0, 1, 2,
            true, ReceiveFilterKind::LegacyHpf, 2, &legacy);
        const auto newBoard = buildHighPriority(true, frequency, false, false, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &mkII);
        check(oldBoard[1435] == 0x40 && newBoard[1435] == 0x20,
              "effective hardware selects legacy HPF versus MkII BPF at 5 MHz");
        check(oldBoard[1431] == 0x40 && newBoard[1431] == 0x20,
              "ADC1 automatic filter uses the separate Thetis Alex1 register");
        const auto bypass = buildHighPriority(true, frequency, true, true, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &mkII);
        check(bypass[1434] == 0x10 && bypass[1430] == 0x10
              && bypass[1435] == 0 && bypass[1431] == 0,
              "both ADC bypasses clear selected receive filter bits");
        mkII.receive[0].highHz = 2499999;
        mkII.receive[1].lowHz = 2500000;
        const std::array<std::uint32_t,1> edge{phaseWord(2300000)};
        const auto changed = buildHighPriority(true, edge, false, true, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &mkII);
        check(changed[1435] == 0x40, "edited range changes the encoded receive relay");
        mkII.lowPass[0].highHz = 5999999;
        mkII.lowPass[1] = {6000000,6999999};
        const auto customLpf = buildHighPriority(true, frequency, false, true, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &mkII);
        check(customLpf[1433] == 0x80 && customLpf[4] == 1,
              "edited LPF limits reach the wire without enabling TX");
        const auto oneAdc = buildHighPriority(true, frequency, false, false, 0, 0, 1, 0,
            true, ReceiveFilterKind::LegacyHpf, 1, &legacy);
        check(oneAdc[1430] == 0 && oneAdc[1431] == 0,
              "single ADC models never receive ADC1 filter commands");
        check(selectedFilterBand(legacy.receive, 1800000) == 0
              && selectedFilterBand(legacy.receive, 1799999) == -1,
              "legacy HPF bypass and exact start edge are preserved");
    }

    {
        // Thetis capture, 2026-10-17: Alex1 upper relay word follows
        // ANT1 + 80 m/20 m LPF even in RX. Its low ADC1 filter word is separate.
        const FilterConfig config = defaultFilterConfig(false);
        for (const auto& [hz, capturedUpper] : std::array<std::pair<double, std::uint32_t>, 2>{{
                 {3650000.0, 0x01400000u}, {14175000.0, 0x01100000u}}}) {
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            const auto packet = buildHighPriority(true, words, false, true, 0, 0, 1, 2,
                true, ReceiveFilterKind::LegacyHpf, 2, &config);
            const auto readWord = [&packet](int offset) {
                return (std::uint32_t{packet[offset]} << 24) | (std::uint32_t{packet[offset+1]} << 16)
                    | (std::uint32_t{packet[offset+2]} << 8) | packet[offset+3];
            };
            check((readWord(1428) & 0xffff0000u) == capturedUpper,
                  "Thetis captured LPF/ANT1 relay word is also sent in Alex1");
            check((readWord(1428) & 0xffffu) == 0x1000u,
                  "mirroring LPF and antenna preserves independent ADC1 bypass");
            check(packet[4] == 1 && (readWord(1428) & 0x08040000u) == 0,
                  "Alex1 relay compatibility never sets T/R, TX status or PTT");
        }
        const std::array<std::uint32_t,1> words{phaseWord(14175000)};
        const auto oneAdc = buildHighPriority(true, words, false, false, 0, 0, 3, 0,
            true, ReceiveFilterKind::LegacyHpf, 1, &config);
        check(oneAdc[1428] == 4 && oneAdc[1429] == 0x10
              && oneAdc[1430] == 0 && oneAdc[1431] == 0,
              "single ADC retains its LPF and antenna relay word without an ADC1 filter");
        const auto saturn = buildHighPriority(true, words, false, true, 0, 0, 1, 0,
            false, ReceiveFilterKind::MkIIBpf, 2);
        check(saturn[1428] == 0 && saturn[1429] == 0,
              "qualified Saturn profile keeps its LPF relay word disabled");
    }

    {
        // Thetis capture OC byte 1401 is the configured mask shifted left, not an ALEX bit.
        auto config = defaultFilterConfig(false);
        config.ocReceive[1] = {3500000, 3800000, 0x0b};
        config.ocReceive[5] = {14000000, 14350000, 0x05};
        const auto packet = [&](double hz, bool run = true) {
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            return buildHighPriority(run, words, false, true, 0, 0, 1, 2,
                true, ReceiveFilterKind::LegacyHpf, 2, &config);
        };
        check(packet(3650000)[1401] == 0, "OC outputs default off despite configured pins");
        config.ocEnabled = true;
        check(packet(3650000)[1401] == 0x16 && packet(14175000)[1401] == 0x0a,
              "explicit RX OC masks match Thetis captured 80m and 20m encoding");
        check(packet(3500000)[1401] == 0x16 && packet(3800000)[1401] == 0x16,
              "OC band edges tolerate phase-word quantisation");
        check(packet(1000000)[1401] == 0 && packet(3650000, false)[1401] == 0,
              "out-of-band and stop commands release RX OC outputs");
        check(packet(3650000)[1397] == 0 && packet(3650000)[4] == 1,
              "RX OC configuration cannot key TX or write Anvelina extra pins");
    }

    {
        for (AnanModel model : {AnanModel::Anan10, AnanModel::Anan100, AnanModel::Anan100D,
                               AnanModel::Anan200D, AnanModel::RedPitaya}) {
            const auto* info = ananModelInfo(model);
            const auto profile = receiveProfile(info->nativeBoardId, model);
            check(profile && !hasAlex2Filters(*profile), "legacy effective models do not expose Alex-2 Filters");
        }
        for (AnanModel model : {AnanModel::Anan7000, AnanModel::Anan8000, AnanModel::AnanG2}) {
            const auto* info = ananModelInfo(model);
            const auto profile = receiveProfile(info->nativeBoardId, model);
            check(profile && hasAlex2Filters(*profile), "dual-ADC MkII effective models expose Alex-2 Filters");
        }
        const auto override = receiveProfile(5,AnanModel::Anan200D,true);
        check(override && !hasAlex2Filters(*override), "Orion MkII mapped as ANAN-200D hides Alex-2 Filters");
        check(!hasAlex2Filters(*receiveProfile(20)), "single-ADC G2E has no Alex-2 Filters");
    }

    {
        auto config = defaultFilterConfig(false);
        config.bypassAdc1 = false;
        const auto packet = [&](double hz) {
            const std::array<std::uint32_t,1> words{phaseWord(hz)};
            return buildHighPriority(true, words, false, false, 0, 0, 1, 2,
                true, ReceiveFilterKind::LegacyHpf, 2, &config);
        };
        config.receiveBypass[0] = true;
        const auto bypass = packet(3800000);
        check(bypass[1434] == 0x10 && bypass[1435] == 0 && bypass[1431] == 0x40,
              "ADC0 per-band HPF bypass sets bit12 without bypassing ADC1");
        check(packet(14200000)[1435] == 2, "band bypass leaves other HPF bands selected");
        config.receiveBypass[0] = false;
        config.disable6mLnaOnRx = true;
        const auto noLna = packet(51000000);
        check(noLna[1434] == 0x10 && noLna[1435] == 0 && noLna[1431] == 8,
              "disable 6m LNA on RX bypasses ADC0 6m filter while preserving ADC1");
        check(packet(14200000)[1435] == 2, "6m LNA disable does not bypass HF filters");
        config.lpfBypassOnRx = true;
        const auto sixMetreLpf = packet(3800000);
        check(sixMetreLpf[1432] == 0x21 && sixMetreLpf[1433] == 0
              && sixMetreLpf[1428] == 0x21 && sixMetreLpf[4] == 1,
              "6m/ByPass on RX selects 6m LPF in both relay words without PTT");
        check(selectedReceiveFilterBand(config,51000000) == -1
              && selectedReceiveFilterBand(config,51000000,true) == 5,
              "LED receive selection reflects bypass while ADC1 stays independently selected");
    }

    {
        // Thetis' separate Alex-2 range table must affect only ADC1.
        auto config = defaultFilterConfig(true);
        config.receiveAdc1 = config.receive;
        (*config.receiveAdc1)[1].highHz = 6499999;
        (*config.receiveAdc1)[2].lowHz = 6500000;
        const std::array<std::uint32_t, 1> words{phaseWord(6000000)};
        const auto packet = buildHighPriority(true, words, false, false, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &config);
        check(packet[1435] == 0x10 && packet[1431] == 0x20,
              "independent Alex-2 limits select ADC1 80/60m while ADC0 retains 40/30m");
        check(selectedReceiveFilterBand(config,6000000) == 2
              && selectedReceiveFilterBand(config,6000000,true) == 1,
              "ADC0 and ADC1 commanded indicators follow their own range tables");
        check(packet[4] == 1 && (packet[1432] & 8) == 0,
              "separate ADC1 ranges do not key PTT or the T/R relay");
        const auto bypass = buildHighPriority(true, words, false, true, 0, 0, 1, 2,
            true, ReceiveFilterKind::MkIIBpf, 2, &config);
        check(bypass[1430] == 0x10 && bypass[1431] == 0 && bypass[1435] == 0x10,
              "ADC1 master bypass overrides independent limits without changing ADC0");
        config.receiveAdc1.reset();
        check(receiveFilterBits(config,6000000,true) == 0x10,
              "absent ADC1 table preserves old shared-range selection");
    }

    {
        const std::array<std::uint32_t, 2> words{phaseWord(14'175'000), phaseWord(7'100'000)};
        const auto normal = buildHighPriority(true, words, false, true, 6, 9, 3, 0, false,
            ReceiveFilterKind::MkIIBpf, 2, nullptr, 0, nullptr, 1.0, false);
        const auto muted = buildHighPriority(true, words, false, true, 6, 9, 3, 0, false,
            ReceiveFilterKind::MkIIBpf, 2, nullptr, 0, nullptr, 1.0, true);
        bool unchanged = true;
        for (std::size_t i = 0; i < normal.size(); ++i) {
            if (i != 1400 && normal[i] != muted[i]) { unchanged = false; }
        }
        check(unchanged && normal[1400] == 0 && muted[1400] == 2 && muted[4] == 1,
              "amplifier mute changes only Thetis HP byte 1400 bit 1, preserving filters, DDCs and RX-only run");
    }

    {
        std::array<std::complex<float>, kTxIqSamplesPerPacket> samples{};
        const auto silence = buildTxIq(samples, 0x12345678);
        check(silence && silence->size() == 1444 && kTxIqPort == 1029,
              "TX IQ has Thetis packet length and destination port");
        check(silence && (*silence)[0] == 0x12 && (*silence)[1] == 0x34
              && (*silence)[2] == 0x56 && (*silence)[3] == 0x78,
              "TX IQ sequence is big endian");
        bool zeroPayload = silence.has_value();
        if (silence) {
            for (std::size_t i = 4; i < silence->size(); ++i) {
                zeroPayload = zeroPayload && (*silence)[i] == 0;
            }
        }
        check(zeroPayload, "TX silence fills the entire payload with zero");
        samples[0] = {1.0f, -1.0f};
        samples[1] = {0.5f, -0.5f};
        samples.back() = {2.0f, -2.0f};
        const auto encoded = buildTxIq(samples, 0xffffffff);
        const std::array<std::uint8_t, 12> expected{
            0x7f,0xff,0xff,0x80,0x00,0x01,0x40,0x00,0x00,0xc0,0x00,0x00};
        bool matched = encoded.has_value();
        if (encoded) {
            for (std::size_t i = 0; i < expected.size(); ++i) {
                matched = matched && (*encoded)[4+i] == expected[i];
            }
            for (std::size_t i = 0; i < 6; ++i) {
                matched = matched && (*encoded)[1438+i] == expected[i];
            }
        }
        check(matched, "TX I/Q order, signed BE24, rounding and saturation match Thetis");
        check(!buildTxIq({}, 0) && !buildTxIq(std::span(samples).first(239), 0),
              "TX IQ rejects incomplete blocks");
        std::array<std::complex<float>, 241> oversized{};
        check(!buildTxIq(oversized, 0), "TX IQ rejects oversized blocks");
        samples.back() = {std::numeric_limits<float>::quiet_NaN(), 0};
        check(!buildTxIq(samples, 0), "TX IQ rejects NaN even in the last sample");
        samples.back() = {0, std::numeric_limits<float>::infinity()};
        check(!buildTxIq(samples, 0), "TX IQ rejects infinite Q");
        const auto wrapped = buildTxIq(std::span(oversized).first(240), 0);
        check(wrapped && (*wrapped)[0] == 0 && (*wrapped)[1] == 0
              && (*wrapped)[2] == 0 && (*wrapped)[3] == 0,
              "TX IQ accepts sequence zero after wrap");
    }

    {
        using namespace AetherSDR::anan;
        const auto reading = rfPowerReading(AnanModel::Anan200D, 5, 1004, 102, 14'200'000);
        const double forward = std::pow(1000.0 * 5 / 4095, 2) / 0.108;
        check(reading && std::abs(reading->forwardWatts - forward) < 1e-9,
              "200D override uses Orion detector scale on shared board5");
        check(reading && reading->swr && std::abs(*reading->swr - 1.222222222) < 1e-6,
              "SWR uses the corrected forward/reflected voltage ratio");
        const auto six = rfPowerReading(AnanModel::Anan200D, 4, 1004, 102, 50'100'000);
        check(six && reading && six->reflectedWatts < reading->reflectedWatts,
              "six metre reflected-power bridge has its own scale");
        check(!rfPowerReading(AnanModel::Automatic, 5, 1000, 100, 14e6),
              "ambiguous automatic board5 cannot guess a PA calibration");
        check(!rfPowerReading(AnanModel::Automatic, 10, 1000, 100, 14e6),
              "automatic Saturn cannot distinguish G2 and G2 1K detector scaling");
        check(!rfPowerReading(AnanModel::Anan200D, 4, 65535, 0, 14e6),
              "out of range slow ADC input is rejected");
        const auto noise = rfPowerReading(AnanModel::Anan200D, 4, 4, 2, 14e6);
        check(noise && noise->forwardWatts == 0 && !noise->swr,
              "no-carrier reading is zero watts without invented SWR");
        const auto bad = rfPowerReading(AnanModel::Anan200D, 4, 1004, 2002, 14e6);
        check(bad && bad->swr && *bad->swr >= 10, "reflected power above forward is a bad match");
        std::array<std::uint8_t, kHighPriorityStatusBytes> packet{};
        packet[14] = 3; packet[15] = 0xec; packet[22] = 0; packet[23] = 102;
        const auto status = parseHighPriorityStatus(packet);
        check(status && status->forwardPowerCounts == 1004 && status->reversePowerCounts == 102,
              "P2 reads directional ADCs at Thetis byte offsets14 and22");
    }
    if (g_failures == 0)
        std::fprintf(stderr, "anan_p2_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
