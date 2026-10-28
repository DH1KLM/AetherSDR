#include "AnanBackend.h"
#include "AnanSettings.h"
#include "AnanTransmitPower.h"
#include "AnanPlannedResource.h"

#include <QMetaObject>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

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
        {QStringLiteral("txEnabled"),false}, {QStringLiteral("preparation"),transmitPreparationState()}};
}

QString AnanBackend::transmitPreparationIdentity() const
{
    return QStringLiteral("%1:%2:%3:%4:%5:%6").arg(m_radioSerial).arg(m_connectGeneration)
        .arg(static_cast<int>(m_pendingParams.model)).arg(m_discoveredBoardId)
        .arg(m_pendingParams.protocol).arg(m_mode);
}

void AnanBackend::cancelTransmitPreparation()
{
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
    const bool supported = m_connected && m_pendingParams.protocol == 2
        && p2TransmitProfile(m_discoveredBoardId, m_pendingParams.model, m_pendingParams.overrideDetection)
        && (m_mode == QLatin1String("USB") || m_mode == QLatin1String("LSB"))
        && !m_rateChanging && !m_receiveBuilding;
    if (!TxCoordinator::Command{operation, key}.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_client || !m_ioThread || !m_ioThread->isRunning() || (key && !supported)) {
        completion.finish();
        return;
    }
    if (!key && !operation.sameAuthority(m_transmitOperation)) {
        completion.finish();
        return;
    }
    m_transmitOperation = key ? operation : TxCoordinator::Operation{};
    QMetaObject::invokeMethod(m_client, [client = m_client, key, operation, completion,
                                       mode = modeFromString(m_mode)] {
        if (!key || client->transmitMode() == mode) {
            client->setTransmitKeying(key, operation);
        }
        completion.finish();
    }, Qt::QueuedConnection);
}

void AnanBackend::submitTxAudio(const QByteArray& pcm, int sampleRateHz, TxAudioSource source,
    const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    const auto operation = m_transmitOperation;
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
