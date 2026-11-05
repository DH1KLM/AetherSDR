#include "AnanClient.h"
#include "AnanPlannedResource.h"

#include <QTimer>

#include <cstring>
#include <cmath>
#include <numbers>
#include <utility>

namespace AetherSDR::anan {
bool AnanClient::prepareTransmit(std::unique_ptr<AnanTxDsp> dsp, const P2TxControl& control)
{
    auto owned = retainPlannedResource(std::move(dsp),m_transmitPlanner);
    if ((m_protocol != 1 && m_protocol != 2) || m_transmitKeyed || !owned || !owned->mode()
        || owned->outputSamples() != (m_protocol == 1 ? 256 : 1024)) { return false; }
    m_transmitConfigured = false;
    if (!transmitTransport([](auto* transport) { return transport->enableTransmit(false); }) || !transmitTransport([&](auto* transport) { return transport->configureTransmit(control); })
        || !transmitTransport([](auto* transport) { return transport->enableTransmit(true); })) { return false; }
    clearTransmitAudio();
    m_transmitDsp = std::move(owned);
    m_transmitConfigured = true;
    return true;
}


bool AnanClient::stageTransmitDsp(std::shared_ptr<AnanTxDsp> dsp)
{
    if ((m_protocol != 1 && m_protocol != 2) || m_transmitKeyed || m_transmitConfigured || !dsp || !dsp->mode()
        || dsp->outputSamples() != (m_protocol == 1 ? 256 : 1024)) { return false; }
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

bool AnanClient::armStagedTransmit(const P2TxControl& control)
{
    if (m_transmitKeyed && !transmitTransport([](auto* transport) { return transport->transmitActive(); })) {
        clearTransmitAudio();
        m_transmitKeyed = false;
        m_transmitOperation = {};
    }
    if ((m_protocol != 1 && m_protocol != 2) || !m_transmitDsp || !m_transmitDsp->mode() || m_transmitKeyed) { return false; }
    m_transmitConfigured = false;
    if (!transmitTransport([](auto* transport) { return transport->enableTransmit(false); }) || !transmitTransport([&](auto* transport) { return transport->configureTransmit(control); })
        || !transmitTransport([](auto* transport) { return transport->enableTransmit(true); })) { return false; }
    clearTransmitAudio();
    m_transmitConfigured = true;
    return true;
}

bool AnanClient::disarmTransmit(bool discardDsp)
{
    m_transmitConfigured = false;
    m_transmitKeyed = false;
    m_transmitOperation = {};
    const bool stopped = transmitTransport([](auto* transport) { return transport->enableTransmit(false); });
    if (!stopped) { transmitTransport([](auto* transport) { transport->stop(); }); }
    clearTransmitAudio();
    if (!transmitTransport([](auto* transport) { return transport->setPureSignalEnabled(false); })) {
        transmitTransport([](auto* transport) { transport->stop(); });
    }
    if (m_uncorrectedTransmitDsp) { m_transmitDsp = std::move(m_uncorrectedTransmitDsp); }
    m_pureSignalMode = PureSignalMode::Off;
    if (discardDsp) { m_transmitDsp.reset(); }
    return stopped;
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
    if (!transmitTransport([&](auto* transport) { return transport->updateTransmitDrive(power->drive,operation); })) {
        failTransmit(QStringLiteral("TX power update refused"));
        return false;
    }
    return true;
}

void AnanClient::clearTransmitAudio()
{
    if (m_pureSignalMode != PureSignalMode::Off && m_transmitDsp && m_transmitDsp->pureSignalChannel()) {
        const std::array<std::complex<float>,1> silence{};
        m_transmitDsp->pureSignalChannel()->feedPureSignal(silence, silence, false);
    }
    m_transmitFinishing = false; m_transmitFlushBlocks = 0; m_transmitCarrier = false;
    m_transmitTwoTone = false; m_transmitToneSample = 0;
    if (m_transmitAudioTimer) { m_transmitAudioTimer->stop(); }
    m_transmitHead = 0;
    m_transmitCount = 0;
    m_transmitPendingFrames.store(0,std::memory_order_relaxed);
    m_transmitContext = {};
    m_transmitSource.reset();
    m_transmitMediaStarted = false;
    m_transmitMeterMs = -1;
    if (m_transmitDsp && !m_transmitDsp->end()) {
        m_transmitDsp.reset();
    }
}

void AnanClient::failTransmit(const QString& reason)
{
    m_transmitKeyed = false;
    m_transmitOperation = {};
    if (!transmitTransport([](auto* transport) { return transport->enableTransmit(false); })) { transmitTransport([](auto* transport) { transport->stop(); }); }
    clearTransmitAudio();
    emit transmitFault(reason);
}

bool AnanClient::setTransmitKeying(bool key, const TxCoordinator::Operation& operation)
{
    if ((m_protocol != 1 && m_protocol != 2) || (key && (!m_transmitDsp || !m_transmitConfigured))) { return false; }
    if (!key && !operation.sameAuthority(m_transmitOperation)) { return false; }
    if (key && !transmitTransport([](auto* transport) { return transport->transmitActive(); })) { clearTransmitAudio(); }
    if (key && !m_transmitDsp) { transmitTransport([](auto* transport) { return transport->enableTransmit(false); }); return false; }
    const bool accepted = transmitTransport([&](auto* transport) { return transport->setTransmitKeying(key,operation); });
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
    if ((m_protocol != 1 && m_protocol != 2) || m_transmitCarrier || m_transmitFinishing || !m_transmitKeyed || !transmitTransport([](auto* transport) { return transport->transmitActive(); }) || !m_transmitDsp
        || !operation.sameAuthority(m_transmitOperation) || !operation.permitsDispatch(now)
        || !context.permitsDispatch(now)) { return false; }
    if (m_transmitTwoTone) { return false; }
    const std::size_t frames = static_cast<std::size_t>(pcm.size()) / 4;
    if (rate != AnanTxDsp::kInputRate || pcm.isEmpty() || pcm.size() % 4 != 0
        || (source != TxAudioSource::Microphone && source != TxAudioSource::ClientLeveled
            && source != TxAudioSource::EngineGenerated)) {
        failTransmit(QStringLiteral("TX audio format refused: rate=%1 bytes=%2 source=%3")
            .arg(rate).arg(pcm.size()).arg(static_cast<int>(source)));
        return false;
    }
    if ((m_transmitSource && *m_transmitSource != source)
        || (m_transmitMediaStarted && !context.sameContext(m_transmitContext))) {
        failTransmit(QStringLiteral("TX audio source or producer context changed during transmission"));
        return false;
    }
    if (frames > kTransmitAudioCapacity - m_transmitCount) {
        failTransmit(QStringLiteral("TX audio queue overflow: pending=%1 incoming=%2 capacity=%3")
            .arg(m_transmitCount).arg(frames).arg(kTransmitAudioCapacity));
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
    m_transmitPendingFrames.store(m_transmitCount,std::memory_order_relaxed);
    if (!m_transmitAudioTimer) {
        m_transmitAudioTimer = new QTimer(this);
        m_transmitAudioTimer->setTimerType(Qt::PreciseTimer);
        m_transmitAudioTimer->setInterval(1);
        connect(m_transmitAudioTimer, &QTimer::timeout, this, &AnanClient::pumpTransmitAudio);
    }
    if (!m_transmitAudioTimer->isActive()) { m_transmitAudioTimer->start(); }
    pumpTransmitAudio();
    return true;
}

void AnanClient::pumpTransmitAudio()
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_transmitKeyed || !m_transmitDsp || !transmitTransport([](auto* transport) { return transport->transmitActive(); })) {
        clearTransmitAudio();
        m_transmitKeyed = false;
        return;
    }
    if (!m_transmitOperation.permitsDispatch(now) || !m_transmitContext.permitsDispatch(now)) {
        failTransmit(QStringLiteral("TX audio provenance expired"));
        return;
    }
    for (int block = 0; block < 4; ++block) {
        if (!m_transmitKeyed || !m_transmitDsp) { return; }
        if (m_transmitTwoTone && m_transmitCount < AnanTxDsp::kInputSamples) {
            while (m_transmitCount < AnanTxDsp::kInputSamples) {
                const double phase = 2 * std::numbers::pi * m_transmitToneSample / AnanTxDsp::kInputRate;
                m_transmitAudio[(m_transmitHead + m_transmitCount++) % kTransmitAudioCapacity]
                    = static_cast<float>(0.49999 * (std::sin(700 * phase) + std::sin(1900 * phase)));
                m_transmitToneSample = (m_transmitToneSample + 1) % AnanTxDsp::kInputRate;
            }
        }
        if (m_transmitFinishing && m_transmitCount < AnanTxDsp::kInputSamples
            && (m_transmitCount > 0 || m_transmitFlushBlocks > 0)) {
            if (m_transmitCount == 0) { --m_transmitFlushBlocks; }
            while (m_transmitCount < AnanTxDsp::kInputSamples) {
                m_transmitAudio[(m_transmitHead + m_transmitCount++) % kTransmitAudioCapacity] = 0;
            }
        }
        if (m_transmitCount < AnanTxDsp::kInputSamples) {
            if (m_transmitAudioTimer) { m_transmitAudioTimer->stop(); }
            return;
        }
        const std::size_t outputSamples = m_transmitDsp->outputSamples();
        if (transmitTransport([](auto* transport) { return transport->transmitQueueFreeSamples(); }) < outputSamples) { return; }
        std::array<float, AnanTxDsp::kInputSamples> mono{};
        for (std::size_t i = 0; i < mono.size(); ++i) {
            mono[i] = m_transmitAudio[(m_transmitHead + i) % kTransmitAudioCapacity];
        }
        std::array<std::complex<float>, AnanTxDsp::kOutputSamples> iq{};
        const auto result = m_transmitDsp->process(mono,m_transmitContext,now,std::span(iq).first(outputSamples));
        if (result == AnanTxDsp::Result::WouldBlock) { return; }
        if (result != AnanTxDsp::Result::Output) {
            failTransmit(QStringLiteral("TX audio DSP exchange refused"));
            return;
        }
        if (m_transmitMeterMs < 0 || now - m_transmitMeterMs >= 50) {
            m_transmitMeterMs = now;
            const auto mic = m_transmitDsp->meter(WdspChannel::TransmitMeter::MicrophonePeak);
            const auto alc = m_transmitDsp->meter(WdspChannel::TransmitMeter::AlcPeak);
            const auto gain = m_transmitDsp->meter(WdspChannel::TransmitMeter::AlcGain);
            if (mic && alc && gain) { emit transmitMeters(*mic,*alc,*gain,m_transmitContext,m_transmitOperation); }
        }
        const QByteArray monitor = m_transmitDsp->monitorAudio(std::span(iq).first(outputSamples));
        if (m_transmitMonitor && !monitor.isEmpty()) {
            emit transmitMonitorAudio(monitor,m_transmitContext,m_transmitOperation);
        }
        m_transmitHead = (m_transmitHead + mono.size()) % kTransmitAudioCapacity;
        m_transmitCount -= mono.size();
        m_transmitPendingFrames.store(m_transmitCount,std::memory_order_relaxed);
        if (!transmitTransport([&](auto* transport) { return transport->enqueueTransmitIq(std::span(iq).first(outputSamples),m_transmitOperation,m_transmitContext); }) && m_transmitKeyed) {
            failTransmit(QStringLiteral("TX audio could not reach the IQ transport"));
            return;
        }
    }
}

bool AnanClient::startTransmitTwoTone(const TxCoordinator::Operation& operation,
    const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_transmitKeyed || !operation.sameAuthority(m_transmitOperation)) { return false; }
    if (!operation.permitsDispatch(now) || !context.permitsDispatch(now) || !m_transmitDsp
        || (m_transmitDsp->mode() != WdspChannel::Mode::Usb && m_transmitDsp->mode() != WdspChannel::Mode::Lsb)
        || m_transmitMediaStarted || !m_transmitDsp->begin(context, now)) {
        failTransmit(QStringLiteral("Two-tone TXA start refused")); return false;
    }
    m_transmitContext = context; m_transmitSource = TxAudioSource::EngineGenerated;
    m_transmitMediaStarted = true; m_transmitTwoTone = true; m_transmitToneSample = 0;
    if (!m_transmitAudioTimer) {
        m_transmitAudioTimer = new QTimer(this);
        m_transmitAudioTimer->setTimerType(Qt::PreciseTimer);
        m_transmitAudioTimer->setInterval(1);
        connect(m_transmitAudioTimer, &QTimer::timeout, this, &AnanClient::pumpTransmitAudio);
    }
    m_transmitAudioTimer->start(); pumpTransmitAudio();
    return true;
}

void AnanClient::setTransmitCarrier(bool enabled, bool down)
{
    if (!m_transmitKeyed || !m_transmitOperation.permitsCleanup()) { return; }
    if (enabled && !m_transmitCarrier) { clearTransmitAudio(); }
    m_transmitCarrier = enabled;
    transmitTransport([&](auto* transport) { transport->setCarrier(enabled,down); });
}

void AnanClient::finishTransmitAudio(const TxCoordinator::Context& context)
{
    if (!m_transmitKeyed || !m_transmitMediaStarted || !context.sameContext(m_transmitContext)
        || !context.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
    m_transmitFinishing = true; m_transmitFlushBlocks = 8;
    if (m_transmitAudioTimer) { m_transmitAudioTimer->start(); }
}
} // namespace AetherSDR::anan
