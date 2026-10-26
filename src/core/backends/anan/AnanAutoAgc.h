#pragma once

#include <cmath>
#include <algorithm>
#include <optional>
#include <span>

namespace AetherSDR::anan {
// A fresh, smoothed per-bin floor, referred to WDSP's fixed 4096-point reference.
class AnanAutoAgc {
public:
    static constexpr int kReferencePoints = 4096;
    static constexpr double kDefaultOffsetDb = 20.0;
    static constexpr double kSettleMs = 2000.0;
    static constexpr double kUpdateMs = 500.0;

    void reset() { m_floor.reset(); m_settledMs = 0.0; m_updateMs = 0.0; }
    static std::optional<double> referenceFloor(std::span<const float> bins)
    {
        if (bins.size() < 16) { return {}; }
        const std::size_t edge = bins.size() / 20;
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = edge; i < bins.size() - edge; ++i) {
            const float value = bins[i];
            if (!std::isfinite(value) || value <= -220.0f || value > 20.0f) { return {}; }
            if (i != bins.size() / 2) { sum += value; ++count; }
        }
        return sum / static_cast<double>(count)
            + 10.0 * std::log10(static_cast<double>(bins.size()) / kReferencePoints);
    }
    std::optional<double> feed(std::span<const float> bins, double elapsedMs, double offsetDb)
    {
        const auto floor = referenceFloor(bins);
        if (!floor || !std::isfinite(elapsedMs) || elapsedMs <= 0.0 || elapsedMs > 1500.0
            || !std::isfinite(offsetDb) || offsetDb < -60.0 || offsetDb > 60.0) {
            reset();
            return {};
        }
        if (!m_floor) { m_floor = floor; return {}; }
        *m_floor += (1.0 - std::exp(-elapsedMs / kSettleMs)) * (*floor - *m_floor);
        m_settledMs += elapsedMs;
        m_updateMs += elapsedMs;
        if (m_settledMs < kSettleMs || m_updateMs < kUpdateMs) { return {}; }
        m_updateMs = 0.0;
        return std::clamp(*m_floor + offsetDb, -160.0, 2.0);
    }
private:
    std::optional<double> m_floor;
    double m_settledMs = 0.0;
    double m_updateMs = 0.0;
};
}
