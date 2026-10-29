#include "AnanBackend.h"
#include "AnanSettings.h"
#include "AnanTransmitPower.h"
#include "AnanTxMode.h"
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

Q_LOGGING_CATEGORY(lcAnanTx, "aether.anan.tx", QtWarningMsg)

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
constexpr std::size_t kMaximumPendingAudioBytes = 4 * AnanTxDsp::kMaximumQueuedInputSamples;
}

bool AnanBackend::rememberTransmitSession(int antenna)
{
    const bool saved = AnanSettings::setTransmitSessionAntenna(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol,antenna);
    if (!saved) { qCWarning(lcAnanTx) << "ANAN TX readiness preference could not be saved"; }
    return saved;
}

void AnanBackend::muteReceiveForTransmit(bool muted)
{
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp,"setAudioMuted",Qt::QueuedConnection,Q_ARG(bool,muted));
    }
}

void AnanBackend::publishLocalTransmitState(bool keyed)
{
    if (m_localTransmitPublished == keyed) { return; }
    m_localTransmitPublished = keyed;
    TransmitDelta delta;
    delta.mox = keyed;
    emit transmitChanged(delta);
}

void AnanBackend::refreshIdleTransmitSession()
{
    if (!m_txSessionEnabled) { return; }
    if (m_transmitOperation.permitsCleanup() || !m_txSessionControl || !m_txSessionProfile
        || receiveAntennaBandIndex() != m_txSessionBand || !transmitSessionRefusal().isEmpty()) {
        disableTransmitSession();
        return;
    }
    P2TxControl control = *m_txSessionControl;
    control.frequencyHz = m_sliceFreqHz;
    if (!validP2TxControl(control)) { disableTransmitSession(); return; }
    m_txSessionControl = control;
    m_txSessionIdentity = transmitSessionIdentity();
}

void AnanBackend::restoreTransmitSession()
{
    if (!m_connected || !primaryVisible() || m_receiveLayout.slices().size() != 1 || !m_additionalRx.empty()
        || m_txSessionEnabled || m_txSessionArming || m_transmitPlanBuilding
        || !m_txSessionError.isEmpty() || !m_transmitPlanError.isEmpty()
        || m_receiveCalibrationRun || m_droopCalibrator.isRunning()) { return; }
    const auto profile = parseTransmitPower(AnanSettings::transmitPower(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol));
    const int band = receiveAntennaBandIndex();
    if (!profile || band < 0 || band >= 11 || profile->limitsWatts[band] <= 0) { return; }
    const int savedAntenna = AnanSettings::transmitSessionAntenna(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol);
    const int antenna = savedAntenna > 0 ? savedAntenna : 1;
    if (!transmitPreparationState().value(QStringLiteral("available")).toBool()) { return; }
    if (m_transmitPlanReady && m_transmitPlanIdentity == transmitPreparationIdentity()) {
        enableTransmitSession(antenna); return;
    }
    prepareTransmitDsp();
    m_restoreTxAntenna = antenna;
}

