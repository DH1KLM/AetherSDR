#pragma once
#include "AnanFilterConfig.h"
#include <QVariantMap>
#include <QVariantList>
#include <optional>

namespace AetherSDR::anan {
inline QVariantMap filterOptions(const FilterConfig& config)
{
    const auto rows = [](const auto& bands) {
        QVariantList list;
        for (const FilterBand& band : bands) {
            list.append(QVariantMap{{QStringLiteral("lowHz"), band.lowHz},
                                    {QStringLiteral("highHz"), band.highHz}});
        }
        return list;
    };
    return {{QStringLiteral("receive"), rows(config.receive)},
            {QStringLiteral("lowPass"), rows(config.lowPass)},
            {QStringLiteral("bypassAdc0"), config.bypassAdc0},
            {QStringLiteral("bypassAdc1"), config.bypassAdc1}};
}
inline std::optional<FilterConfig> parseFilterOptions(const QVariantMap& map)
{
    FilterConfig config{};
    const auto read = [&map](const QString& key, auto& bands) {
        const QVariantList rows = map.value(key).toList();
        if (rows.size() != static_cast<int>(bands.size())) { return false; }
        for (std::size_t n = 0; n < bands.size(); ++n) {
            const QVariantMap row = rows[static_cast<int>(n)].toMap();
            bool lowOk = false, highOk = false;
            const double low = row.value(QStringLiteral("lowHz")).toDouble(&lowOk);
            const double high = row.value(QStringLiteral("highHz")).toDouble(&highOk);
            if (!lowOk || !highOk || !std::isfinite(low) || !std::isfinite(high)
                || low < 0 || high > 61440000 || low > high
                || std::floor(low) != low || std::floor(high) != high) { return false; }
            bands[n] = {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high)};
        }
        return validFilterBands(bands);
    };
    if (!read(QStringLiteral("receive"), config.receive)
        || !read(QStringLiteral("lowPass"), config.lowPass)
        || map.value(QStringLiteral("bypassAdc0")).metaType().id() != QMetaType::Bool
        || map.value(QStringLiteral("bypassAdc1")).metaType().id() != QMetaType::Bool) {
        return std::nullopt;
    }
    config.bypassAdc0 = map.value(QStringLiteral("bypassAdc0")).toBool();
    config.bypassAdc1 = map.value(QStringLiteral("bypassAdc1")).toBool();
    return config;
}
} // namespace AetherSDR::anan
