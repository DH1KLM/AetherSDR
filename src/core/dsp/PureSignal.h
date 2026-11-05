#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace AetherSDR {

struct PureSignalSettings {
    bool autoAttenuate = false;
    double moxWaitSeconds = 0.2;
    double calibrationWaitSeconds = 0.0;
    double amplifierDelayNs = 150.0;
    double hardwarePeak = 0.4072;

    [[nodiscard]] bool valid() const noexcept
    {
        return std::isfinite(moxWaitSeconds) && moxWaitSeconds >= 0 && moxWaitSeconds <= 5
            && std::isfinite(calibrationWaitSeconds) && calibrationWaitSeconds >= 0 && calibrationWaitSeconds <= 5
            && std::isfinite(amplifierDelayNs) && amplifierDelayNs >= 0 && amplifierDelayNs <= 1000000
            && std::isfinite(hardwarePeak) && hardwarePeak >= 0.01 && hardwarePeak <= 1.0;
    }
};

enum class PureSignalMode { Off, Automatic, Single, Current };

struct PureSignalSnapshot {
    std::array<int, 16> information{};
    double maximumTransmit = 0;
    double hardwarePeak = 0;
    bool correctionRunning = false;
    bool correctionBusy = false;
    bool correctionAvailable = false;
    // Bundled WDSP calcc.c: sixteen buckets of 256 observations, 512 curve points.
    static constexpr int kSamples = 4096;
    static constexpr int kCurvePoints = 512;
    std::array<std::vector<double>, 4> measurements;
    std::array<std::vector<double>, 4> correction;
    double phaseReferenceDegrees = 0;
};

} // namespace AetherSDR
