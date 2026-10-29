#pragma once
#include "AnanModel.h"
#include <cmath>
#include <algorithm>
#include <optional>

namespace AetherSDR::anan {
struct RfPowerReading {
    double forwardWatts;
    double reflectedWatts;
    std::optional<double> swr;
};
// Thetis console.cs computeAlexFwdPower/computeRefPower detector constants.
// These reference curves do not replace per-unit calibration.
inline std::optional<RfPowerReading> rfPowerReading(AnanModel model, int board,
    int forward, int reverse, double frequencyHz)
{
    if (forward < 0 || forward > 4095 || reverse < 0 || reverse > 4095
        || !std::isfinite(frequencyHz) || frequencyHz < 0 || frequencyHz > 61'440'000) { return std::nullopt; }
    if (model == AnanModel::Automatic) {
        switch (board) {
        case 3: model = AnanModel::Anan100D; break;
        case 4: model = AnanModel::Anan200D; break;
        case 20: model = AnanModel::AnanG2E; break;
        default: return std::nullopt; // Shared board IDs cannot select a PA detector.
        }
    }
    const bool sixMetres = frequencyHz >= 50'000'000 && frequencyHz <= 54'000'000;
    double reference = 3.3, forwardBridge = 0.09, reverseBridge = 0.09;
    int forwardOffset = 6, reverseOffset = 3;
    switch (model) {
    case AnanModel::Anan100: case AnanModel::Anan100B: case AnanModel::Anan100D:
        forwardBridge = 0.095; reverseBridge = sixMetres ? 0.5 : 0.095; break;
    case AnanModel::Anan200D:
        reference = 5; forwardBridge = 0.108; reverseBridge = sixMetres ? 0.5 : 0.108;
        forwardOffset = 4; reverseOffset = 2; break;
    case AnanModel::Anan7000: case AnanModel::AnanG2: case AnanModel::AnanG2E:
    case AnanModel::AnvelinaPro3: case AnanModel::RedPitaya:
        reference = 5; forwardBridge = 0.12; reverseBridge = sixMetres ? 0.7 : 0.15;
        forwardOffset = 32; reverseOffset = 28; break;
    case AnanModel::OrionMkII: case AnanModel::Anan8000:
        reference = 5; forwardBridge = 0.08; reverseBridge = 0.08;
        forwardOffset = 18; reverseOffset = 16; break;
    case AnanModel::Hpsdr: case AnanModel::Hermes: case AnanModel::Anan10: case AnanModel::Anan10E: break;
    default: return std::nullopt;
    }
    const auto watts = [reference](int raw, int offset, double bridge) {
        const double volts = std::max(0, raw - offset) * reference / 4095.0;
        return volts * volts / bridge;
    };
    RfPowerReading result{watts(forward, forwardOffset, forwardBridge), watts(reverse, reverseOffset, reverseBridge), {}};
    if (result.forwardWatts >= 0.1) {
        const double reflection = std::sqrt(result.reflectedWatts / result.forwardWatts);
        result.swr = reflection < 1 ? (1 + reflection) / (1 - reflection) : 99.0;
    }
    return result;
}
inline double rfWattsToDbm(double watts)
{
    return watts > 0 ? 10 * std::log10(watts * 1000) : -100.0;
}
} // namespace AetherSDR::anan
