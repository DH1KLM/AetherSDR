//DH1KLM: Source-pinned logical models, independent of sockets and GUI labels.
#include "core/backends/anan/P2Protocol.h"
#include <array>
#include <cstdlib>
#include <iostream>

using namespace AetherSDR::anan;
static void check(bool value, const char* claim)
{
    if (!value) { std::cerr << "FAIL: " << claim << '\n'; std::exit(1); }
}
//DH1KLM: ANT1/2/3 use P2 ALEX bits 24..26 and P1 bank-0 C4 bits 0..1.
// Switching antennas must preserve the rest of the receive configuration.
static void antennaIsolation(const ReceiveProfile& profile, bool supportsP1)
{
    for (const int hz : {3'800'000, 14'200'000, 51'000'000}) {
        for (const bool bypass : {false, true}) {
            auto filters = defaultFilterConfig(profile.filterKind == ReceiveFilterKind::MkIIBpf);
            filters.ocEnabled = true;
            filters.ocReceive[0] = {0, 61'440'000, 0x55};
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            const auto highPriority = [&](int antenna) {
                return buildHighPriority(true, words, bypass, bypass, 17, 23, antenna,
                    profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
                    profile.numAdcs, &filters);
            };
            const auto baseline = highPriority(1);
            for (int antenna = 1; antenna <= 3; ++antenna) {
                auto packet = highPriority(antenna);
                check((packet[1432] & 7u) == (1u << (antenna - 1)), "P2 exact ANT1/2/3 relay bit");
                check((packet[1428] & 7u) == (profile.selectReceiveLpf ? (1u << (antenna - 1)) : 0u),
                      "P2 mirrored antenna only on profiles using the shared LPF word");
                check(packet[4] == 1, "antenna selection keeps P2 in RX without PTT");
                packet[1432] = (packet[1432] & ~7u) | (baseline[1432] & 7u);
                packet[1428] = (packet[1428] & ~7u) | (baseline[1428] & 7u);
                check(packet == baseline, "P2 antenna leaves tuning, filters, OC and attenuation unchanged");
            }
            if (!supportsP1) { continue; }
            p1::ReceiveConfig config;
            config.numAdcs = profile.numAdcs;
            config.adcIndex = profile.numAdcs - 1;
            config.frequencyHz = hz;
            config.rxOcMask = 0x55;
            config.receiveFilterBits = bypass ? 0x20 : 0x02;
            config.adc1ReceiveFilterBits = bypass ? 0x20 : 0x04;
            config.adc0AttenuationDb = 17;
            config.adc1AttenuationDb = 23;
            for (int address = 0; address <= 0x26; ++address) {
                config.antenna = 1;
                const auto original = p1::receiveControl(address, config);
                for (int antenna = 1; antenna <= 3; ++antenna) {
                    config.antenna = antenna;
                    auto control = p1::receiveControl(address, config);
                    check(control.has_value() == original.has_value(), "P1 antenna cannot authorize another control bank");
                    if (!control) { continue; }
                    check(((*control)[0] & 1) == 0, "P1 antenna keeps MOX clear");
                    if (address == 0) {
                        check(((*control)[4] & 3) == antenna - 1, "P1 exact ANT1/2/3 selector");
                        (*control)[4] = (*original)[4];
                    }
                    check(control == original, "P1 antenna leaves ADC routing, frequency, filters and OC unchanged");
                }
            }
        }
    }
}
//DH1KLM: Thetis network.h bits 8..11/14 and networkproto1.c bank-0 C3.
static void receiveInputIsolation(const ReceiveProfile& profile, bool supportsP1)
{
    const bool mkII = profile.filterKind == ReceiveFilterKind::MkIIBpf;
    const std::array<unsigned, 4> alexBits = mkII
        ? std::array<unsigned, 4>{0, 0x0c00, 0x4200, 0x4100}
        : std::array<unsigned, 4>{0, 0x0c00, 0x0a00, 0x0900};
    const std::array<unsigned, 4> c3Bits = mkII
        ? std::array<unsigned, 4>{0, 0xa0, 0x41, 0x61}
        : std::array<unsigned, 4>{0, 0xa0, 0xc0, 0xe0};
    auto filters = defaultFilterConfig(mkII);
    filters.ocEnabled = true;
    filters.ocReceive[0] = {0,61'440'000,0x55};
    const std::array<std::uint32_t, 1> words{phaseWord(14'200'000)};
    const auto packet = [&] {
        return buildHighPriority(true, words, false, false, 17, 23, 2,
            profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
            profile.numAdcs, &filters);
    };
    const auto original = packet();
    p1::ReceiveConfig config;
    config.numAdcs = profile.numAdcs;
    config.adcIndex = profile.numAdcs - 1;
    config.antenna = 2;
    config.dither = config.random = true;
    config.rxOcMask = 0x55;
    config.mkIIBpf = mkII;
    const auto general = p1::receiveControl(0, config);
    for (int input = 0; input <= 3; ++input) {
        filters.rxOnlyInput = input;
        auto actual = packet();
        check((actual[1434] & 0x4fu) == (alexBits[input] >> 8), "P2 RX input and RX_OUT/master exact bits");
        actual[1434] &= ~0x4fu;
        check(actual == original, "P2 input preserves ANT relays, ADC1, filters, OC, tuning and PTT");
        if (!supportsP1) { continue; }
        config.rxOnlyInput = input;
        auto control = p1::receiveControl(0, config);
        check(control && ((*control)[3] & 0xe1u) == c3Bits[input], "P1 RX input and RX_OUT/master exact C3 bits");
        (*control)[3] &= ~0xe1u;
        check(control == general, "P1 input preserves dither, random, OC and antenna bank");
        for (int address = 1; address <= 0x26; ++address) {
            const auto changed = p1::receiveControl(address, config);
            config.rxOnlyInput = 0;
            check(changed == p1::receiveControl(address, config), "P1 input changes only the general control bank");
            config.rxOnlyInput = input;
        }
    }
    filters.rxOnlyInput = 0;
    check(packet() == original, "return to ANT bank clears dedicated-input relays");
    config.rxOnlyInput = 4;
    check(!p1::receiveControl(0, config), "invalid P1 receive input refused");
}
//DH1KLM: Independent ADC1 boundaries must survive ADC0 bypass and input routing.
static void adc1FilterIsolation(const ReceiveProfile& profile)
{
    auto filters = defaultFilterConfig(profile.filterKind == ReceiveFilterKind::MkIIBpf);
    filters.receiveAdc1 = std::array<FilterBand, 6>{{
        {0,999999}, {1000000,2999999}, {3000000,7999999},
        {8000000,15999999}, {16000000,31999999}, {32000000,61440000}}};
    filters.receiveBypass.fill(true);
    filters.disable6mLnaOnRx = true;
    constexpr std::array<unsigned, 6> bits{6,5,4,1,2,3};
    for (std::size_t row = 0; row < bits.size(); ++row) {
        const auto band = (*filters.receiveAdc1)[row];
        for (const auto hz : {band.lowHz, (band.lowHz + band.highHz) / 2, band.highHz}) {
            const std::array<std::uint32_t, 1> words{phaseWord(hz)};
            for (const bool bypass : {false, true}) {
                for (int input = 0; input <= 3; ++input) {
                    filters.rxOnlyInput = input;
                    const auto packet = buildHighPriority(true, words, true, bypass, 17, 23, 2,
                        profile.firstDdcIndex, profile.selectReceiveLpf, profile.filterKind,
                        profile.numAdcs, &filters);
                    const unsigned lower = (unsigned{packet[1430]} << 8) | packet[1431];
                    const unsigned expected = profile.numAdcs == 1 ? 0
                        : bypass ? 0x1000 : 1u << bits[row];
                    check(lower == expected, "P2 ADC1 exact band/bypass bits at independent range edges");
                    check((packet[1434] & 0x10) != 0 && packet[1443] == 17
                          && packet[1442] == (profile.numAdcs == 2 ? 23 : 0),
                          "ADC1 filtering preserves ADC0 bypass and independent attenuation");
                }
            }
        }
    }
    for (int adc = 0; adc < profile.numAdcs; ++adc) {
        const std::array<DdcConfig, 1> ddcs{{{96, adc}}};
        const auto packet = buildDdcSpecific(ddcs, profile.numAdcs, true, true, profile.firstDdcIndex);
        check(packet[7] == (1u << profile.firstDdcIndex)
              && packet[17 + 6 * profile.firstDdcIndex] == adc,
              "ADC source selection retains the model's physical RX DDC");
    }
    check(hasAlex2Filters(profile)
              == (profile.numAdcs == 2 && profile.filterKind == ReceiveFilterKind::MkIIBpf),
          "Alex-2 availability follows the effective filter hardware, not the physical board alone");
}
int main()
{
    check(kAnanModels.size() == 18, "all 17 Thetis logical IDs plus automatic");
    for (int id = -1; id <= 16; ++id) {
        const auto model = static_cast<AnanModel>(id);
        const auto* info = ananModelInfo(model);
        check(info && info->model == model && !info->name.empty(), "model IDs have unique names");
        if (id < 0 || model == AnanModel::Hpsdr || model == AnanModel::HermesLite2) { continue; }
        const auto profile = receiveProfile(info->nativeBoardId, model);
        check(profile.has_value(), "all ANAN logical models resolve P2 RX");
        check(profile->numAdcs == (id <= 5 || model == AnanModel::AnanG2E ? 1 : 2), "model ADC counts");
        check(profile->firstDdcIndex == (info->nativeBoardId == 10 || profile->numAdcs == 1 ? 0 : 2),
              "preserve Saturn bench DDC0 and select correct physical DDC elsewhere");
        const auto expectedFilter = id <= 7 || model == AnanModel::RedPitaya
            ? ReceiveFilterKind::LegacyHpf : ReceiveFilterKind::MkIIBpf;
        check(profile->filterKind == expectedFilter, "model-specific legacy HPF or MkII BPF");
        check(maximumP1RateKsps(model) == (model == AnanModel::RedPitaya ? 384 : 192), "P1 model rate ceiling");
        for (const int wrong : {0, 6, 11, 99}) {
            check(!receiveProfile(wrong, model), "unrelated/unqualified wire ID cannot inherit model profile");
        }
    }
    //DH1KLM: The explicit override may cross every implemented ANAN board
    // family; its effective layout must equal the selected native profile.
    for (const int board : {1, 2, 3, 4, 5, 10, 20}) {
        for (const auto& entry : kAnanModels) {
            if (entry.model == AnanModel::Automatic || !receiveProfile(entry.nativeBoardId)) { continue; }
            const auto actual = receiveProfile(board, entry.model, true);
            const auto wanted = receiveProfile(entry.nativeBoardId, entry.model);
            if (actual) {
                antennaIsolation(*actual, entry.nativeBoardId != 10);
                receiveInputIsolation(*actual, entry.nativeBoardId != 10);
                adc1FilterIsolation(*actual);
            }
            check(actual && wanted && actual->numAdcs == wanted->numAdcs
                  && actual->firstDdcIndex == wanted->firstDdcIndex
                  && actual->filterKind == wanted->filterKind
                  && actual->physicalDdcLimit == wanted->physicalDdcLimit
                  && actual->swapSpeakerChannels == wanted->swapSpeakerChannels
                  && actual->saturnDroopDefaults == wanted->saturnDroopDefaults,
                  "board override uses the complete explicit model profile");
        }
    }
    check(!receiveProfile(5, AnanModel::Automatic, true), "override requires explicit model");
    check(!receiveProfile(5, static_cast<AnanModel>(99), true), "override rejects unknown model");
    for (const int board : {0, 6, 11, 99}) {
        check(!receiveProfile(board, AnanModel::Anan200D, true), "override cannot authorize unsupported physical board");
    }
    check(!receiveProfile(5, AnanModel::HermesLite2, true), "override cannot switch to HL2 P2");
    check(!ananModelInfo(static_cast<AnanModel>(17)), "LAST sentinel is not a model");
    check(!receiveProfile(5, static_cast<AnanModel>(99)), "unknown model rejected");
    check(!receiveProfile(6, AnanModel::HermesLite2), "HL2 P2 is excluded");
    check(!receiveProfile(11, AnanModel::AnanG2), "planned Saturn MkII cannot silently inherit G2");
    check(receiveProfile(1, AnanModel::Anan10E)->physicalDdcLimit == 2,
          "Thetis-compatible Hermes ID still uses selected Hermes II model layout");
    for (const int board : {1, 5}) {
        const auto rp = receiveProfile(board, AnanModel::RedPitaya);
        check(rp && rp->numAdcs == 2 && rp->firstDdcIndex == 2
            && rp->filterKind == ReceiveFilterKind::LegacyHpf, "Red Pitaya P2 overrides shared discovery identity");
    }
    check(receiveInputNames(AnanModel::Hermes,1)[1] == "RX1"
          && receiveInputNames(AnanModel::Anan100,1)[1] == "EXT2"
          && receiveInputNames(AnanModel::Anan200D,5)[1] == "EXT2"
          && receiveInputNames(AnanModel::RedPitaya,1)[1] == "BYPASS"
          && receiveInputNames(AnanModel::Anan7000,5)[1] == "BYPASS",
          "connector labels follow the mapped model rather than physical board");
    check(receiveInputNames(AnanModel::Automatic,1)[1] == "RX1"
          && receiveInputNames(AnanModel::Automatic,4)[1] == "EXT2"
          && receiveInputNames(AnanModel::Automatic,5)[1] == "BYPASS",
          "automatic connector labels follow the board family");
    p1::ReceiveConfig config;
    check(!p1::receiveControl(0x26, config), "Anvelina OC bank excluded by default");
    config.anvelinaExtraOc = true;
    check(p1::receiveControl(0x26, config) == p1::Control{0x26, 0, 0, 0, 0}, "Anvelina RX extra OC stays zero");
    std::cout << "ANAN model profiles passed\n";
}