QVariantMap AnanBackend::transmitPowerState()
{
    const auto defaults = defaultTransmitPower(m_pendingParams.model,m_discoveredBoardId);
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
    if (!m_connected || !m_discoveryInfoReceived || (m_pendingParams.protocol != 1 && m_pendingParams.protocol != 2)
        || !p2TransmitProfile(m_discoveredBoardId,m_pendingParams.model,m_pendingParams.overrideDetection)) {
        return QStringLiteral("TX requires a validated OpenHPSDR model and supported protocol.");
    }
    if (!primaryVisible() || m_receiveLayout.slices().size() != 1 || !m_additionalRx.empty()
        || m_receiveBuilding || m_rateChanging || m_receiveCalibrationRun || m_droopCalibrator.isRunning()) {
        return QStringLiteral("Use one primary receive slice and finish receive configuration or calibration first.");
    }
    const auto recipe = transmitMode(modeFromString(m_mode));
    if (!recipe) { return QStringLiteral("The selected mode is receive-only."); }
    const auto profile = parseTransmitPower(AnanSettings::transmitPower(m_radioSerial,
        static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol));
    const int band = receiveAntennaBandIndex();
    if (!profile || band < 0 || band >= 11 || profile->limitsWatts[band] <= 0) {
        return QStringLiteral("Save a PA profile with a positive current-band operator power limit.");
    }
    if (!m_transmitPlanReady || m_transmitPlanIdentity != transmitPreparationIdentity()) {
        return m_transmitPlanError.isEmpty() ? QStringLiteral("TX signal processing is being prepared automatically.")
            : QStringLiteral("TX signal processing failed: %1").arg(m_transmitPlanError);
    }
    const auto envelope = m_antennaBandPlan->amateurBandAt(m_sliceFreqHz / 1e6);
    const double low = (m_sliceFreqHz + std::min(-250.0,recipe->occupiedLowHz)) / 1e6;
    const double high = (m_sliceFreqHz + std::max(250.0,recipe->occupiedHighHz)) / 1e6;
    if (envelope) {
        const auto regions = m_antennaBandPlan->contiguousRegionsForBand(envelope->lowMhz,envelope->highMhz);
        for (const auto& region : regions) {
            if (low >= region.lowMhz && high <= region.highMhz) { return {}; }
        }
    }
    return QStringLiteral("The mode's occupied TX passband and tune carrier must fit inside the selected band plan.");
}

QVariantMap AnanBackend::transmitSessionState()
{
    if (m_txSessionEnabled && !transmitSessionCurrent()) { disableTransmitSession(); }
    const QString refusal = transmitSessionRefusal();
    return {{QStringLiteral("enabled"),transmitSessionCurrent()}, {QStringLiteral("arming"),m_txSessionArming},
        {QStringLiteral("identity"),transmitSessionIdentity()},
        {QStringLiteral("keyed"),m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())},
        {QStringLiteral("available"),refusal.isEmpty() && !m_txSessionArming && !m_txSessionEnabled},
        {QStringLiteral("reason"),m_txSessionError.isEmpty() ? refusal : m_txSessionError},
        {QStringLiteral("frequencyHz"),m_sliceFreqHz}, {QStringLiteral("powerPercent"),m_txPowerPercent},
        {QStringLiteral("antenna"),m_txSessionControl ? m_txSessionControl->antenna
            : std::max(1,AnanSettings::transmitSessionAntenna(m_radioSerial,
                static_cast<int>(m_pendingParams.model),m_discoveredBoardId,m_pendingParams.protocol))},
        {QStringLiteral("limitWatts"),m_txSessionProfile && m_txSessionBand >= 0
            ? m_txSessionProfile->limitsWatts[m_txSessionBand] : 0.0}};
}

