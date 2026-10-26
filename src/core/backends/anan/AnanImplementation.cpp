#include "AnanImplementation.h"
#include "AnanBackend.h"
#include "core/backends/anan_original/AnanBackend.h"
#include "core/backends/anan_original/AnanSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/AppSettings.h"
#include <QJsonObject>

namespace AetherSDR::anan {
namespace {
using OriginalSettings = anan_original::AnanSettings;
}

QString AnanImplementation::label(bool original)
{
    return original ? QStringLiteral("Saturn (ANAN G2) — tropo1234")
                    : QStringLiteral("Saturn (ANAN G2) — DH1KLM");
}

bool AnanImplementation::isSaturn(const RadioInfo& radio)
{
    return radio.family == QLatin1String("anan") && radio.boardId == 10 && radio.hpsdrProtocol == 2;
}

QList<RadioInfo> AnanImplementation::choices(const RadioInfo& radio)
{
    if (!isSaturn(radio)) { return {radio}; }
    RadioInfo extended = radio;
    extended.implementation.clear();
    RadioInfo original = radio;
    original.implementation = originalId();
    return {extended, original};
}

QString AnanImplementation::preferred(const QString& serial)
{
    const QJsonObject doc = RadioSettingsScope(QStringLiteral("anan"), serial)
        .feature(QStringLiteral("Implementation"));
    return doc.value(QStringLiteral("selected")).toString();
}

bool AnanImplementation::setPreferred(const QString& serial, const QString& implementation)
{
    if (!implementation.isEmpty() && implementation != originalId()) { return false; }
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    QJsonObject doc = scope.featureExact(QStringLiteral("Implementation"), &version, &status);
    if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc.insert(QStringLiteral("selected"), implementation);
    return scope.setFeature(QStringLiteral("Implementation"), 1, doc);
}

RadioInfo AnanImplementation::selected(const RadioInfo& radio)
{
    RadioInfo result = radio;
    if (isSaturn(radio)) { result.implementation = preferred(radio.serial); }
    return result;
}

QVariantMap AnanImplementation::originalOptions()
{
    return {{QStringLiteral("ddc0RateKsps"), OriginalSettings::ddc0RateKsps()},
            {QStringLiteral("ddc0AdcIndex"), OriginalSettings::ddc0AdcIndex()},
            {QStringLiteral("ditherEnabled"), OriginalSettings::ditherEnabled()},
            {QStringLiteral("randomEnabled"), OriginalSettings::randomEnabled()},
            {QStringLiteral("bypassAdc0Filters"), OriginalSettings::bypassAdc0Filters()},
            {QStringLiteral("bypassAdc1Filters"), OriginalSettings::bypassAdc1Filters()},
            {QStringLiteral("speakerAudioEnabled"), OriginalSettings::speakerAudioEnabled()},
            {QStringLiteral("bandFiltersEnabled"), OriginalSettings::bandFiltersEnabled()},
            {QStringLiteral("speakerAmpMuted"), OriginalSettings::speakerAmpMuted()}};
}

void AnanImplementation::populateParams(RadioConnectRequest& request, const QString& implementation)
{
    if (implementation != originalId()) { return; }
    // Replace, never inherit model overrides or extended front-end options.
    for (auto it = request.params.begin(); it != request.params.end();) {
        if (it.key().startsWith(QLatin1String("anan."))) { it = request.params.erase(it); }
        else { ++it; }
    }
    const QVariantMap options = originalOptions();
    for (auto it = options.begin(); it != options.end(); ++it) {
        request.params.insert(QStringLiteral("anan.") + it.key(), it.value());
    }
}

void AnanImplementation::setOriginalOption(const QString& name, const QVariant& value)
{
    if (name == QLatin1String("ddc0RateKsps")) {
        if (OriginalSettings::validDdc0RatesKsps().contains(value.toInt())) { OriginalSettings::setDdc0RateKsps(value.toInt()); }
    } else if (name == QLatin1String("ddc0AdcIndex")) {
        if (value.toInt() == 0 || value.toInt() == 1) { OriginalSettings::setDdc0AdcIndex(value.toInt()); }
    } else if (name == QLatin1String("ditherEnabled")) { OriginalSettings::setDitherEnabled(value.toBool()); }
    else if (name == QLatin1String("randomEnabled")) { OriginalSettings::setRandomEnabled(value.toBool()); }
    else if (name == QLatin1String("bypassAdc0Filters")) { OriginalSettings::setBypassAdc0Filters(value.toBool()); }
    else if (name == QLatin1String("bypassAdc1Filters")) { OriginalSettings::setBypassAdc1Filters(value.toBool()); }
    else if (name == QLatin1String("speakerAudioEnabled")) { OriginalSettings::setSpeakerAudioEnabled(value.toBool()); }
    else if (name == QLatin1String("bandFiltersEnabled")) { OriginalSettings::setBandFiltersEnabled(value.toBool()); }
    else if (name == QLatin1String("speakerAmpMuted")) { OriginalSettings::setSpeakerAmpMuted(value.toBool()); }
}

std::unique_ptr<IRadioBackend> AnanImplementation::makeBackend(const QString& implementation)
{
    if (implementation.isEmpty()) { return std::make_unique<AnanBackend>(); }
    if (implementation == originalId()) { return std::make_unique<anan_original::AnanBackend>(); }
    return {};
}
}
