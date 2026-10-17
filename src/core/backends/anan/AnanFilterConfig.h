#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>

namespace AetherSDR::anan {
//DH1KLM: Thetis RX filter rows, in integer Hz. No TX relay or keying state.
struct FilterBand { std::uint32_t lowHz; std::uint32_t highHz; };
struct OcReceiveBand { std::uint32_t lowHz = 0; std::uint32_t highHz = 0; std::uint8_t mask = 0; };
struct FilterConfig {
    std::array<FilterBand, 6> receive;
    std::array<FilterBand, 7> lowPass;
    bool bypassAdc0 = false;
    bool bypassAdc1 = true;
    std::array<OcReceiveBand, 11> ocReceive{};
    bool ocEnabled = false;
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
    return 0;
}
inline std::uint32_t receiveFilterBits(const FilterConfig& config, double hz) noexcept
{
    constexpr std::array<int,6> bits{6,5,4,1,2,3};
    const int row = selectedFilterBand(config.receive, hz);
    return row < 0 ? 0 : std::uint32_t{1} << bits[row];
}
inline std::uint32_t lowPassFilterBits(const FilterConfig& config, double hz) noexcept
{
    constexpr std::array<int,7> bits{23,22,21,20,31,30,29};
    const int row = selectedFilterBand(config.lowPass, hz);
    return std::uint32_t{1} << bits[row < 0 ? 6 : row];
}
} // namespace AetherSDR::anan
