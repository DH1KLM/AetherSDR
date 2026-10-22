#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan/P2Protocol.h"
#include "core/RadioDiscovery.h"
#include <QCoreApplication>
#include "core/backends/anan/AnanFilterOptions.h"

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <iterator>

namespace AetherSDR::anan {
namespace {

// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"ddc0RateKsps":int, "dither":bool,
//         "random":bool, "ddc0AdcIndex":int, "bypassAdc0Filters":bool,
//         "bypassAdc1Filters":bool, "speakerAudioEnabled":bool}
const QString kRootKey = QStringLiteral("Anan");

constexpr const char* kFieldDdc0RateKsps      = "ddc0RateKsps";
constexpr const char* kFieldDither            = "dither";
constexpr const char* kFieldRandom            = "random";
constexpr const char* kFieldDdc0AdcIndex      = "ddc0AdcIndex";
constexpr const char* kFieldRxAntenna          = "rxAntenna";
constexpr const char* kFieldBypassAdc0Filters = "bypassAdc0Filters";
constexpr const char* kFieldSpeakerAudioEnabled = "speakerAudioEnabled";
constexpr const char* kFieldBypassAdc1Filters = "bypassAdc1Filters";

}  // namespace

QJsonObject AnanSettings::readObj()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AnanSettings::writeObj(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    // COMMIT -- setValue() only marks the row dirty; save() runs the
    // transaction and there is no autosave timer. See IcomSettings::
    // writeObj(), the sibling this is modelled on, for the same note.
    s.save();
}

//DH1KLM: A model-specific child prevents an override from reusing another filter bank's limits.
QVariantMap AnanSettings::hardwareFilters(const QString& serial, int model, int board)
{
    if (serial.trimmed().isEmpty()) { return {}; }
    int version = 0;
    AppSettings::FeatureReadStatus status;
    const auto doc = RadioSettingsScope(QStringLiteral("anan"), serial)
        .featureExact(QStringLiteral("HardwareFilters"), &version, &status);
    if (version > 6 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) { return {}; }
    return doc.value(QStringLiteral("%1:%2").arg(model).arg(board)).toObject().toVariantMap();
}

bool AnanSettings::setHardwareFilters(const QString& serial, int model, int board,
                                     const QVariantMap& options)
{
    const auto config = parseFilterOptions(options);
    if (serial.trimmed().isEmpty() || !config
        || !ananModelInfo(static_cast<AnanModel>(model)) || !receiveProfile(board)) { return false; }
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    auto doc = scope.featureExact(QStringLiteral("HardwareFilters"), &version, &status);
    if (version > 6 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc.insert(QStringLiteral("%1:%2").arg(model).arg(board), QJsonObject::fromVariantMap(filterOptions(*config)));
    return scope.setFeature(QStringLiteral("HardwareFilters"), 6, doc);
}

int AnanSettings::ddc0RateKsps()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0RateKsps)).toInt(48);
    return v > 0 ? v : 48;
}

void AnanSettings::setDdc0RateKsps(int ksps)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0RateKsps)] = ksps;
    writeObj(obj);
}

bool AnanSettings::ditherEnabled()
{
    return readObj().value(QLatin1String(kFieldDither)).toBool(true);
}

void AnanSettings::setDitherEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDither)] = on;
    writeObj(obj);
}

bool AnanSettings::randomEnabled()
{
    return readObj().value(QLatin1String(kFieldRandom)).toBool(true);
}

void AnanSettings::setRandomEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldRandom)] = on;
    writeObj(obj);
}

int AnanSettings::ddc0AdcIndex()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0AdcIndex)).toInt(0);
    return v == 1 ? 1 : 0;   // anything but a deliberate 1 reads as ADC0
}

void AnanSettings::setDdc0AdcIndex(int index)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0AdcIndex)] = (index == 1 ? 1 : 0);
    writeObj(obj);
}

int AnanSettings::rxAntenna()
{
    const int v = readObj().value(QLatin1String(kFieldRxAntenna)).toInt(1);
    return (v >= 1 && v <= 3) ? v : 1;
}

int AnanSettings::radioRxAntenna(const QString& serial, int model, int board)
{
    if (serial.trimmed().isEmpty()) { return rxAntenna(); }
    int version = 0;
    AppSettings::FeatureReadStatus status;
    const auto doc = RadioSettingsScope(QStringLiteral("anan"), serial)
        .featureExact(QStringLiteral("ReceiveAntenna"), &version, &status);
    if (status != AppSettings::FeatureReadStatus::Present || version != 1) { return rxAntenna(); }
    const auto value = doc.value(QStringLiteral("%1:%2").arg(model).arg(board));
    const int antenna = value.toInt(-1);
    return value.isDouble() && value.toDouble() == antenna && antenna >= 1 && antenna <= 3
        ? antenna : rxAntenna();
}

