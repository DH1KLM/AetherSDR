//DH1KLM: Source-pinned logical models, independent of sockets and GUI labels.
#include "core/backends/anan/P2Protocol.h"
#include <cstdlib>
#include <iostream>

using namespace AetherSDR::anan;
static void check(bool value, const char* claim)
{
    if (!value) { std::cerr << "FAIL: " << claim << '\n'; std::exit(1); }
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
    p1::ReceiveConfig config;
    check(!p1::receiveControl(0x26, config), "Anvelina OC bank excluded by default");
    config.anvelinaExtraOc = true;
    check(p1::receiveControl(0x26, config) == p1::Control{0x26, 0, 0, 0, 0}, "Anvelina RX extra OC stays zero");
    std::cout << "ANAN model profiles passed\n";
}
