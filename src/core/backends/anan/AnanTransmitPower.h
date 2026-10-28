#pragma once

#include "AnanModel.h"

#include <QVariantMap>
#include <QVariantList>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace AetherSDR::anan {
inline constexpr std::array<const char*,11> kPaBands{
    "160 m","80 m","60 m","40 m","30 m","20 m","17 m","15 m","12 m","10 m","6 m"};
struct TransmitPowerProfile {
    std::array<double,11> gainsDb{};
    std::array<double,11> limitsWatts{};
};
inline std::optional<TransmitPowerProfile> defaultTransmitPower(AnanModel model)
{
    TransmitPowerProfile profile;
    // Thetis clsHardwareSpecific.DefaultPAGainsForBands; no measured claim.
    switch (model) {
    case AnanModel::Hermes: case AnanModel::Anan10: case AnanModel::Anan10E:
    case AnanModel::OrionMkII:
        profile.gainsDb = {41,41.2,41.3,41.3,41,40.5,39.9,38.8,38.8,38.8,38.8}; break;
    case AnanModel::Anan100: case AnanModel::Anan100B: case AnanModel::Anan8000:
        profile.gainsDb = {50,50.5,50.5,50,49.5,48.5,48,47.5,46.5,42,43}; break;
    case AnanModel::Anan100D: case AnanModel::Anan200D:
        profile.gainsDb = {49.5,50.5,50.5,50,49,48,47,46.5,46,43.5,43}; break;
    case AnanModel::Anan7000: case AnanModel::AnanG2: case AnanModel::AnanG2E:
    case AnanModel::AnanG21K: case AnanModel::AnvelinaPro3: case AnanModel::RedPitaya:
        profile.gainsDb = {47.9,50.5,50.8,50.8,50.9,50.9,50.5,47,47.9,46.5,44.6}; break;
    default: return std::nullopt;
    }
    return profile;
}
inline QVariantMap transmitPowerOptions(const TransmitPowerProfile& profile)
{
    QVariantList gains, limits;
    for (double value : profile.gainsDb) { gains.append(value); }
    for (double value : profile.limitsWatts) { limits.append(value); }
    return {{QStringLiteral("gainsDb"),gains}, {QStringLiteral("limitsWatts"),limits}};
}
inline std::optional<TransmitPowerProfile> parseTransmitPower(const QVariantMap& options)
{
    if (options.size() != 2 || options.value(QStringLiteral("gainsDb")).metaType().id() != QMetaType::QVariantList
        || options.value(QStringLiteral("limitsWatts")).metaType().id() != QMetaType::QVariantList) { return {}; }
    const auto gains = options.value(QStringLiteral("gainsDb")).toList();
    const auto limits = options.value(QStringLiteral("limitsWatts")).toList();
    if (gains.size() != 11 || limits.size() != 11) { return {}; }
    TransmitPowerProfile profile;
    for (std::size_t i = 0; i < 11; ++i) {
        const auto number = [](const QVariant& value) {
            const int type = value.metaType().id();
            return type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
                || type == QMetaType::UInt || type == QMetaType::LongLong || type == QMetaType::ULongLong;
        };
        if (!number(gains[i]) || !number(limits[i])) { return {}; }
        const double gain = gains[i].toDouble(), limit = limits[i].toDouble();
        if (!std::isfinite(gain) || gain < 0 || gain > 100
            || !std::isfinite(limit) || limit < 0 || limit > 2000) { return {}; }
        profile.gainsDb[i] = gain; profile.limitsWatts[i] = limit;
    }
    return profile;
}
struct TransmitPowerPreview { double watts; double audioVolume; int drive; };
inline std::optional<TransmitPowerPreview> previewTransmitPower(
    const TransmitPowerProfile& profile, int band, double requestedWatts)
{
    if (band < 0 || band >= 11 || !std::isfinite(requestedWatts) || requestedWatts < 0 || requestedWatts > 2000
        || !parseTransmitPower(transmitPowerOptions(profile))) { return {}; }
    const double watts = std::min(requestedWatts, profile.limitsWatts[band]);
    if (watts == 0) { return TransmitPowerPreview{0,0,0}; }
    // Thetis SetPowerUsingTargetDBM -> Audio.RadioVolume -> NetworkIO.SetOutputPower.
    const double dbm = 10 * std::log10(watts * 1000) - profile.gainsDb[band];
    const double volume = std::min(std::sqrt(std::pow(10.0,dbm * 0.1) * 0.05) / 0.8, 1.0);
    return TransmitPowerPreview{watts, volume, static_cast<int>(255 * std::min(volume * 1.02,1.0))};
}

inline std::optional<TransmitPowerPreview> transmitPowerForPercent(
    const TransmitPowerProfile& profile, int band, int percent)
{
    if (band < 0 || band >= 11 || percent < 0 || percent > 100) { return {}; }
    return previewTransmitPower(profile,band,profile.limitsWatts[band] * percent / 100.0);
}
} // namespace AetherSDR::anan
