#pragma once
#include "AnanReceiveCalibration.h"
#include <QVariantMap>
#include <QVariantList>
#include <QMetaType>
#include <QString>

namespace AetherSDR::anan {
inline QVariantMap calibrationOptions(const ReceiveCalibration& value)
{
    QVariantList adcs;
    for (int n = 0; n < 2; ++n) {
        adcs.append(QVariantMap{{QStringLiteral("meterDb"), value.meterOffsetDb[n]},
            {QStringLiteral("displayDb"), value.displayOffsetDb[n]},
            {QStringLiteral("calibrated"), value.levelCalibrated[n]}});
    }
    return {{QStringLiteral("frequencyFactor"), value.frequencyFactor}, {QStringLiteral("adcs"), adcs}};
}
inline std::optional<ReceiveCalibration> parseCalibrationOptions(const QVariantMap& map)
{
    ReceiveCalibration value;
    const auto number = [](const QVariant& field) -> std::optional<double> {
        const int type = field.metaType().id();
        if (type != QMetaType::Double && type != QMetaType::Int && type != QMetaType::LongLong
            && type != QMetaType::UInt && type != QMetaType::ULongLong) { return std::nullopt; }
        const double result = field.toDouble();
        return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
    };
    const auto factor = number(map.value(QStringLiteral("frequencyFactor")));
    const auto adcs = map.value(QStringLiteral("adcs")).toList();
    if (!factor || adcs.size() != 2) { return std::nullopt; }
    value.frequencyFactor = *factor;
    for (int n = 0; n < 2; ++n) {
        const auto row = adcs[n].toMap();
        const auto meter = number(row.value(QStringLiteral("meterDb")));
        const auto display = number(row.value(QStringLiteral("displayDb")));
        if (!meter || !display || row.value(QStringLiteral("calibrated")).metaType().id() != QMetaType::Bool) { return std::nullopt; }
        value.meterOffsetDb[n] = *meter;
        value.displayOffsetDb[n] = *display;
        value.levelCalibrated[n] = row.value(QStringLiteral("calibrated")).toBool();
    }
    return validReceiveCalibration(value) ? std::optional<ReceiveCalibration>(value) : std::nullopt;
}
} //DH1KLM namespace AetherSDR::anan