void AnanBackend::disableTransmitSession()
{
    m_tuneActive = false; m_cwAutoKeyed = false;
    if (m_cwHangTimer) { m_cwHangTimer->stop(); }
    const bool hadSession = m_txSessionEnabled || m_txSessionArming;
    if (m_txSessionPermit) { m_txSessionPermit->store(false); }
    ++m_txSessionRevision;
    m_txSessionEnabled = false;
    m_txSessionArming = false;
    m_txSessionProfile.reset();
    m_txSessionControl.reset();
    m_transmitOperation = {};
    m_transmitDispatchOperation = {};
    m_cwReleaseCompletion.finish(); m_cwReleaseCompletion = {};
    if (!hadSession) { return; }
    QMetaObject::invokeMethod(m_client,[client = m_client] { client->disarmTransmit(false); },Qt::QueuedConnection);
    muteReceiveForTransmit(false);
    publishLocalTransmitState(false);
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
    QMetaObject::invokeMethod(m_client,[this,control,permit,revision] {
        const bool ok = permit->load() && m_client->armStagedTransmit(control);
        if (!permit->load() && ok) { m_client->disarmTransmit(); }
        QMetaObject::invokeMethod(this,[this,revision,ok] {
            if (revision != m_txSessionRevision) { return; }
            if (!ok || m_txSessionIdentity != transmitSessionIdentity() || !m_connected) {
                disableTransmitSession();
                m_txSessionError = QStringLiteral("TX session could not be enabled for the current radio state.");
                qCWarning(lcAnanTx) << m_txSessionError;
                return;
            }
            TransmitDelta zero;
            zero.rfPower = m_txPowerPercent;
            emit transmitChanged(zero);
            m_txSessionArming = false;
            m_txSessionEnabled = true;
            rememberTransmitSession(m_txSessionControl->antenna);
            emit capabilitiesChanged();
            emitSliceState();
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}

ReceiveDispatch AnanBackend::requestTransmitControl(const TransmitControlRequest& request,
                                                    TunePowerContext context)
{
    Q_UNUSED(context);
    if (!request.valid()) { return ReceiveDispatch::Unsupported; }
    using Field = TransmitControlRequest::Field;
    if (request.field == Field::MonitorEnabled || request.field == Field::MonitorLevel) {
        const auto setting = std::get<MonitorSetting>(request.value);
        setTxMonitor(setting.enabled,setting.level);
        return ReceiveDispatch::Dispatched;
    }
    if (request.field == Field::TunePower) { setTunePower(std::get<int>(request.value)); return ReceiveDispatch::Dispatched; }
    if (request.field == Field::RfPower && m_connected) {
        setTxPower(std::get<int>(request.value)); return ReceiveDispatch::Dispatched;
    }
    if (request.field == Field::MicGain || request.field == Field::CwSpeed
        || request.field == Field::CwBreakIn || request.field == Field::CwDelay
        || request.field == Field::CwSidetone || request.field == Field::CwIambic
        || request.field == Field::CwIambicMode || request.field == Field::CwSwap) { return ReceiveDispatch::LocalOnly; }
    return ReceiveDispatch::Unsupported;
}

void AnanBackend::setTxMonitor(bool on, int level)
{
    m_txMonitor = on;
    m_txMonitorLevel = std::clamp(level,0,100);
    QMetaObject::invokeMethod(m_client,[client = m_client,on] { client->setTransmitMonitor(on); },Qt::QueuedConnection);
    TransmitDelta delta;
    delta.sbMonitor = on;
    delta.monGainSb = m_txMonitorLevel;
    emit transmitChanged(delta);
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
        band = m_txSessionBand,percent = m_tuneActive ? m_tunePowerPercent : m_txPowerPercent,operation = m_transmitDispatchOperation] {
        client->setTransmitPower(profile,band,percent,operation);
    },Qt::QueuedConnection);
}

void AnanBackend::setTune(bool on, int percent, const TxCoordinator::Operation& operation,
                          const TxCoordinator::Completion& completion)
{
    if (!TxCoordinator::Command{operation,on}.permitsDispatch(TxCoordinator::monotonicMs())
        || (on && (!transmitSessionCurrent() || percent < 0 || percent > 100))) { completion.finish(); return; }
    m_tuneActive = on; m_tunePowerPercent = std::clamp(percent,0,100);
    setKeying(on,operation,completion);
    if (on && m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
        const auto permit = m_txSessionPermit;
        const auto dispatch = m_transmitDispatchOperation;
        QMetaObject::invokeMethod(m_client,[client=m_client,permit,dispatch] {
            if (permit->load() && dispatch.permitsDispatch(TxCoordinator::monotonicMs())) { client->setTransmitCarrier(true,true); }
        },Qt::QueuedConnection);
    }
}

void AnanBackend::setTunePower(int percent)
{
    m_tunePowerPercent = std::clamp(percent,0,100);
    if (!m_tuneActive || !transmitSessionCurrent() || !m_txSessionProfile) { return; }
    QMetaObject::invokeMethod(m_client,[client=m_client,profile=*m_txSessionProfile,band=m_txSessionBand,
        percent=m_tunePowerPercent,operation=m_transmitDispatchOperation] {
        client->setTransmitPower(profile,band,percent,operation);
    },Qt::QueuedConnection);
}

void AnanBackend::setCwKeying(bool down, bool breakIn, int delayMs,
    const TxCoordinator::Operation& operation, const TxCoordinator::Completion& completion)
{
    const auto mode = transmitMode(modeFromString(m_mode));
    if (!TxCoordinator::Command{operation,down}.permitsDispatch(TxCoordinator::monotonicMs())
        || (down && (!transmitSessionCurrent() || !mode || !mode->cw))) { completion.finish(); return; }
    if (m_cwHangTimer) { m_cwHangTimer->stop(); }
    m_cwReleaseCompletion.finish(); m_cwReleaseCompletion = {};
    if (down && !m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
        if (!breakIn) { completion.finish(); return; }
        m_cwAutoKeyed = true; setKeying(true,operation);
    }
    const auto dispatch = m_transmitDispatchOperation;
    const auto permit = m_txSessionPermit;
    QMetaObject::invokeMethod(m_client,[client=m_client,permit,dispatch,down] {
        if (permit && permit->load() && (down ? dispatch.permitsDispatch(TxCoordinator::monotonicMs()) : dispatch.permitsCleanup())) {
            client->setTransmitCarrier(true,down);
        }
    },Qt::QueuedConnection);
    if (!down && m_cwAutoKeyed) {
        if (!m_cwHangTimer) {
            m_cwHangTimer = new QTimer(this); m_cwHangTimer->setSingleShot(true);
            connect(m_cwHangTimer,&QTimer::timeout,this,[this] {
                const auto completion = m_cwReleaseCompletion; m_cwReleaseCompletion = {};
                m_cwAutoKeyed = false;
                setKeying(false,m_cwReleaseOperation,completion);
            });
        }
        m_cwReleaseOperation = operation; m_cwReleaseCompletion = completion;
        m_cwHangTimer->start(std::max(6,std::clamp(delayMs,0,2000)));
    } else { completion.finish(); }
}

int AnanBackend::finishTxAudio(const TxCoordinator::Context& context)
{
    const auto operation = m_transmitDispatchOperation;
    if (!transmitSessionCurrent() || !operation.permitsDispatch(TxCoordinator::monotonicMs())
        || !context.permitsDispatch(TxCoordinator::monotonicMs())) { return 0; }
    QMetaObject::invokeMethod(m_client,[client=m_client,operation,context] {
        if (operation.permitsDispatch(TxCoordinator::monotonicMs())) { client->finishTransmitAudio(context); }
    },Qt::QueuedConnection);
    // Queued PCM is observed atomically; 128 ms covers DSP flush and protocol FIFO.
    const std::size_t pending = m_transmitQueuedBytes->load(std::memory_order_relaxed) / 4 + m_client->queuedTransmitFrames();
    return static_cast<int>((pending * 1000 + AnanTxDsp::kInputRate - 1) / AnanTxDsp::kInputRate) + 128;
}

ReceiveDispatch AnanBackend::requestTxSlice(int id, bool enabled, const RadioControlCompletion& completion,
                                           const RadioControlFence& current)
{
    if ((current && !current()) || id != 0 || !m_connected || !primaryVisible()
        || !p2TransmitProfile(m_discoveredBoardId,m_pendingParams.model,m_pendingParams.overrideDetection)) {
        if (completion) { completion({RadioControlReply::Kind::Refused,-1,QStringLiteral("Only the supported primary slice can be selected for TX")}); }
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
    m_restoreTxAntenna.reset();
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
    const bool supported = m_connected && m_discoveryInfoReceived && (m_pendingParams.protocol == 1 || m_pendingParams.protocol == 2)
        && p2TransmitProfile(m_discoveredBoardId,m_pendingParams.model,m_pendingParams.overrideDetection)
        && transmitMode(modeFromString(m_mode)).has_value();
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
    const int outputRate = m_pendingParams.protocol == 1 ? 48000 : 192000;
    QMetaObject::invokeMethod(m_dspBuildContext,[this,cancelled,ticket,mode,outputRate] {
        if (cancelled->load()) { return; }
        auto channel = std::make_unique<AnanTxDsp>();
        std::string error;
        const bool ok = channel->configure(mode,&error,outputRate);
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
                const auto antenna = std::exchange(m_restoreTxAntenna,std::nullopt);
                if (installed && antenna) { enableTransmitSession(*antenna); }
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
        const quint64 revision = m_txSessionRevision;
        QMetaObject::invokeMethod(m_client,[this,client = m_client,cleanup,completion,revision] {
            const bool ok = client->setTransmitKeying(false,cleanup);
            QMetaObject::invokeMethod(this,[this,revision,ok] {
                if (revision != m_txSessionRevision) { return; }
                if (!ok) {
                    disableTransmitSession();
                    m_txSessionError = QStringLiteral("TX key-off was refused by the local transport.");
                    qCWarning(lcAnanTx) << m_txSessionError;
                    return;
                }
                if (!m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
                    publishLocalTransmitState(false);
                }
                QTimer::singleShot(100,this,[this] {
                    if (m_connected && !m_rateChanging && !m_receiveBuilding && !m_receiveCalibrationRun
                        && !m_droopCalibrator.isRunning()
                        && !m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
                        muteReceiveForTransmit(false);
                    }
                });
            },Qt::QueuedConnection);
            completion.finish();
        },Qt::QueuedConnection);
        return;
    }
    if (!transmitSessionCurrent() || !m_txSessionProfile || !m_txSessionControl) {
        qCWarning(lcAnanTx) << "ANAN TX key-on refused:" << transmitSessionRefusal();
        completion.finish();
        return;
    }
    P2TxControl control = *m_txSessionControl;
    const auto power = transmitPowerForPercent(*m_txSessionProfile,m_txSessionBand,m_tuneActive ? m_tunePowerPercent : m_txPowerPercent);
    if (!power) { completion.finish(); disableTransmitSession(); return; }
    control.drive = power->drive;
    qCInfo(lcAnanTx) << "ANAN TX key-on request" << "model" << static_cast<int>(m_pendingParams.model)
        << "board" << m_discoveredBoardId << "protocol" << m_pendingParams.protocol
        << "frequencyHz" << control.frequencyHz << "antenna" << control.antenna << "drive" << control.drive;
    const bool continuing = m_transmitOperation.sameOperation(operation)
        && m_transmitDispatchOperation.permitsDispatch(TxCoordinator::monotonicMs());
    const auto permit = m_txSessionPermit;
    if (!continuing) {
        const auto held = m_tuneActive ? operation : m_cwAutoKeyed ? operation.heldCwKeying() : operation.heldKeying();
        m_transmitDispatchOperation = held.withKeyingPermit([permit] { return permit->load(); });
    }
    m_transmitOperation = operation;
    // Mute demodulation before key-on; the independent IQ spectrum stays live.
    muteReceiveForTransmit(true);
    const auto dispatch = m_transmitDispatchOperation;
    const quint64 revision = m_txSessionRevision;
    QMetaObject::invokeMethod(m_client,[this,operation,dispatch,completion,control,permit,revision,continuing,
                                      mode = modeFromString(m_mode)] {
        if (!permit->load() || !operation.permitsDispatch(TxCoordinator::monotonicMs())) { completion.finish(); return; }
        const bool ok = m_client->transmitMode() == mode
            && (continuing || m_client->armStagedTransmit(control))
            && m_client->setTransmitKeying(true,dispatch);
        qCInfo(lcAnanTx) << "ANAN TX local transport key-on accepted:" << ok;
        QMetaObject::invokeMethod(this,[this,revision,operation,ok] {
            if (revision != m_txSessionRevision || !operation.sameOperation(m_transmitOperation)
                || !operation.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
            if (ok) {
                publishLocalTransmitState(true);
            } else {
                disableTransmitSession();
                m_txSessionError = QStringLiteral("MOX/PTT request was refused by the TX audio or transport path.");
                qCWarning(lcAnanTx) << m_txSessionError;
            }
        },Qt::QueuedConnection);
        completion.finish();
    }, Qt::QueuedConnection);
}

void AnanBackend::submitTxAudio(const QByteArray& pcm, int sampleRateHz, TxAudioSource source,
    const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    const auto operation = m_transmitDispatchOperation;
    const auto mode = transmitMode(modeFromString(m_mode));
    if (!m_connected || !mode || mode->cw || m_tuneActive || !m_client || !m_ioThread
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
