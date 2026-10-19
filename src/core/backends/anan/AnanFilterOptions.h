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
    QVariantList receiveBypass;
    for (bool on : config.receiveBypass) { receiveBypass.append(on); }
    QVariantList oc;
    for (const auto& band : config.ocReceive) {
        oc.append(QVariantMap{{QStringLiteral("lowHz"), band.lowHz},
            {QStringLiteral("highHz"), band.highHz}, {QStringLiteral("mask"), band.mask}});
    }
    QVariantMap options{{QStringLiteral("rxOnlyInput"), config.rxOnlyInput},
            {QStringLiteral("receiveBypass"), receiveBypass},
            {QStringLiteral("disable6mLnaOnRx"), config.disable6mLnaOnRx},
            {QStringLiteral("lpfBypassOnRx"), config.lpfBypassOnRx},
            {QStringLiteral("ocReceive"), oc}, {QStringLiteral("ocEnabled"), config.ocEnabled},
            {QStringLiteral("receive"), rows(config.receive)},
            {QStringLiteral("lowPass"), rows(config.lowPass)},
            {QStringLiteral("bypassAdc0"), config.bypassAdc0},
            {QStringLiteral("bypassAdc1"), config.bypassAdc1}};
    if (config.receiveAdc1) { options.insert(QStringLiteral("receiveAdc1"), rows(*config.receiveAdc1)); }
    return options;
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
    if (map.contains(QStringLiteral("rxOnlyInput"))) {
        const QVariant value = map.value(QStringLiteral("rxOnlyInput"));
        bool ok = false;
        const double input = value.toDouble(&ok);
        const int type = value.metaType().id();
        const bool numeric = type == QMetaType::Int || type == QMetaType::UInt
            || type == QMetaType::LongLong || type == QMetaType::ULongLong
            || type == QMetaType::Float || type == QMetaType::Double;
        if (!numeric || !ok || !std::isfinite(input) || input < 0 || input > 3
            || std::floor(input) != input) { return std::nullopt; }
        config.rxOnlyInput = static_cast<int>(input);
    }
    if (map.contains(QStringLiteral("receiveAdc1"))) {
        config.receiveAdc1.emplace();
        if (!read(QStringLiteral("receiveAdc1"), *config.receiveAdc1)) { return std::nullopt; }
    }
    if (map.contains(QStringLiteral("ocEnabled")) || map.contains(QStringLiteral("ocReceive"))) {
        if (map.value(QStringLiteral("ocEnabled")).metaType().id() != QMetaType::Bool) { return std::nullopt; }
        const auto rows = map.value(QStringLiteral("ocReceive")).toList();
        if (rows.size() != 11) { return std::nullopt; }
        std::uint32_t previousHigh = 0;
        bool previous = false;
        for (int n = 0; n < 11; ++n) {
            const auto row = rows[n].toMap();
            bool lowOk = false, highOk = false, maskOk = false;
            const double low = row.value(QStringLiteral("lowHz")).toDouble(&lowOk);
            const double high = row.value(QStringLiteral("highHz")).toDouble(&highOk);
            const double mask = row.value(QStringLiteral("mask")).toDouble(&maskOk);
            if (!lowOk || !highOk || !maskOk || !std::isfinite(low) || !std::isfinite(high)
                || !std::isfinite(mask) || low < 0 || low > high || high > 61440000
                || mask < 0 || mask > 127 || std::floor(low) != low || std::floor(high) != high
                || std::floor(mask) != mask || (mask != 0 && low == high)) { return std::nullopt; }
            if (mask != 0) {
                if (previous && low <= previousHigh) { return std::nullopt; }
                previous = true;
                previousHigh = static_cast<std::uint32_t>(high);
            }
            config.ocReceive[n] = {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high),
                                  static_cast<std::uint8_t>(mask)};
        }
        config.ocEnabled = map.value(QStringLiteral("ocEnabled")).toBool();
    }
    if (map.contains(QStringLiteral("receiveBypass"))) {
        const auto bypasses = map.value(QStringLiteral("receiveBypass")).toList();
        if (bypasses.size() != 6) { return std::nullopt; }
        for (int n = 0; n < 6; ++n) {
            if (bypasses[n].metaType().id() != QMetaType::Bool) { return std::nullopt; }
            config.receiveBypass[n] = bypasses[n].toBool();
        }
    }
    for (const auto& key : {QStringLiteral("disable6mLnaOnRx"), QStringLiteral("lpfBypassOnRx")}) {
        if (map.contains(key) && map.value(key).metaType().id() != QMetaType::Bool) { return std::nullopt; }
    }
    config.disable6mLnaOnRx = map.value(QStringLiteral("disable6mLnaOnRx"), false).toBool();
    config.lpfBypassOnRx = map.value(QStringLiteral("lpfBypassOnRx"), false).toBool();
    config.bypassAdc0 = map.value(QStringLiteral("bypassAdc0")).toBool();
    config.bypassAdc1 = map.value(QStringLiteral("bypassAdc1")).toBool();
    return config;
}
} // namespace AetherSDR::anan
