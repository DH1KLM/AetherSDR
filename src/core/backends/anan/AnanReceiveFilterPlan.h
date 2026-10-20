#pragma once
#include "core/backends/anan/AnanFilterConfig.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>

namespace AetherSDR::anan {
struct ReceiveFilterDemand {
    int adc;
    double frequencyHz;
    double centerHz = -1; // No separate capture centre supplied.
};
// Computed receive state, separate from the operator's persisted FilterConfig.
struct ReceiveFilterPlan {
    std::array<std::optional<std::uint32_t>, 2> receiveBits{};
    std::array<int, 2> selections{};
    std::array<bool, 2> combined{};
    std::optional<double> lowPassHz;
    int lowPassSelections = 0;
};
inline ReceiveFilterPlan planReceiveFilters(const FilterConfig& config,
                                            std::span<const ReceiveFilterDemand> demands,
                                            bool combineBandFilters = false)
{
    ReceiveFilterPlan plan;
    std::array<unsigned, 2> rows{};
    std::array<std::uint32_t, 2> bits{};
    unsigned lowPassRows = 0;
    for (const auto& demand : demands) {
        if (demand.adc < 0 || demand.adc > 1 || !std::isfinite(demand.frequencyHz) ||
            demand.frequencyHz < 0 || demand.frequencyHz > 61440000) {
            continue;
        }
        const int adc = demand.adc;
        const int row = selectedReceiveFilterBand(config, demand.frequencyHz, adc == 1);
        rows[adc] |= 1u << (row < 0 ? 6 : row);
        bits[adc] |= receiveFilterBits(config, demand.frequencyHz, adc == 1);
        if (adc == 0) {
            // Nereus SharedInputLowPass::HighestCentre: CTUN shifts a VFO,
            // while the shared analogue low-pass follows the capture's DDS.
            const double center = demand.centerHz >= 0 && demand.centerHz <= 61440000 &&
                std::isfinite(demand.centerHz) ? demand.centerHz : demand.frequencyHz;
            if (!plan.lowPassHz || center > *plan.lowPassHz) {
                plan.lowPassHz = center;
            }
            const int low = selectedFilterBand(config.lowPass, center);
            lowPassRows |= 1u << (low < 0 ? 7 : low);
        }
    }
    for (int adc = 0; adc < 2; ++adc) {
        plan.selections[adc] = std::popcount(rows[adc]);
        if (plan.selections[adc] == 0) {
            continue;
        }
        if (plan.selections[adc] > 1 && !combineBandFilters) {
            plan.receiveBits[adc] = 0; // Different requirements on one physical preselector.
        } else {
            auto selection = bits[adc];
            // Nereus AlexFilterMap, 9a20b6b, combineRxBandSelections:
            // native Anvelina combines rows; keep 6 m LNA out of the 12/10 m row.
            if (combineBandFilters && (selection & (1u << 3))) {
                selection &= ~(1u << 2);
            }
            plan.receiveBits[adc] = selection;
            plan.combined[adc] = plan.selections[adc] > 1 && selection != 0;
        }
    }
    plan.lowPassSelections = std::popcount(lowPassRows);
    return plan;
}
} // namespace AetherSDR::anan
