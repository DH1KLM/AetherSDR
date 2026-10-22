#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <optional>

namespace AetherSDR::anan {
//DH1KLM: Thetis RX filter rows, in integer Hz. No TX relay or keying state.
struct FilterBand { std::uint32_t lowHz; std::uint32_t highHz; };
struct OcReceiveBand { std::uint32_t lowHz = 0; std::uint32_t highHz = 0; std::uint8_t mask = 0; };
//DH1KLM Stable row order: 160m..6m, 2m, GEN, WWV, XVTR. ANT and RX-only are independent.
struct ReceiveAntennaBand {
    int antenna = 1;
    int rxOnly = 0;
};
struct ReceiveAntennaBands {
    bool enabled = false;
    std::array<ReceiveAntennaBand, 15> rows{};
};
struct FilterConfig {
    std::array<FilterBand, 6> receive;
    std::array<FilterBand, 7> lowPass;
    bool bypassAdc0 = false;
    bool bypassAdc1 = true;
    std::array<OcReceiveBand, 11> ocReceive{};
    bool ocEnabled = false;
    std::array<bool, 6> receiveBypass{};
    bool disable6mLnaOnRx = false;
    bool lpfBypassOnRx = false;
    //DH1KLM: Thetis RX-only selector: 0=ANT bank, 1/2=external, 3=XVTR.
    int rxOnlyInput = 0;
    //DH1KLM: Old documents inherit ADC0 ranges until ADC1 is configured.
    std::optional<std::array<FilterBand, 6>> receiveAdc1{};
    //DH1KLM Absent in legacy documents; seed from the saved manual path before enabling.
    std::optional<ReceiveAntennaBands> antennaBands{};
    //DH1KLM Independent SWL masks; HF ranges take precedence, including unassigned HF pins.
    std::array<OcReceiveBand, 14> ocSwl{};
    //DH1KLM Independent manual DB9 outputs; only the four user bits are permitted.
    std::uint8_t userDigitalOutputs = 0;
};
inline constexpr FilterConfig defaultFilterConfig(bool mkII) noexcept
{
    return {mkII ? std::array<FilterBand, 6>{{
        {1500000,2099999}, {2100000,5499999}, {5500000,10999999},
        {11000000,21999999}, {22000000,34999999}, {35000000,61440000}}}
        : std::array<FilterBand, 6>{{
        {1800000,6499999}, {6500000,9499999}, {9500000,12999999},
        {13000000,19999999}, {20000000,49999999}, {50000000,61440000}}},
        {{{0,2500000}, {2500001,5000000}, {5000001,8000000},
          {8000001,16500000}, {16500001,24000000}, {24000001,35600000},
          {35600001,61440000}}}, false, true};
}
template<std::size_t N>
inline bool validFilterBands(const std::array<FilterBand, N>& rows) noexcept
{
    for (std::size_t n = 0; n < N; ++n) {
        if (rows[n].lowHz > rows[n].highHz || rows[n].highHz > 61440000
            || (n > 0 && rows[n].lowHz <= rows[n-1].highHz)) { return false; }
    }
    return true;
}
template<std::size_t N>
inline int selectedFilterBand(const std::array<FilterBand, N>& rows, double hz) noexcept
{
    for (std::size_t n = 0; n < N; ++n) {
        if (hz >= rows[n].lowHz - 0.02 && hz <= rows[n].highHz + 0.02) {
            return static_cast<int>(n);
        }
    }
    return -1;
}
//DH1KLM: RX OC routing is explicit operator configuration, never a model default.
inline std::uint8_t receiveOcMask(const FilterConfig& config, double hz) noexcept
{
    if (!config.ocEnabled || !std::isfinite(hz)) { return 0; }
    for (const auto& row : config.ocReceive) {
        if (row.lowHz < row.highHz && hz >= row.lowHz - 0.02 && hz <= row.highHz + 0.02) { return row.mask & 0x7f; }
    }
    for (const auto& row : config.ocSwl) {
        if (row.lowHz < row.highHz && hz >= row.lowHz - 0.02 && hz <= row.highHz + 0.02) { return row.mask & 0x7f; }
    }
    return 0;
}
inline int selectedReceiveFilterBand(const FilterConfig& config, double hz, bool adc1 = false) noexcept
{
    const int row = selectedFilterBand(adc1 && config.receiveAdc1 ? *config.receiveAdc1 : config.receive, hz);
    if (row < 0 || (!adc1 && (config.receiveBypass[row] || (row == 5 && config.disable6mLnaOnRx)))) { return -1; }
    return row;
}
inline std::uint32_t receiveFilterBits(const FilterConfig& config, double hz, bool adc1 = false) noexcept
{
    constexpr std::array<int,6> bits{6,5,4,1,2,3};
    const int row = selectedReceiveFilterBand(config, hz, adc1);
    return row < 0 ? 0 : std::uint32_t{1} << bits[row];
}
inline std::uint32_t lowPassFilterBits(const FilterConfig& config, double hz) noexcept
{
    constexpr std::array<int,7> bits{23,22,21,20,31,30,29};
    const int row = config.lpfBypassOnRx ? 6 : selectedFilterBand(config.lowPass, hz);
    return std::uint32_t{1} << bits[row < 0 ? 6 : row];
}
} // namespace AetherSDR::anan
