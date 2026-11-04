#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace AetherSDR::anan {

struct DiversitySettings {
    bool enabled = false;
    double gain = 1.0;
    double phaseDegrees = 0.0;
    int reference = 0;
    int output = 2; // ADC0, ADC1, or combined IQ.
    bool operator==(const DiversitySettings&) const = default;

    [[nodiscard]] bool valid() const noexcept
    {
        return std::isfinite(gain) && gain >= 0.0 && gain <= 5.0
            && std::isfinite(phaseDegrees) && phaseDegrees >= -180.0 && phaseDegrees <= 180.0
            && (reference == 0 || reference == 1) && output >= 0 && output <= 2;
    }
};

// Thetis wdsp/div.c xdiv: sum the reference IQ with a complex-scaled second
// input before demodulation. Inputs must already be paired by their wire frame;
// arrival order or independent DDC sequence numbers never establish coherence.
inline bool combineDiversity(std::span<const std::complex<float>> first,
                             std::span<const std::complex<float>> second,
                             const DiversitySettings& settings,
                             std::vector<std::complex<float>>& output)
{
    output.clear();
    if (!settings.valid() || first.empty() || first.size() > 4096
        || first.size() != second.size()) {
        return false;
    }
    const double angle = settings.phaseDegrees * std::numbers::pi / 180.0;
    const std::complex<float> rotation{static_cast<float>(settings.gain * std::cos(angle)),
                                       static_cast<float>(settings.gain * std::sin(angle))};
    output.reserve(first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        const std::complex<float> a = first[i];
        const std::complex<float> b = second[i];
        if (!std::isfinite(a.real()) || !std::isfinite(a.imag())
            || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
            output.clear();
            return false;
        }
        if (!settings.enabled || settings.output == 0) {
            output.push_back(a);
        } else if (settings.output == 1) {
            output.push_back(b);
        } else {
            output.push_back(settings.reference == 0 ? a + rotation * b : rotation * a + b);
        }
    }
    return true;
}

} // namespace AetherSDR::anan
