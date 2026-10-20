#include "core/backends/anan/P2Protocol.h"
#include <cstdlib>
#include <iostream>
using namespace AetherSDR::anan;
void check(bool ok, const char* reason) { if (!ok) { std::cerr << reason << '\n'; std::exit(1); } }
int main()
{
    auto config = defaultFilterConfig(false);
    const std::array<ReceiveFilterDemand, 2> split{{{0, 7100000}, {0, 14200000}}};
    auto plan = planReceiveFilters(config, split);
    check(plan.receiveBits[0] == 0 && plan.selections[0] == 2 && plan.lowPassHz == 14200000,
          "40m + 20m bypasses shared HPF and uses highest-frequency LPF");
    check(!config.bypassAdc0 && !config.lpfBypassOnRx, "operator switches remain off");
    const std::array<std::uint32_t, 2> words{phaseWord(7100000), phaseWord(14200000)};
    const auto hp = buildHighPriority(true, words, false, true, 0, 0, 1, 2, true,
        ReceiveFilterKind::LegacyHpf, 2, &config, 0, &plan);
    auto word = [](const auto& p, int n) { return std::uint32_t(p[n]) << 24 | std::uint32_t(p[n+1]) << 16 | std::uint32_t(p[n+2]) << 8 | p[n+3]; };
    check((word(hp,1432) & (1u << 12)) && (word(hp,1432) & (1u << 20)) &&
          !(word(hp,1432) & (1u << 21)) && hp[4] == 1,
          "wire uses RX bypass and 20m LPF without a 40m LPF or PTT");
    const std::array<ReceiveFilterDemand, 2> same{{{0, 7100000}, {0, 7200000}}};
    plan = planReceiveFilters(config, same);
    check(plan.selections[0] == 1 && plan.receiveBits[0] == receiveFilterBits(config, 7100000),
          "same filter remains selected for two slices");
    const std::array<ReceiveFilterDemand, 2> separate{{{0,7100000},{1,14200000}}};
    plan = planReceiveFilters(config, separate);
    check(plan.selections[0] == 1 && plan.selections[1] == 1 && plan.lowPassHz == 7100000,
          "different ADCs never widen each other's filters or ADC0 low-pass");
    plan = planReceiveFilters(config, std::span(split).first(1));
    check(plan.receiveBits[0] == receiveFilterBits(config,7100000) && plan.lowPassHz == 7100000,
          "closing the high-band slice restores normal filtering");
    const std::array<ReceiveFilterDemand, 1> ctun{{{0,16510000,16490000}}};
    plan = planReceiveFilters(config, ctun);
    check(plan.lowPassHz == 16490000 && selectedFilterBand(config.lowPass,*plan.lowPassHz) == 3,
          "CTUN low-pass follows DDC centre rather than displaced VFO across an LPF edge");
    config = defaultFilterConfig(true);
    plan = planReceiveFilters(config, split, true);
    check(plan.combined[0] && plan.receiveBits[0] == ((1u << 4) | (1u << 1)),
          "native combining hardware selects both required BPFs");
    const std::array<ReceiveFilterDemand, 2> six{{{0,28000000},{0,50100000}}};
    plan = planReceiveFilters(config,six,true);
    check(plan.receiveBits[0] == (1u << 3), "native 6m LNA excludes incompatible 12/10m filter");
    const std::array<ReceiveFilterDemand, 2> general{{{0,5000000},{0,15000000}}};
    plan = planReceiveFilters(config,general);
    check(plan.selections[0] == 2 && plan.receiveBits[0] == 0,
          "WWV/general-coverage frequencies use actual filter selections, not band names");
    config.lpfBypassOnRx = true;
    plan = planReceiveFilters(config,split);
    const auto forced = buildHighPriority(true,words,true,true,0,0,1,2,true,
        ReceiveFilterKind::MkIIBpf,2,&config,0,&plan);
    check((word(forced,1432)&(1u<<12)) && (word(forced,1432)&(1u<<29)),
          "manual bypass controls retain priority");
    std::cout << "ANAN automatic receive filter checks passed\n";
}
