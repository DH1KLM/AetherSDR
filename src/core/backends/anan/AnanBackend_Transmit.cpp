#include "AnanBackend.h"

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

void AnanBackend::setKeying(bool key, const TxCoordinator::Operation& operation,
    const TxCoordinator::Completion& completion)
{
    const bool supported = m_connected && m_pendingParams.protocol == 2
        && p2TransmitProfile(m_discoveredBoardId, m_pendingParams.model)
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