bool AnanSettings::setRadioRxAntenna(const QString& serial, int model, int board, int antenna)
{
    if (serial.trimmed().isEmpty() || antenna < 1 || antenna > 3
        || !ananModelInfo(static_cast<AnanModel>(model)) || !receiveProfile(board)) { return false; }
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    auto doc = scope.featureExact(QStringLiteral("ReceiveAntenna"), &version, &status);
    if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc.insert(QStringLiteral("%1:%2").arg(model).arg(board), antenna);
    return scope.setFeature(QStringLiteral("ReceiveAntenna"), 1, doc);
}

void AnanSettings::setRxAntenna(int antenna)
{
    if (antenna < 1 || antenna > 3)
        return;
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldRxAntenna)] = antenna;
    writeObj(obj);
}

bool AnanSettings::bypassAdc0Filters()
{
    //DH1KLM: Saturn BPF1 is selected from the tuned frequency unless the operator opts into bypass.
    return readObj().value(QLatin1String(kFieldBypassAdc0Filters)).toBool(false);
}

void AnanSettings::setBypassAdc0Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc0Filters)] = on;
    writeObj(obj);
}

bool AnanSettings::speakerAudioEnabled()
{
    // false, not true: an absent key must mean "off" here. Every other option in
    // this file defaults true because absence means "the shipped hardware
    // default"; this one's absence means the operator has never asked for an
    // outbound stream, and starting one anyway is the wrong way to be wrong.
    return readObj().value(QLatin1String(kFieldSpeakerAudioEnabled)).toBool(false);
}

void AnanSettings::setSpeakerAudioEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldSpeakerAudioEnabled)] = on;
    writeObj(obj);
}

bool AnanSettings::bypassAdc1Filters()
{
    return readObj().value(QLatin1String(kFieldBypassAdc1Filters)).toBool(true);
}

void AnanSettings::setBypassAdc1Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc1Filters)] = on;
    writeObj(obj);
}

QList<int> AnanSettings::validDdc0RatesKsps()
{
    QList<int> rates;
    rates.reserve(static_cast<int>(std::size(kDdc0RatesKsps)));
    for (const int ksps : kDdc0RatesKsps)
        rates.append(ksps);
    return rates;
}

void AnanSettings::reset()
{
    writeObj({});
}

bool AnanSettings::redPitayaP1ForRadio(const QString& serial)
{
    if (serial.trimmed().isEmpty())
        return false;
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    const auto doc = scope.featureExact(QStringLiteral("HardwareProfile"), &version);
    return version <= 1 && doc.value(QStringLiteral("redPitayaP1")).toBool(false);
}

AnanSettings::RadioProfile AnanSettings::radioProfile(const QString& serial)
{
    if (serial.trimmed().isEmpty()) { return {}; }
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    const auto doc = scope.featureExact(QStringLiteral("HardwareProfile"), &version, &status);
    if (status == AppSettings::FeatureReadStatus::Missing) { return {}; }
    if (status != AppSettings::FeatureReadStatus::Present || version < 1 || version > 3) {
        return {AnanModel::Automatic, 0, false};
    }
    //DH1KLM: Preserve the hardware-tested version-1 Red Pitaya P1 preference.
    if (version == 1) {
        return doc.value(QStringLiteral("redPitayaP1")).toBool(false)
            ? RadioProfile{AnanModel::RedPitaya, 1, true} : RadioProfile{};
    }
    const auto model = static_cast<AnanModel>(doc.value(QStringLiteral("model")).toInt(-100));
    const int protocol = doc.value(QStringLiteral("protocol")).toInt(-1);
    const auto overrideValue = doc.value(QStringLiteral("overrideDetection"));
    const bool overrideDetection = version >= 3 && overrideValue.toBool(false);
    return {model, protocol,
            ananModelInfo(model) && protocol >= 0 && protocol <= 2
                && (version < 3 || overrideValue.isBool())
                && (!overrideDetection || (model != AnanModel::Automatic
                    && model != AnanModel::Hpsdr && model != AnanModel::HermesLite2)),
            overrideDetection};
}

