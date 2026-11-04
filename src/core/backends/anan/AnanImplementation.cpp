#include "AnanImplementation.h"
#include "AnanBackend.h"
#include "AnanSettings.h"
#include "P2Protocol.h"
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

QList<AnanImplementation::ManualProfileChoice> AnanImplementation::manualProfileChoices()
{
    // This operating-model picker excludes Atlas/Metis, the separately handled
    // HL2, the Orion board designation and G2 1K. Wire recognition is independent.
    return {
        {static_cast<int>(AnanModel::Automatic), QStringLiteral("Automatic physical-board profile")},
        {static_cast<int>(AnanModel::Hermes), QStringLiteral("HERMES")},
        {static_cast<int>(AnanModel::Anan10), QStringLiteral("ANAN-10")},
        {static_cast<int>(AnanModel::Anan10E), QStringLiteral("ANAN-10E")},
        {static_cast<int>(AnanModel::Anan100), QStringLiteral("ANAN-100")},
        {static_cast<int>(AnanModel::Anan100B), QStringLiteral("ANAN-100B")},
        {static_cast<int>(AnanModel::Anan100D), QStringLiteral("ANAN-100D")},
        {static_cast<int>(AnanModel::Anan200D), QStringLiteral("ANAN-200D")},
        {static_cast<int>(AnanModel::Anan7000), QStringLiteral("ANAN-7000DLE")},
        {static_cast<int>(AnanModel::Anan8000), QStringLiteral("ANAN-8000DLE")},
        {static_cast<int>(AnanModel::AnanG2E), QStringLiteral("ANAN-G2E")},
        {static_cast<int>(AnanModel::AnanG2), QStringLiteral("ANAN-G2")},
        {static_cast<int>(AnanModel::AnvelinaPro3), QStringLiteral("ANVELINA-Pro III")},
        {static_cast<int>(AnanModel::RedPitaya), QStringLiteral("RED-PITAYA")}
    };
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

AnanImplementation::ManualSelectionResult AnanImplementation::selectForManualConnect(
    const RadioInfo& radio, const ManualSelection& selection)
{
    ManualSelectionResult result;
    result.radio = radio;
    if (selection.protocol > 0 && selection.protocol != radio.hpsdrProtocol) {
        result.status = ManualSelectionStatus::IgnoreProtocol;
        return result;
    }
    const bool originalChoice = selection.profileId == -200
        || (selection.profileId == -100 && isSaturn(radio) && preferred(radio.serial) == originalId());
    if (originalChoice) {
        if (!isSaturn(radio)) {
            result.status = ManualSelectionStatus::OriginalUnavailable;
            return result;
        }
        result.radio.implementation = originalId();
    } else {
        const AnanSettings::RadioProfile saved = AnanSettings::radioProfile(radio.serial);
        const AnanModel model = selection.profileId == -100
            ? saved.model : static_cast<AnanModel>(selection.profileId);
        const int protocol = selection.protocol < 0 ? saved.protocol : selection.protocol;
        const bool overrideDetection = selection.overrideDetection.value_or(saved.overrideDetection);
        if (protocol > 0 && protocol != radio.hpsdrProtocol) {
            result.status = ManualSelectionStatus::IgnoreProtocol;
            return result;
        }
        if ((!saved.valid && (selection.profileId == -100 || selection.protocol < 0
                             || !selection.overrideDetection.has_value()))
            || !receiveProfile(radio.boardId, model, overrideDetection)) {
            result.status = overrideDetection ? ManualSelectionStatus::InvalidOverride
                                              : ManualSelectionStatus::InvalidProfile;
            return result;
        }
        if (selection.profileId != -100 || selection.protocol >= 0 || selection.overrideDetection.has_value()) {
            if (!AnanSettings::setRadioProfile(radio.serial, model, protocol, overrideDetection)) {
                result.status = ManualSelectionStatus::ProfileSaveFailed;
                return result;
            }
            // A successful profile write consumes the explicit selection even
            // if the separate implementation preference cannot be saved.
            result.resetChoices = true;
        }
    }
    if (isSaturn(radio) && !setPreferred(radio.serial, result.radio.implementation)) {
        result.status = ManualSelectionStatus::ImplementationSaveFailed;
        return result;
    }
    result.status = ManualSelectionStatus::Accepted;
    result.resetChoices = true;
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

void AnanImplementation::populateParams(RadioConnectRequest& request, const RadioInfo& radio)
{
    if (radio.family.compare(QLatin1String("anan"), Qt::CaseInsensitive) != 0) {
        return;
    }
    populateParams(request, radio.implementation);
    // Identity follows implementation options so the original variant's reset
    // cannot discard the physical board and discovered protocol.
    if (radio.boardId >= 0) {
        request.params.insert(QStringLiteral("anan.boardId"), radio.boardId);
    }
    request.params.insert(QStringLiteral("anan.protocol"), radio.hpsdrProtocol == 1 ? 1 : 2);
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
