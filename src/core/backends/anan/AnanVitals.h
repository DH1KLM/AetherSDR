#pragma once
#include "AnanModel.h"
#include <QVariantMap>
#include <cmath>
#include <algorithm>
#include <optional>

namespace AetherSDR::anan {
struct VitalConfig {
    bool diy = false;
    double offsetMv = 360;
    double sensitivityMvPerAmp = 120;
    double referenceVolts = 5;
    double voltageDivider = 23.0 / 1.1;
};
inline bool hasNativeVitals(AnanModel model, int board)
{
    if (model == AnanModel::Automatic) { return board == 10 || board == 20; }
    return model == AnanModel::Anan7000 || model == AnanModel::Anan8000
        || model == AnanModel::AnvelinaPro3
        || model == AnanModel::AnanG2 || model == AnanModel::AnanG21K || model == AnanModel::AnanG2E;
}
inline VitalConfig defaultVitals(AnanModel model, int board = -1)
{
    if (model == AnanModel::Automatic && board == 10) { model = AnanModel::AnanG2; }
    if (model == AnanModel::Anan7000 || model == AnanModel::AnvelinaPro3 || model == AnanModel::RedPitaya) {
        return {false, 340, 88};
    }
    if (model == AnanModel::AnanG2 || model == AnanModel::AnanG21K) { return {false, 0.001, 66.23}; }
    return {};
}
inline std::optional<VitalConfig> parseVitals(const QVariantMap& values, AnanModel model, int board = -1)
{
    VitalConfig config = defaultVitals(model, board);
    if (values.isEmpty()) { return config; }
    if (values.value(QStringLiteral("diy")).metaType().id() != QMetaType::Bool) { return std::nullopt; }
    config.diy = values.value(QStringLiteral("diy")).toBool();
    const auto number = [&](const char* key, double& result) {
        const QVariant value = values.value(QLatin1String(key));
        if (value.metaType().id() != QMetaType::Double && value.metaType().id() != QMetaType::Int) { return false; }
        result = value.toDouble(); return std::isfinite(result);
    };
    if (!number("offsetMv", config.offsetMv) || !number("sensitivityMvPerAmp", config.sensitivityMvPerAmp)
        || config.offsetMv < 0 || config.offsetMv > 5000
        || config.sensitivityMvPerAmp < 0.001 || config.sensitivityMvPerAmp > 5000) { return std::nullopt; }
    if (!number("referenceVolts", config.referenceVolts) || !number("voltageDivider", config.voltageDivider)
        || config.referenceVolts < 0.1 || config.referenceVolts > 10
        || config.voltageDivider < 0.01 || config.voltageDivider > 1000) { return std::nullopt; }
    return config;
}
inline QVariantMap vitalOptions(const VitalConfig& config)
{
    return {{QStringLiteral("diy"), config.diy}, {QStringLiteral("offsetMv"), config.offsetMv},
        {QStringLiteral("sensitivityMvPerAmp"), config.sensitivityMvPerAmp},
        {QStringLiteral("referenceVolts"), config.referenceVolts}, {QStringLiteral("voltageDivider"), config.voltageDivider}};
}
inline std::optional<double> paCurrentAmps(double counts, const VitalConfig& config)
{
    if (!std::isfinite(counts) || counts < 0 || counts > 4095 || !std::isfinite(config.sensitivityMvPerAmp)
        || config.sensitivityMvPerAmp < 0.001 || config.sensitivityMvPerAmp > 5000
        || !std::isfinite(config.offsetMv) || config.offsetMv < 0 || config.offsetMv > 5000
        || !std::isfinite(config.referenceVolts) || config.referenceVolts < 0.1 || config.referenceVolts > 10) { return std::nullopt; }
    return std::max(0.0, (counts * config.referenceVolts * 1000.0 / 4095.0 - config.offsetMv) / config.sensitivityMvPerAmp);
}
} // namespace AetherSDR::anan
