#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace AetherSDR::anan {
class AnanCarrier final {
public:
    void key(bool down) noexcept
    {
        if (down == m_down) { return; }
        m_down = down; m_start = m_level; m_position = 0;
    }
    std::complex<float> sample(int rate) noexcept
    {
        const double duration = std::max(1.0,0.005 * rate);
        m_position = std::min(duration,m_position + 1);
        const double weight = (1 - std::cos(std::numbers::pi * m_position / duration)) * 0.5;
        m_level = m_start + ((m_down ? 1.0 : 0.0) - m_start) * weight;
        return {static_cast<float>(0.5 * m_level),0};
    }
private:
    bool m_down = false;
    double m_level = 0, m_start = 0, m_position = 0;
};
} // namespace AetherSDR::anan