bool AnanSettings::setRadioProfile(const QString& serial, AnanModel model, int protocol,
                                  bool overrideDetection)
{
    if (serial.trimmed().isEmpty() || !ananModelInfo(model) || protocol < 0 || protocol > 2
        || (overrideDetection && (model == AnanModel::Automatic
            || model == AnanModel::Hpsdr || model == AnanModel::HermesLite2))) {
        return false;
    }
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    auto doc = scope.featureExact(QStringLiteral("HardwareProfile"), &version, &status);
    if (version > 3 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc.insert(QStringLiteral("model"), static_cast<int>(model));
    doc.insert(QStringLiteral("protocol"), protocol);
    doc.insert(QStringLiteral("overrideDetection"), overrideDetection);
    return scope.setFeature(QStringLiteral("HardwareProfile"), 3, doc);
}

//DH1KLM: Discovery reports a board family, never proof of a specific SKU.
// Keep that identity intact and expose the effective mapping separately.
void AnanSettings::applyRadioProfileDisplay(RadioInfo& radio)
{
    if (radio.family.compare(QLatin1String("anan"), Qt::CaseInsensitive) != 0)
        return;
    if (radio.boardId >= 0 && radio.boardId <= 255) {
        DiscoveryReply physical;
        physical.boardId = static_cast<std::uint8_t>(radio.boardId);
        radio.model = QString::fromLatin1(physical.boardDisplayName());
        radio.name = radio.model;
    }
    const auto saved = radioProfile(radio.serial);
    const auto* descriptor = ananModelInfo(saved.model);
    //DH1KLM: The main operating UI uses the selected model only for explicit
    // override. Discovery and the connection-details snapshot keep model intact.
    radio.effectiveModel = saved.valid && saved.overrideDetection && descriptor
        && receiveProfile(radio.boardId, saved.model, true)
        ? QString::fromLatin1(descriptor->name.data(), descriptor->name.size()) : QString();
    const QString profile = !saved.valid || !descriptor
        ? QCoreApplication::translate("AnanSettings", "Invalid saved profile")
        : QString::fromLatin1(descriptor->name.data(), descriptor->name.size());
    radio.connectionProfile = QCoreApplication::translate("AnanSettings", "Profile: %1 • Override %2")
        .arg(profile, saved.valid && saved.overrideDetection ? QStringLiteral("ON") : QStringLiteral("OFF"));
    if (radio.hpsdrProtocol == 1 || radio.hpsdrProtocol == 2)
        radio.connectionProfile += QStringLiteral(" • P%1").arg(radio.hpsdrProtocol);
}

bool AnanSettings::hasRadioProfileMapping(const RadioInfo& radio)
{
    if (radio.family.compare(QLatin1String("anan"), Qt::CaseInsensitive) != 0
        || radio.serial.trimmed().isEmpty())
        return false;
    int version = 0;
    AppSettings::FeatureReadStatus status;
    const auto doc = RadioSettingsScope(QStringLiteral("anan"), radio.serial)
        .featureExact(QStringLiteral("HardwareProfile"), &version, &status);
    if (status != AppSettings::FeatureReadStatus::Present || version < 1 || version > 3)
        return false;
    return version == 1 ? doc.value(QStringLiteral("redPitayaP1")).toBool(false)
        : doc.value(QStringLiteral("model")).toInt(-1) != -1
            || doc.value(QStringLiteral("overrideDetection")).toBool(false);
}

bool AnanSettings::forgetRadioProfileMapping(const RadioInfo& radio)
{
    if (radio.family.compare(QLatin1String("anan"), Qt::CaseInsensitive) != 0
        || radio.serial.trimmed().isEmpty())
        return false;
    const RadioSettingsScope scope(QStringLiteral("anan"), radio.serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    auto doc = scope.featureExact(QStringLiteral("HardwareProfile"), &version, &status);
    if (status == AppSettings::FeatureReadStatus::Missing)
        return true;
    if (status != AppSettings::FeatureReadStatus::Present || version < 1 || version > 3)
        return false;
    //DH1KLM: Forget only the logical model/override mapping. Keep protocol,
    // sibling fields and every other feature (nickname, DSP, calibration, etc.).
    if (version == 1)
        doc.insert(QStringLiteral("protocol"), doc.value(QStringLiteral("redPitayaP1")).toBool(false) ? 1 : 0);
    doc.remove(QStringLiteral("redPitayaP1"));
    doc.insert(QStringLiteral("model"), static_cast<int>(AnanModel::Automatic));
    doc.insert(QStringLiteral("overrideDetection"), false);
    return scope.setFeature(QStringLiteral("HardwareProfile"), 3, doc);
}

bool AnanSettings::setRedPitayaP1ForRadio(const QString& serial, bool enabled)
{
    if (serial.trimmed().isEmpty())
        return false;
    const RadioSettingsScope scope(QStringLiteral("anan"), serial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    auto doc = scope.featureExact(QStringLiteral("HardwareProfile"), &version, &status);
    if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable)
        return false;
    doc.insert(QStringLiteral("redPitayaP1"), enabled);
    return scope.setFeature(QStringLiteral("HardwareProfile"), 1, doc);
}

}  // namespace AetherSDR::anan
