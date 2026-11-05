#include "AnanBackend.h"
#include "core/RadioSettingsScope.h"

#include <QJsonObject>
#include <QMetaObject>
#include <optional>

namespace AetherSDR::anan {
namespace {
QString settingsKey(AnanModel model, int board, int protocol)
{
    return QStringLiteral("%1:%2:%3").arg(static_cast<int>(model)).arg(board).arg(protocol);
}

bool number(const QVariant& value)
{
    const int type = value.metaType().id();
    return (type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
        || type == QMetaType::LongLong || type == QMetaType::UInt)
        && std::isfinite(value.toDouble());
}

std::optional<DiversitySettings> parseSettings(const QVariantMap& fields)
{
    for (const QString& name : {QStringLiteral("gain"), QStringLiteral("phaseDegrees"),
                               QStringLiteral("reference"), QStringLiteral("output")}) {
        if (!number(fields.value(name))) { return std::nullopt; }
    }
    const double reference = fields.value(QStringLiteral("reference")).toDouble();
    const double output = fields.value(QStringLiteral("output")).toDouble();
    if (reference < 0 || reference > 1 || std::floor(reference) != reference
        || output < 0 || output > 2 || std::floor(output) != output) { return std::nullopt; }
    DiversitySettings settings;
    settings.gain = fields.value(QStringLiteral("gain")).toDouble();
    settings.phaseDegrees = fields.value(QStringLiteral("phaseDegrees")).toDouble();
    settings.reference = static_cast<int>(reference);
    settings.output = static_cast<int>(output);
    return settings.valid() ? std::optional<DiversitySettings>(settings) : std::nullopt;
}

QVariantMap values(const DiversitySettings& settings)
{
    return {{QStringLiteral("gain"), settings.gain}, {QStringLiteral("phaseDegrees"), settings.phaseDegrees},
            {QStringLiteral("reference"), settings.reference}, {QStringLiteral("output"), settings.output}};
}
} // namespace

void AnanBackend::loadDiversitySettings()
{
    if (m_radioSerial.isEmpty()) { return; }
    int version = 0;
    const QJsonObject document = RadioSettingsScope(QStringLiteral("anan"), m_radioSerial)
        .featureExact(QStringLiteral("Diversity"), &version);
    if (version > 1) { return; }
    const std::optional<DiversitySettings> saved = parseSettings(document.value(
        settingsKey(m_pendingParams.model, m_discoveredBoardId, m_pendingParams.protocol)).toObject().toVariantMap());
    if (saved) {
        const bool enabled = m_diversity.enabled;
        m_diversity = *saved;
        m_diversity.enabled = enabled;
    }
}

QVariantMap AnanBackend::diversityState() const
{
    const std::optional<ReceiveProfile> physical = receiveProfile(m_discoveredBoardId);
    const std::optional<ReceiveProfile> profile = receiveProfile(m_discoveredBoardId,
        m_pendingParams.model, m_pendingParams.overrideDetection);
    // P1 Red Pitaya reports Hermes but has the validated two-ADC wire layout.
    const std::optional<P1Client::Profile> p1Profile = m_pendingParams.protocol == 1
        ? P1Client::profile(m_discoveredBoardId, m_pendingParams.redPitayaP1
            || m_pendingParams.model == AnanModel::RedPitaya) : std::nullopt;
    const bool twoPhysicalAdcs = m_pendingParams.protocol == 1
        ? p1Profile && p1Profile->numAdcs == 2 : physical && physical->numAdcs == 2;
    QString reason;
    if (!m_connected || !m_discoveryInfoReceived) {
        reason = QStringLiteral("Connect a validated Extended ANAN receiver first.");
    } else if (!twoPhysicalAdcs || !profile || profile->numAdcs != 2) {
        reason = QStringLiteral("Coherent diversity requires two physical ADC inputs.");
    } else if (!primaryVisible() || primaryPanIndex() != 0 || m_receiveAdcs[0] != 0) {
        reason = QStringLiteral("Diversity requires RX1 on its primary DDC with ADC0 selected.");
    } else if (m_pendingParams.protocol == 1) {
        const std::optional<P1Client::Profile> wire = P1Client::profile(effectiveBoardId(), m_pendingParams.redPitayaP1);
        if (!wire || wire->numAdcs != 2 || wire->frameSlotCount < 3) {
            reason = QStringLiteral("This P1 session cannot provide the synchronized ADC pair.");
        }
    } else if (m_discoveredReceiverCount > 0 && m_discoveredReceiverCount < 2) {
        reason = QStringLiteral("This P2 firmware does not report two DDCs for the synchronized pair.");
    }
    const bool busy = m_pureSignalEnabled || m_pureSignalPending || m_diversityPending || m_receiveBuilding || m_rateChanging || m_receiveCalibrationRun
        || m_droopCalibrator.isRunning() || m_transmitOperation.permitsCleanup() || m_txSessionArming;
    if (reason.isEmpty() && busy) {
        reason = QStringLiteral("Finish TX, calibration or receive configuration before editing diversity.");
    }
    QVariantMap state = values(m_diversity);
    state.insert(QStringLiteral("enabled"), m_diversity.enabled);
    state.insert(QStringLiteral("available"), reason.isEmpty());
    state.insert(QStringLiteral("reason"), reason);
    state.insert(QStringLiteral("identity"), transmitPreparationIdentity());
    return state;
}

void AnanBackend::requestDiversity(quint64 requestId, const QVariantMap& fields)
{
    const QVariantMap state = diversityState();
    std::optional<DiversitySettings> settings = parseSettings(fields);
    if (!state.value(QStringLiteral("available")).toBool() || !settings || fields.size() != 6
        || fields.value(QStringLiteral("enabled")).metaType().id() != QMetaType::Bool
        || fields.value(QStringLiteral("identity")) != state.value(QStringLiteral("identity"))) {
        emit extensionError(requestId, state.value(QStringLiteral("reason")).toString().isEmpty()
            ? QStringLiteral("Invalid diversity settings or retired connection.")
            : state.value(QStringLiteral("reason")).toString());
        return;
    }
    const RadioSettingsScope scope(QStringLiteral("anan"), m_radioSerial);
    int version = 0;
    AppSettings::FeatureReadStatus status;
    scope.featureExact(QStringLiteral("Diversity"), &version, &status);
    if (!scope.hasRadioIdentity() || version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable) {
        emit extensionError(requestId, QStringLiteral("The diversity settings document cannot be updated."));
        return;
    }
    settings->enabled = fields.value(QStringLiteral("enabled")).toBool();
    const quint64 epoch = m_receiveEpoch;
    m_diversityPending = true;
    QMetaObject::invokeMethod(m_client, [this, settings = *settings, epoch, requestId, scope] {
        if (m_receiveIoEpoch->load() != epoch) { return; }
        const bool accepted = m_client->setDiversity(settings);
        QMetaObject::invokeMethod(this, [this, settings, epoch, requestId, scope, accepted] {
            if (m_receiveEpoch != epoch || !m_connected) { return; }
            m_diversityPending = false;
            if (!accepted) {
                emit extensionError(requestId, QStringLiteral("The session refused the diversity configuration; RX settings were not saved."));
                return;
            }
            m_diversity = settings;
            int version = 0;
            AppSettings::FeatureReadStatus status;
            QJsonObject document = scope.featureExact(QStringLiteral("Diversity"), &version, &status);
            document.insert(settingsKey(m_pendingParams.model, m_discoveredBoardId, m_pendingParams.protocol),
                            QJsonObject::fromVariantMap(values(settings)));
            const bool saved = version <= 1 && status != AppSettings::FeatureReadStatus::Corrupt
                && status != AppSettings::FeatureReadStatus::Unavailable
                && scope.setFeature(QStringLiteral("Diversity"), 1, document);
            updateReceiveStreams();
            emit capabilitiesChanged();
            if (saved) { emit extensionResult(requestId, diversityState()); }
            else { emit extensionError(requestId, QStringLiteral("Diversity applied, but its adjustment values could not be saved.")); }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
} // namespace AetherSDR::anan
