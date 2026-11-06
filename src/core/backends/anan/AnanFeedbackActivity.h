#pragma once
#include <cmath>
#include <cstdint>

namespace AetherSDR::anan {
// Presentation only: CALCC continues receiving blocks during pauses.
class AnanFeedbackActivity final {
public:
    void reset() noexcept { m_lastSignalMs = -1; }
    void observe(double txPeak, double feedbackPeak, double referenceFullScale, std::int64_t nowMs) noexcept
    {
        if (nowMs < 0 || !std::isfinite(txPeak) || !std::isfinite(feedbackPeak)
            || !std::isfinite(referenceFullScale) || referenceFullScale <= 0) { reset(); return; }
        if (txPeak >= referenceFullScale * 0.01 && feedbackPeak > 1e-7) { m_lastSignalMs = nowMs; }
    }
    [[nodiscard]] bool active(std::int64_t nowMs) const noexcept
    { return m_lastSignalMs >= 0 && nowMs >= m_lastSignalMs && nowMs-m_lastSignalMs <= 250; }
private:
    std::int64_t m_lastSignalMs = -1;
};
}
