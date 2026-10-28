#include "AnanBackend.h"
#include "AnanSettings.h"
#include "AnanTransmitPower.h"
#include "AnanPlannedResource.h"
#include "core/AppSettings.h"
#include "models/BandPlanManager.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcAnanP2)

namespace AetherSDR::anan {
namespace {
struct AudioCredit final {
    AudioCredit(std::shared_ptr<std::atomic<std::size_t>> value, std::size_t count)
        : ledger(std::move(value)), bytes(count) {}
    AudioCredit(const AudioCredit&) = delete;
    AudioCredit& operator=(const AudioCredit&) = delete;
    std::shared_ptr<std::atomic<std::size_t>> ledger;
    std::size_t bytes;
    ~AudioCredit() { ledger->fetch_sub(bytes, std::memory_order_relaxed); }
};
constexpr std::size_t kMaximumPendingAudioBytes = 8192;
}

QVariantMap AnanBackend::transmitPowerState()
{
    const auto defaults = defaultTransmitPower(m_pendingParams.model);
    const bool available = m_connected && m_discoveryInfoReceived && defaults && !m_radioSerial.isEmpty();
    const QString identity = QStringLiteral("%1:%2:%3:%4:%5").arg(m_radioSerial).arg(m_connectGeneration)
        .arg(static_cast<int>(m_pendingParams.model)).arg(m_discoveredBoardId).arg(m_pendingParams.protocol);
    const QVariantMap saved = available ? AnanSettings::transmitPower(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol) : QVariantMap{};
    const auto parsed = parseTransmitPower(saved);
    QVariantList bands;
    for (const char* name : kPaBands) { bands.append(QString::fromLatin1(name)); }
    return {{QStringLiteral("available"),available}, {QStringLiteral("identity"),identity},
        {QStringLiteral("bands"),bands}, {QStringLiteral("saved"),parsed.has_value()},
        {QStringLiteral("options"),parsed ? transmitPowerOptions(*parsed)
            : defaults ? transmitPowerOptions(*defaults) : QVariantMap{}},
        {QStringLiteral("reference"),defaults ? transmitPowerOptions(*defaults) : QVariantMap{}},
        {QStringLiteral("txEnabled"),transmitSessionCurrent()},
        {QStringLiteral("session"),transmitSessionState()}, {QStringLiteral("preparation"),transmitPreparationState()}};
}

QString AnanBackend::transmitSessionIdentity() const
{
    return transmitPreparationIdentity() + QLatin1Char(':') + QString::number(m_sliceFreqHz,'g',17)
        + QLatin1Char(':') + QString::number(m_receiveCalibration.frequencyFactor,'g',17)
        + QLatin1Char(':') + QString::number(m_receiveLayout.slices().size())
        + QLatin1Char(':') + AppSettings::instance().value(QStringLiteral("BandPlanName"),QStringLiteral("ARRL (US)")).toString();
}

bool AnanBackend::transmitSessionCurrent() const
{
    return m_txSessionEnabled && m_txSessionPermit && m_txSessionPermit->load()
        && m_connected && !m_receiveBuilding && !m_rateChanging && !m_receiveCalibrationRun
        && !m_droopCalibrator.isRunning()
        && primaryVisible() && m_receiveLayout.slices().size() == 1 && m_additionalRx.empty()
        && m_txSessionIdentity == transmitSessionIdentity();
}

QString AnanBackend::transmitSessionRefusal() const
{
    if (!m_connected || !m_discoveryInfoReceived || m_pendingParams.protocol != 2
        || m_pendingParams.model != AnanModel::Anan200D
        || !p2TransmitProfile(m_discoveredBoardId,m_pendingParams.model,m_pendingParams.overrideDetection)) {
        return QStringLiteral("Session TX currently requires ANAN-200D P2, native board 4 or explicit board-5 override.");
    }
    if (!primaryVisible() || m_receiveLayout.slices().size() != 1 || !m_additionalRx.empty()
        || m_receiveBuilding || m_rateChanging || m_receiveCalibrationRun || m_droopCalibrator.isRunning()) {
        return QStringLiteral("Use one primary receive slice and finish receive configuration or calibration first.");
    }
    if ((m_mode != QLatin1String("USB") && m_mode != QLatin1String("LSB")) || !m_transmitPlanReady
        || m_transmitPlanIdentity != transmitPreparationIdentity()) {
        return QStringLiteral("Prepare the current USB or LSB TX DSP first.");
    }
    const auto profile = parseTransmitPower(AnanSettings::transmitPower(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol));
    const int band = receiveAntennaBandIndex();
    if (!profile || band < 0 || band >= 11 || profile->limitsWatts[band] <= 0 || profile->limitsWatts[band] > 100) {
        return QStringLiteral("Save a PA profile with a current-band power limit above 0 and at most 100 W.");
    }
    const auto envelope = m_antennaBandPlan->amateurBandAt(m_sliceFreqHz / 1e6);
    const double low = (m_sliceFreqHz + (m_mode == QLatin1String("USB") ? 300 : -2700)) / 1e6;
    const double high = (m_sliceFreqHz + (m_mode == QLatin1String("USB") ? 2700 : -300)) / 1e6;
    if (envelope) {
        const auto regions = m_antennaBandPlan->contiguousRegionsForBand(envelope->lowMhz,envelope->highMhz);
        for (const auto& region : regions) {
            if (low >= region.lowMhz && high <= region.highMhz) { return {}; }
        }
    }
    return QStringLiteral("The fixed 300–2700 Hz TX passband must fit inside a continuous region of the selected band plan.");
}

QVariantMap AnanBackend::transmitSessionState()
{
    if (m_txSessionEnabled && !transmitSessionCurrent()) { disableTransmitSession(); }
    const QString refusal = transmitSessionRefusal();
    return {{QStringLiteral("enabled"),transmitSessionCurrent()}, {QStringLiteral("arming"),m_txSessionArming},
        {QStringLiteral("identity"),transmitSessionIdentity()},
        {QStringLiteral("available"),refusal.isEmpty() && !m_txSessionArming && !m_txSessionEnabled},
        {QStringLiteral("reason"),m_txSessionError.isEmpty() ? refusal : m_txSessionError},
        {QStringLiteral("frequencyHz"),m_sliceFreqHz}, {QStringLiteral("powerPercent"),m_txPowerPercent},
        {QStringLiteral("antenna"),m_txSessionControl ? m_txSessionControl->antenna : 1},
        {QStringLiteral("limitWatts"),m_txSessionProfile && m_txSessionBand >= 0
            ? m_txSessionProfile->limitsWatts[m_txSessionBand] : 0.0}};
}

void AnanBackend::disableTransmitSession()
{
    const bool hadSession = m_txSessionEnabled || m_txSessionArming;
    if (m_txSessionPermit) { m_txSessionPermit->store(false); }
    ++m_txSessionRevision;
    m_txSessionEnabled = false;
    m_txSessionArming = false;
    m_txSessionProfile.reset();
    m_txSessionControl.reset();
    m_transmitOperation = {};
    m_transmitDispatchOperation = {};
    if (!hadSession) { return; }
    m_transmitPlanReady = false;
    QMetaObject::invokeMethod(m_client,[client = m_client] { client->disarmTransmit(); },Qt::QueuedConnection);
    emit capabilitiesChanged();
    emitSliceState();
}

void AnanBackend::enableTransmitSession(int antenna)
{
    m_txSessionError.clear();
    const QString refusal = transmitSessionRefusal();
    if (!refusal.isEmpty() || antenna < 1 || antenna > 3 || m_txSessionEnabled || m_txSessionArming) { return; }
    m_txSessionProfile = parseTransmitPower(AnanSettings::transmitPower(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol));
    m_txSessionBand = receiveAntennaBandIndex();
    P2TxControl control;
    control.frequencyHz = m_sliceFreqHz;
    control.frequencyCorrection = m_receiveCalibration.frequencyFactor;
    control.antenna = antenna;
    control.disablePa = false;
    if (m_pendingParams.filters) { control.lowPass = m_pendingParams.filters->lowPass; }
    if (!validP2TxControl(control)) { return; }
    m_txSessionControl = control;
    m_txSessionIdentity = transmitSessionIdentity();
    m_txSessionPermit = std::make_shared<std::atomic<bool>>(true);
    const auto permit = m_txSessionPermit;
    const quint64 revision = ++m_txSessionRevision;
    m_txSessionArming = true;
    m_txPowerPercent = 0;
    QMetaObject::invokeMethod(m_client,[this,control,permit,revision] {
        const bool ok = permit->load() && m_client->armStagedTransmit(control);
        if (!permit->load() && ok) { m_client->disarmTransmit(); }
        QMetaObject::invokeMethod(this,[this,revision,ok] {
            if (revision != m_txSessionRevision) { return; }
            if (!ok || m_txSessionIdentity != transmitSessionIdentity() || !m_connected) {
                disableTransmitSession();
                m_txSessionError = QStringLiteral("TX session could not be enabled for the current radio state.");
                qCWarning(lcAnanP2) << m_txSessionError;
                return;
            }
            TransmitDelta zero;
            zero.rfPower = 0;
            emit transmitChanged(zero);
            m_txSessionArming = false;
            m_txSessionEnabled = true;
            emit capabilitiesChanged();
            emitSliceState();
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}

ReceiveDispatch AnanBackend::requestTransmitControl(const TransmitControlRequest& request,
                                                    TunePowerContext context)
{
    Q_UNUSED(context);
    if (!request.valid() || request.field != TransmitControlRequest::Field::RfPower
        || !transmitSessionCurrent()) {
        return ReceiveDispatch::Unsupported;
    }
    setTxPower(std::get<int>(request.value));
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setTxPower(int percent)
{
    m_txPowerPercent = std::clamp(percent,0,100);
    TransmitDelta delta;
    delta.rfPower = m_txPowerPercent;
    emit transmitChanged(delta);
    if (!transmitSessionCurrent() || !m_txSessionProfile
        || !m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
    QMetaObject::invokeMethod(m_client,[client = m_client,profile = *m_txSessionProfile,
        band = m_txSessionBand,percent = m_txPowerPercent,operation = m_transmitDispatchOperation] {
        client->setTransmitPower(profile,band,percent,operation);
    },Qt::QueuedConnection);
}

ReceiveDispatch AnanBackend::requestTxSlice(int id, bool enabled, const RadioControlCompletion& completion,
                                           const RadioControlFence& current)
{
    if ((current && !current()) || id != 0 || !transmitSessionCurrent()) {
        if (completion) { completion({RadioControlReply::Kind::Refused,-1,QStringLiteral("Only the armed primary slice can transmit")}); }
        return ReceiveDispatch::Unsupported;
    }
    if (!enabled) { disableTransmitSession(); }
    else { emitSliceState(); }
    if (completion) { completion({RadioControlReply::Kind::Accepted,0,{}}); }
    return ReceiveDispatch::Dispatched;
}

QString AnanBackend::transmitPreparationIdentity() const
{
    return QStringLiteral("%1:%2:%3:%4:%5:%6").arg(m_radioSerial).arg(m_connectGeneration)
        .arg(static_cast<int>(m_pendingParams.model)).arg(m_discoveredBoardId)
        .arg(m_pendingParams.protocol).arg(m_mode);
}

void AnanBackend::cancelTransmitPreparation()
{
    disableTransmitSession();
    if (m_transmitPlanCancelled) { m_transmitPlanCancelled->store(true); }
    ++m_transmitPlanTicket;
    m_transmitPlanBuilding = false;
    m_transmitPlanReady = false;
    m_transmitPlanIdentity.clear();
    m_transmitPlanError.clear();
    if (m_client && m_ioThread && m_ioThread->isRunning()) {
        QMetaObject::invokeMethod(m_client,[client = m_client] { client->discardStagedTransmit(); },Qt::QueuedConnection);
    }
}

QVariantMap AnanBackend::transmitPreparationState()
{
    const bool supported = m_connected && m_discoveryInfoReceived && m_pendingParams.protocol == 2
        && p2TransmitProfile(m_discoveredBoardId,m_pendingParams.model,m_pendingParams.overrideDetection)
        && (m_mode == QLatin1String("USB") || m_mode == QLatin1String("LSB"));
    if (!m_transmitPlanIdentity.isEmpty()
        && (!supported || m_transmitPlanIdentity != transmitPreparationIdentity())) { cancelTransmitPreparation(); }
    return {{QStringLiteral("available"),supported && !m_rateChanging && !m_receiveBuilding},
        {QStringLiteral("building"),m_transmitPlanBuilding}, {QStringLiteral("ready"),m_transmitPlanReady},
        {QStringLiteral("error"),m_transmitPlanError}, {QStringLiteral("mode"),m_mode}};
}

void AnanBackend::prepareTransmitDsp()
{
    cancelTransmitPreparation();
    m_transmitPlanIdentity = transmitPreparationIdentity();
    m_transmitPlanBuilding = true;
    m_transmitPlanCancelled = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = m_transmitPlanCancelled;
    const quint64 ticket = m_transmitPlanTicket;
    const auto mode = modeFromString(m_mode);
    QMetaObject::invokeMethod(m_dspBuildContext,[this,cancelled,ticket,mode] {
        if (cancelled->load()) { return; }
        auto channel = std::make_unique<AnanTxDsp>();
        std::string error;
        const bool ok = channel->configure(mode,&error);
        if (cancelled->load()) { return; }
        auto prepared = retainPlannedResource(std::move(channel),m_dspBuildContext);
        QMetaObject::invokeMethod(m_client,[this,cancelled,ticket,ok,prepared = std::move(prepared),
                                          error = QString::fromStdString(error)]() mutable {
            if (cancelled->load()) { return; }
            const bool installed = ok && m_client->stageTransmitDsp(std::move(prepared));
            QMetaObject::invokeMethod(this,[this,ticket,installed,error] {
                if (ticket != m_transmitPlanTicket) { return; }
                if (!m_connected || m_transmitPlanIdentity != transmitPreparationIdentity()) {
                    cancelTransmitPreparation(); return;
                }
                m_transmitPlanBuilding = false;
                m_transmitPlanReady = installed;
                m_transmitPlanError = installed ? QString() : error.isEmpty()
                    ? QStringLiteral("TX audio path refused the prepared DSP") : error;
            },Qt::QueuedConnection);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
void AnanBackend::setKeying(bool key, const TxCoordinator::Operation& operation,
    const TxCoordinator::Completion& completion)
{
    if (!TxCoordinator::Command{operation, key}.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_client || !m_ioThread || !m_ioThread->isRunning()) {
        completion.finish();
        return;
    }
    if (!key) {
        if (!operation.sameSession(m_transmitOperation)) { completion.finish(); return; }
        const auto cleanup = m_transmitDispatchOperation;
        m_transmitOperation = {};
        m_transmitDispatchOperation = {};
        QMetaObject::invokeMethod(m_client,[client = m_client,cleanup,completion] {
            client->setTransmitKeying(false,cleanup);
            completion.finish();
        },Qt::QueuedConnection);
        return;
    }
    if (!transmitSessionCurrent() || !m_txSessionProfile || !m_txSessionControl) {
        completion.finish();
        return;
    }
    P2TxControl control = *m_txSessionControl;
    const auto power = transmitPowerForPercent(*m_txSessionProfile,m_txSessionBand,m_txPowerPercent);
    if (!power) { completion.finish(); disableTransmitSession(); return; }
    control.drive = power->drive;
    const bool continuing = m_transmitOperation.sameOperation(operation)
        && m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs());
    const auto permit = m_txSessionPermit;
    if (!continuing) {
        m_transmitDispatchOperation = operation.heldKeying().withKeyingPermit([permit] { return permit->load(); });
    }
    m_transmitOperation = operation;
    const auto dispatch = m_transmitDispatchOperation;
    const quint64 revision = m_txSessionRevision;
    QMetaObject::invokeMethod(m_client,[this,operation,dispatch,completion,control,permit,revision,continuing,
                                      mode = modeFromString(m_mode)] {
        if (!permit->load() || !operation.permitsDispatch(TxCoordinator::monotonicMs())) { completion.finish(); return; }
        const bool ok = m_client->transmitMode() == mode
            && (continuing || m_client->armStagedTransmit(control))
            && m_client->setTransmitKeying(true,dispatch);
        if (!ok) {
            QMetaObject::invokeMethod(this,[this,revision,operation] {
                if (revision == m_txSessionRevision && operation.sameOperation(m_transmitOperation)
                    && operation.permitsDispatch(TxCoordinator::monotonicMs())) {
                    disableTransmitSession();
                    m_txSessionError = QStringLiteral("MOX/PTT request was refused by the TX audio or transport path.");
                    qCWarning(lcAnanP2) << m_txSessionError;
                }
            },Qt::QueuedConnection);
        }
        completion.finish();
    }, Qt::QueuedConnection);
}

void AnanBackend::submitTxAudio(const QByteArray& pcm, int sampleRateHz, TxAudioSource source,
    const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    const auto operation = m_transmitDispatchOperation;
    if (!m_connected || m_pendingParams.protocol != 2 || !m_client || !m_ioThread
        || !m_ioThread->isRunning() || !operation.permitsDispatch(now)
        || !context.permitsDispatch(now)) { return; }
    const std::size_t bytes = static_cast<std::size_t>(pcm.size());
    if (sampleRateHz != AnanTxDsp::kInputRate || bytes == 0 || bytes % 4 != 0
        || bytes > kMaximumPendingAudioBytes) {
        setKeying(false, operation);
        return;
    }
    const auto ledger = m_transmitQueuedBytes;
    const std::size_t previous = ledger->fetch_add(bytes, std::memory_order_relaxed);
    if (previous > kMaximumPendingAudioBytes - bytes) {
        ledger->fetch_sub(bytes, std::memory_order_relaxed);
        setKeying(false, operation);
        return;
    }
    // The credit survives queueing and is returned even when Qt discards an
    // undelivered functor during teardown. Old sessions keep their own ledger.
    const auto credit = std::make_shared<AudioCredit>(ledger, bytes);
    QMetaObject::invokeMethod(m_client, [client = m_client, pcm, sampleRateHz, source,
                                       operation, context, credit] {
        client->submitTransmitAudio(pcm, sampleRateHz, source, operation, context);
    }, Qt::QueuedConnection);
}
} // namespace AetherSDR::anan
