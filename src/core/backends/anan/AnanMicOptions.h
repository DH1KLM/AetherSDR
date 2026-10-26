#pragma once
#include "AnanMicHardware.h"
#include <QVariantMap>
#include <optional>

namespace AetherSDR::anan {
inline QVariantMap micOptions(const MicOptions& options)
{
    return {{QStringLiteral("ring"), options.ring}, {QStringLiteral("bias"), options.bias},
            {QStringLiteral("ptt"), options.ptt}, {QStringLiteral("xlr"), options.xlr}};
}
inline std::optional<MicOptions> parseMicOptions(const QVariantMap& values)
{
    if (values.size() != 4) { return {}; }
    for (const char* key : {"ring", "bias", "ptt", "xlr"}) {
        if (values.value(QLatin1String(key)).metaType().id() != QMetaType::Bool) { return {}; }
    }
    return MicOptions{values.value(QStringLiteral("ring")).toBool(), values.value(QStringLiteral("bias")).toBool(),
                      values.value(QStringLiteral("ptt")).toBool(), values.value(QStringLiteral("xlr")).toBool()};
}
}
