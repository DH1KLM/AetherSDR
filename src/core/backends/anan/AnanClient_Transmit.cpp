#include "AnanClient.h"
#include "AnanPlannedResource.h"

#include <QTimer>

#include <cstring>
#include <utility>

namespace AetherSDR::anan {
bool AnanClient::prepareTransmit(std::unique_ptr<AnanTxDsp> dsp, const P2TxControl& control)
{
    auto owned = retainPlannedResource(std::move(dsp),m_transmitPlanner);
    if (m_protocol != 2 || m_transmitKeyed || !owned || !owned->mode()) { return false; }
    m_transmitConfigured = false;
    if (!m_p2->enableTransmit(false) || !m_p2->configureTransmit(control)
        || !m_p2->enableTransmit(true)) { return false; }
    clearTransmitAudio();
    m_transmitDsp = std::move(owned);
    m_transmitConfigured = true;
    return true;
}


bool AnanClient::stageTransmitDsp(std::shared_ptr<AnanTxDsp> dsp)
{
    if (m_protocol != 2 || m_transmitKeyed || m_transmitConfigured || !dsp || !dsp->mode()) { return false; }
    clearTransmitAudio();
    m_transmitDsp = std::move(dsp);
    return true;
}

void AnanClient::discardStagedTransmit()
{
    if (m_transmitConfigured || m_transmitKeyed) { return; }
    clearTransmitAudio();
    m_transmitDsp.reset();
}
bool AnanClient::setTransmitPower(const TransmitPowerProfile& profile, int band, int percent,
                                  const TxCoordinator::Operation& operation)
{
    if (!m_transmitConfigured || !m_transmitKeyed || !operation.sameAuthority(m_transmitOperation)
        || !operation.permitsDispatch(TxCoordinator::monotonicMs())) { return false; }
    const auto power = transmitPowerForPercent(profile,band,percent);
    if (!power || profile.limitsWatts[band] <= 0) {
        failTransmit(QStringLiteral("TX power profile or disabled band refused"));
        return false;
    }
    if (!m_p2->updateTransmitDrive(power->drive,operation)) {
        failTransmit(QStringLiteral("TX power update refused"));
        return false;
    }
    return true;
}

void AnanClient::clearTransmitAudio()
{
    if (m_transmitAudioTimer) { m_transmitAudioTimer->stop(); }
    m_transmitHead = 0;
    m_transmitCount = 0;
    m_transmitContext = {};
    m_transmitSource.reset();
    m_transmitMediaStarted = false;
    if (m_transmitDsp && !m_transmitDsp->end()) {
        m_transmitDsp.reset();
    }
}

void AnanClient::failTransmit(const QString& reason)
{
    m_transmitKeyed = false;
    m_transmitOperation = {};
    if (!m_p2->enableTransmit(false)) { m_p2->stop(); }
    clearTransmitAudio();
    emit transmitFault(reason);
}

bool AnanClient::setTransmitKeying(bool key, const TxCoordinator::Operation& operation)
{
    if (m_protocol != 2 || (key && (!m_transmitDsp || !m_transmitConfigured))) { return false; }
    if (!key && !operation.sameAuthority(m_transmitOperation)) { return false; }
    if (key && !m_p2->transmitActive()) { clearTransmitAudio(); }
    if (key && !m_transmitDsp) { m_p2->enableTransmit(false); return false; }
    const bool accepted = m_p2->setTransmitKeying(key, operation);
    if (!key) { clearTransmitAudio(); }
    if (accepted) {
        m_transmitKeyed = key;
        m_transmitOperation = key ? operation : TxCoordinator::Operation{};
    }
    return accepted;
}

bool AnanClient::submitTransmitAudio(const QByteArray& pcm, int rate, TxAudioSource source,
    const TxCoordinator::Operation& operation, const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (m_protocol != 2 || !m_transmitKeyed || !m_p2->transmitActive() || !m_transmitDsp
        || !operation.sameAuthority(m_transmitOperation) || !operation.permitsDispatch(now)
        || !context.permitsDispatch(now)) { return false; }
    const std::size_t frames = static_cast<std::size_t>(pcm.size()) / 4;
    if (rate != AnanTxDsp::kInputRate || pcm.isEmpty() || pcm.size() % 4 != 0
        || frames > kTransmitAudioCapacity - m_transmitCount
        || (source != TxAudioSource::Microphone && source != TxAudioSource::ClientLeveled
            && source != TxAudioSource::EngineGenerated)
        || (m_transmitSource && *m_transmitSource != source)
        || (m_transmitMediaStarted && !context.sameContext(m_transmitContext))) {
        failTransmit(QStringLiteral("TX audio format, capacity or source refused"));
        return false;
    }
    if (!m_transmitMediaStarted) {
        if (!m_transmitDsp->begin(context, now)) {
            failTransmit(QStringLiteral("TX audio DSP start refused"));
            return false;
        }
        m_transmitContext = context;
        m_transmitSource = source;
        m_transmitMediaStarted = true;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        qint16 left = 0, right = 0;
        std::memcpy(&left, pcm.constData() + 4*i, sizeof(left));
        std::memcpy(&right, pcm.constData() + 4*i + 2, sizeof(right));
        m_transmitAudio[(m_transmitHead + m_transmitCount++) % kTransmitAudioCapacity]
            = (static_cast<float>(left) + static_cast<float>(right)) / 65536.0f;
    }
    if (!m_transmitAudioTimer) {
        m_transmitAudioTimer = new QTimer(this);
        m_transmitAudioTimer->setTimerType(Qt::PreciseTimer);
        m_transmitAudioTimer->setInterval(1);
        connect(m_transmitAudioTimer, &QTimer::timeout, this, &AnanClient::pumpTransmitAudio);
    }
    if (!m_transmitAudioTimer->isActive()) { m_transmitAudioTimer->start(); }
    return true;
}

void AnanClient::pumpTransmitAudio()
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_transmitKeyed || !m_transmitDsp || !m_p2->transmitActive()) {
        clearTransmitAudio();
        m_transmitKeyed = false;
        return;
    }
    if (!m_transmitOperation.permitsDispatch(now) || !m_transmitContext.permitsDispatch(now)) {
        failTransmit(QStringLiteral("TX audio provenance expired"));
        return;
    }
    if (m_transmitCount < AnanTxDsp::kInputSamples) {
        if (m_transmitAudioTimer) { m_transmitAudioTimer->stop(); }
        return;
    }
    if (m_p2->transmitQueueFreeSamples() < AnanTxDsp::kOutputSamples) { return; }
    std::array<float, AnanTxDsp::kInputSamples> mono{};
    for (std::size_t i = 0; i < mono.size(); ++i) {
        mono[i] = m_transmitAudio[(m_transmitHead + i) % kTransmitAudioCapacity];
    }
    std::array<std::complex<float>, AnanTxDsp::kOutputSamples> iq{};
    const auto result = m_transmitDsp->process(mono, m_transmitContext, now, iq);
    if (result == AnanTxDsp::Result::WouldBlock) { return; }
    if (result != AnanTxDsp::Result::Output) {
        failTransmit(QStringLiteral("TX audio DSP exchange refused"));
        return;
    }
    m_transmitHead = (m_transmitHead + mono.size()) % kTransmitAudioCapacity;
    m_transmitCount -= mono.size();
    if (!m_p2->enqueueTransmitIq(iq, m_transmitOperation, m_transmitContext) && m_transmitKeyed) {
        failTransmit(QStringLiteral("TX audio could not reach the IQ transport"));
    }
}
} // namespace AetherSDR::anan
