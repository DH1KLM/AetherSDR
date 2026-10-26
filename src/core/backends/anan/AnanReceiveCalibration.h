#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR::anan {
// Thetis NetworkIO scales oscillator tuning; display and meter use separate offsets.
struct ReceiveCalibration {
    double frequencyFactor = 1.0;
    std::array<double, 2> meterOffsetDb{};
    std::array<double, 2> displayOffsetDb{};
    std::array<bool, 2> levelCalibrated{};
};
inline bool validFrequencyFactor(double factor) noexcept
{
    return std::isfinite(factor) && factor >= 0.999 && factor <= 1.001;
}
inline bool validReceiveCalibration(const ReceiveCalibration& value) noexcept
{
    if (!validFrequencyFactor(value.frequencyFactor)) { return false; }
    for (int adc = 0; adc < 2; ++adc) {
        if (!std::isfinite(value.meterOffsetDb[adc]) || std::abs(value.meterOffsetDb[adc]) > 150
            || !std::isfinite(value.displayOffsetDb[adc]) || std::abs(value.displayOffsetDb[adc]) > 150) { return false; }
    }
    return true;
}
struct CalibrationPeak { double offsetHz; double levelDb; double snrDb; };
// FFT-shifted spectrum: bin N/2 is DC. Reject weak, ambiguous or edge peaks.
inline std::optional<CalibrationPeak> calibrationPeak(std::span<const float> bins, double rateHz,
                                                     double expectedOffsetHz = 0)
{
    if (bins.size() < 32 || !std::isfinite(rateHz) || rateHz <= 0
        || !std::isfinite(expectedOffsetHz)) { return std::nullopt; }
    const double width = rateHz / static_cast<double>(bins.size());
    const double center = static_cast<double>(bins.size()) / 2.0;
    int peak = -1;
    std::vector<float> noise;
    for (int n = 1; n < static_cast<int>(bins.size()) - 1; ++n) {
        if (!std::isfinite(bins[n])) { return std::nullopt; }
        if (std::abs((n-center)*width-expectedOffsetHz) <= 2500) {
            noise.push_back(bins[n]);
            if (peak < 0 || bins[n] > bins[peak]) { peak = n; }
        }
    }
    if (noise.size() < 9 || peak < 2 || peak >= static_cast<int>(bins.size())-2
        || std::abs((peak-center)*width-expectedOffsetHz) > 2500-width) { return std::nullopt; }
    std::sort(noise.begin(), noise.end());
    const double snr = bins[peak] - noise[noise.size()/2];
    if (snr < 20) { return std::nullopt; }
    for (int n = 1; n < static_cast<int>(bins.size())-1; ++n) {
        if (std::abs(n-peak) > 3 && std::abs((n-center)*width-expectedOffsetHz) <= 2500
            && bins[n] > bins[peak]-6) { return std::nullopt; }
    }
    const double curvature = bins[peak-1] - 2.0*bins[peak] + bins[peak+1];
    const double shift = curvature < -0.001
        ? std::clamp(0.5*(bins[peak-1]-bins[peak+1])/curvature, -0.5, 0.5) : 0.0;
    return CalibrationPeak{(peak+shift-center)*width, bins[peak]-0.25*(bins[peak-1]-bins[peak+1])*shift, snr};
}
// At a known carrier centred on the dial, Thetis' 1+offset/f also extends to a saved factor.
inline std::optional<double> calibratedFrequencyFactor(double previous, double referenceHz, double offsetHz) noexcept
{
    if (!validFrequencyFactor(previous) || !std::isfinite(referenceHz) || referenceHz < 100000
        || referenceHz > 61440000 || !std::isfinite(offsetHz) || std::abs(offsetHz) > 2500) { return std::nullopt; }
    const double factor = previous + offsetHz/referenceHz;
    return validFrequencyFactor(factor) ? std::optional<double>(factor) : std::nullopt;
}
} // namespace AetherSDR::anan
