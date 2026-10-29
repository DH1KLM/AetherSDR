#include "P2Client.h"
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>

namespace AetherSDR::anan {
bool P2Client::sendControl(std::span<const std::uint8_t> packet, quint16 port)
{
    qint64 sent = -1;
    if (m_controlSendOverride) {
        sent = m_controlSendOverride(packet, port);
    } else if (m_socket) {
        sent = m_socket->writeDatagram(reinterpret_cast<const char*>(packet.data()),
            static_cast<qint64>(packet.size()), m_host, port);
    }
    if (sent > 0) { m_txBytes += static_cast<quint64>(sent); }
    return sent == static_cast<qint64>(packet.size());
}

HighPriorityPacket P2Client::receiveHighPriority() const
{
    return buildHighPriority(true, sharedFreqWords(), m_bypassAdc0Filters, m_bypassAdc1Filters,
        m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna, m_firstDdcIndex,
        m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr,
        m_alexAttenuationDb, &m_receiveFilterPlan, m_frequencyCorrection, m_speakerAmpMuted);
}

bool P2Client::writeTransmitKey(bool key)
{
    if (!m_running || !m_discoveryValidated || !hasControlTransport()) { return false; }
    HighPriorityPacket packet = receiveHighPriority();
    if (key) {
        if (!m_transmitControl || !m_transmitProfile) { return false; }
        const auto encoded = buildP2TransmitControl(packet, *m_transmitControl,
            *m_transmitProfile, true, m_highPrioritySequence++);
        return encoded && sendControl(*encoded, kHighPriorityPort);
    }
    for (int i = 0; i < 4; ++i) {
        packet[i] = static_cast<std::uint8_t>(m_highPrioritySequence >> (24 - 8 * i));
    }
    ++m_highPrioritySequence;
    return sendControl(packet, kHighPriorityPort);
}

bool P2Client::sendHighPriority()
{
    const qint64 now = TxCoordinator::monotonicMs();
    const auto writer = [this](bool key) { return writeTransmitKey(key); };
    const bool wasKeyed = m_transmitGate.keyCommandAccepted();
    if (!m_transmitGate.poll(now, writer)) {
        abortTransmit(QStringLiteral("TX stop write failed"));
        return false;
    }
    if (wasKeyed && !m_transmitGate.keyCommandAccepted()) {
        clearTransmitAudio();
        m_transmitOperation = {};
        return true;
    }
    if (m_transmitGate.keyCommandAccepted()) {
        if (!m_transmitGate.setKeying(true, m_transmitOperation, now, writer)) {
            abortTransmit(QStringLiteral("TX high-priority write refused"));
            return false;
        }
        return true;
    }
    return sendControl(receiveHighPriority(), kHighPriorityPort);
}

bool P2Client::configureTransmit(const P2TxControl& control)
{
    if (!m_running || !m_linkUp || !m_discoveryValidated || !m_transmitProfile
        || !hasControlTransport() || !m_generalPacket || m_transmitGate.enabled()
        || m_transmitGate.stopPending() || !validP2TxControl(control)) { return false; }
    m_transmitControl = control;
    return true;
}

bool P2Client::updateTransmitDrive(int drive, const TxCoordinator::Operation& operation)
{
    if (drive < 0 || drive > 255 || !m_transmitControl || !m_transmitGate.keyCommandAccepted()
        || !operation.sameAuthority(m_transmitOperation)
        || !operation.permitsDispatch(TxCoordinator::monotonicMs())) { return false; }
    m_transmitControl->drive = drive;
    return sendHighPriority();
}

bool P2Client::enableTransmit(bool enabled)
{
    if (enabled && m_transmitGate.keyCommandAccepted()) { return false; }
    if (enabled && (!m_running || !m_linkUp || !m_discoveryValidated
        || !hasControlTransport() || !m_transmitProfile || !m_transmitControl || !m_generalPacket)) {
        return false;
    }
    if (!enabled) { clearTransmitAudio(); }
    if (!m_transmitGate.setEnabled(enabled, TxCoordinator::monotonicMs(),
            [this](bool key) { return writeTransmitKey(key); })) { return false; }
    if (!enabled) { m_transmitOperation = {}; }
    if (!m_generalPacket || !m_discoveryValidated || !hasControlTransport()) { return !enabled; }
    (*m_generalPacket)[58] = enabled ? (m_transmitControl->disablePa ? 0 : 1) : (m_receiveDisablePa ? 0 : 1);
    if (!sendControl(*m_generalPacket, kRadioPort) || !sendMicOptions()) {
        m_transmitGate.setEnabled(false, TxCoordinator::monotonicMs(),
            [this](bool key) { return writeTransmitKey(key); });
        (*m_generalPacket)[58] = m_receiveDisablePa ? 0 : 1;
        sendControl(*m_generalPacket, kRadioPort);
        return false;
    }
    return true;
}

bool P2Client::setTransmitKeying(bool key, const TxCoordinator::Operation& operation)
{
    if (!key) {
        if (!operation.sameAuthority(m_transmitOperation)) { return false; }
        clearTransmitAudio();
    } else if (!m_running || !m_linkUp || !m_discoveryValidated || !m_transmitControl) {
        return false;
    } else if (!m_transmitGate.keyCommandAccepted()) {
        clearTransmitAudio();
    }
    const bool accepted = m_transmitGate.setKeying(key, operation, TxCoordinator::monotonicMs(),
        [this](bool value) { return writeTransmitKey(value); });
    if (accepted) {
        m_transmitOperation = key ? operation : TxCoordinator::Operation{};
        if (key) {
            std::fill(m_speakerPending.begin(),m_speakerPending.end(),0);
            ensureTransmitTimer();
        }
    } else if (m_transmitGate.stopPending()) {
        abortTransmit(QStringLiteral("TX key write failed"));
    }
    return accepted;
}

void P2Client::clearTransmitAudio()
{
    m_carrierEnabled = false; m_carrier = {};
    if (m_transmitTimer) { m_transmitTimer->stop(); }
    m_transmitStream.clear();
    m_transmitStreamStarted = false;
    m_transmitClock.invalidate();
}

void P2Client::abortTransmit(const QString& reason)
{
    if (!enableTransmit(false)) { stop(); }
    emit transmitFault(reason);
}

bool P2Client::enqueueTransmitIq(std::span<const std::complex<float>> iq,
    const TxCoordinator::Operation& operation, const TxCoordinator::Context& context)
{
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_running || !m_linkUp || m_carrierEnabled || !m_transmitGate.keyCommandAccepted()
        || !operation.sameAuthority(m_transmitOperation) || !operation.permitsDispatch(now)) {
        return false;
    }
    if (!m_transmitStreamStarted) {
        m_transmitClock.start();
        if (!m_transmitStream.begin(operation, context, transmitElapsedNs(), now)) {
            abortTransmit(QStringLiteral("TX media provenance refused"));
            return false;
        }
        m_transmitStreamStarted = true;
    }
    if (!m_transmitStream.enqueue(iq, context, now)) {
        abortTransmit(QStringLiteral("TX IQ queue refused input"));
        return false;
    }
    ensureTransmitTimer();
    return true;
}

void P2Client::ensureTransmitTimer()
{
    if (!m_transmitTimer) {
        m_transmitTimer = new QTimer(this);
        m_transmitTimer->setTimerType(Qt::PreciseTimer);
        m_transmitTimer->setInterval(1);
        connect(m_transmitTimer, &QTimer::timeout, this, &P2Client::onTransmitTick);
    }
    if (!m_transmitTimer->isActive()) { m_transmitTimer->start(); }
}

void P2Client::setCarrier(bool enabled, bool down)
{
    if (!transmitActive()) { return; }
    if (enabled && !m_carrierEnabled) {
        m_carrier = {};
        m_transmitClock.start();
        m_transmitStreamStarted = m_transmitStream.beginCarrier(m_transmitOperation,0,TxCoordinator::monotonicMs());
    }
    m_carrierEnabled = enabled;
    m_carrier.key(down);
    ensureTransmitTimer();
}

void P2Client::onTransmitTick()
{
    if (!m_running || (!m_transmitGate.keyCommandAccepted() && !m_transmitGate.stopPending())) {
        clearTransmitAudio();
        return;
    }
    const qint64 now = TxCoordinator::monotonicMs();
    if (!m_running || !hasControlTransport() || !m_transmitGate.poll(now,
            [this](bool key) { return writeTransmitKey(key); })
        || !m_transmitGate.keyCommandAccepted()) {
        abortTransmit(QStringLiteral("TX operation ended"));
        return;
    }
    if (!m_transmitStreamStarted) { return; }
    const auto writer = [this](const TxIqPacket& packet) { return sendControl(packet,kTxIqPort); };
    const auto result = m_carrierEnabled
        ? m_transmitStream.pumpCarrier(transmitElapsedNs(),now,m_transmitGate,
            [this] { return m_carrier.sample(192000); },writer)
        : m_transmitStream.pump(transmitElapsedNs(),now,m_transmitGate,writer);
    if (result == AnanP2TxStream::Result::Refused || result == AnanP2TxStream::Result::WriteFailed) {
        abortTransmit(QStringLiteral("TX IQ transport stopped"));
    }
}
} // namespace AetherSDR::anan
