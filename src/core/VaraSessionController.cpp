#include "core/VaraSessionController.h"

namespace AetherSDR {
namespace Vara {

SessionController::SessionController(QObject* parent)
    : QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &SessionController::onRetryTimeout);
}

void SessionController::setTransmitter(Transmitter* transmitter)
{
    // Wiring a transmitter in is plumbing, not consent — it must not arm.
    // The controller's autonomous paths (auto-ACK on frame receipt, retry on
    // timer) all run through synthesis, which refuses while inhibited, so an
    // unarmed session is inert on the air by construction.

    m_transmitter = transmitter;
}

void SessionController::setReceiver(Receiver* receiver)
{
    if (m_receiver) {
        disconnect(m_receiver, nullptr, this, nullptr);
    }
    m_receiver = receiver;
    if (m_receiver) {
        connect(m_receiver, &Receiver::frameDetected, this, &SessionController::onFrameDetected);
        connect(m_receiver, &Receiver::framePayload, this, &SessionController::onFramePayload);
    }
}

void SessionController::setMyCall(const QString& callsign)
{
    m_myCall = callsign;
}

void SessionController::startListening()
{
    setState(State::Listening);
}

void SessionController::connectTo(const QString& remoteCall)
{
    m_remoteCall = remoteCall;
    setState(State::Calling);

    if (m_transmitter) {
        // Send Connect Request burst
        QVector<float> audio = m_transmitter->synthesizeAckBurst(true, 1);
        if (!audio.isEmpty())
            emit m_transmitter->audioReady(audio);
    }

    m_retryCount = 0;
    m_retryTimer.start(kRetryTimeoutMs);
}

void SessionController::disconnectSession()
{
    setState(State::Disconnecting);
    if (m_transmitter) {
        QVector<float> audio = m_transmitter->synthesizeAckBurst(false, 8); // QRT burst
        if (!audio.isEmpty())
            emit m_transmitter->audioReady(audio);
    }
    setState(State::Disconnected);
    emit disconnected();
}

void SessionController::abortSession()
{
    m_retryTimer.stop();
    m_txBuffer.clear();
    setState(State::Disconnected);
    emit disconnected();
}

void SessionController::sendData(const QByteArray& data)
{
    m_txBuffer.append(data);
    if (m_state == State::Connected_ISS) {
        transmitNextTurn();
    }
}

void SessionController::setState(State newState)
{
    // Property 3 of the transmit arm (VaraTransmitter.h): the armed state must
    // not survive a session. A dropped link, an abort, a retry exhaustion or a
    // reconnect all come back inhibited, and the operator has to arm again
    // deliberately — the "not to recover or resync a state" clause of
    // Principle VI.
    //
    // Deliberately OUTSIDE the state-change guard below. An operator can arm
    // before connecting, so a controller sitting in Disconnected can already
    // hold an armed transmitter; routing this through the transition check
    // would make abortSession() a no-op in exactly that case and leave it
    // armed. Disarm on any request to enter Disconnected, transition or not.
    if (newState == State::Disconnected && m_transmitter)
        m_transmitter->disarm();

    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(newState);
    }
}

void SessionController::transmitNextTurn()
{
    if (!m_transmitter || m_txBuffer.isEmpty()) {
        return;
    }

    // Determine payload block size for current rate level
    int blockSize = (m_speedLevel <= 4) ? 32 : 64;
    QByteArray chunk = m_txBuffer.left(blockSize);

    m_transmitter->setSpeedLevel(m_speedLevel);
    QVector<float> audio = m_transmitter->synthesizeDataFrame(chunk);
    if (!audio.isEmpty()) {
        emit m_transmitter->audioReady(audio);
        m_retryTimer.start(kRetryTimeoutMs);
    }
}

void SessionController::adaptRate(bool ackReceived, double snr)
{
    emit snrReported(snr);

    if (ackReceived) {
        ++m_consecutiveAcks;
        m_consecutiveNacks = 0;

        // Step up if 2 consecutive ACKs and SNR is high enough
        if (m_consecutiveAcks >= 2 && m_speedLevel < 14) {
            ++m_speedLevel;
            m_consecutiveAcks = 0;
            emit speedLevelChanged(m_speedLevel);
        }
    } else {
        ++m_consecutiveNacks;
        m_consecutiveAcks = 0;

        // Step down immediately on NACK
        if (m_consecutiveNacks >= 1 && m_speedLevel > 1) {
            --m_speedLevel;
            emit speedLevelChanged(m_speedLevel);
        }
    }
}

void SessionController::onFrameDetected(const QVector<int>& symbols, double confidence)
{
    Q_UNUSED(symbols);
    Q_UNUSED(confidence);
}

void SessionController::onFramePayload(const QByteArray& payload, int residual)
{
    m_retryTimer.stop();
    m_retryCount = 0;

    if (m_state == State::Calling) {
        // Inbound connection confirmation
        setState(State::Connected_ISS);
        emit connected(m_remoteCall);
        if (!m_txBuffer.isEmpty()) {
            transmitNextTurn();
        }
        return;
    }

    if (residual == 0) {
        // Valid received packet
        adaptRate(true, 30.0);
        emit dataReceived(payload);

        if (m_state == State::Connected_IRS && m_transmitter) {
            // Send ACK
            QVector<float> ackAudio = m_transmitter->synthesizeAckBurst(true, 1);
            if (!ackAudio.isEmpty())
                emit m_transmitter->audioReady(ackAudio);
        }
    } else {
        // Parity errors -> NACK
        adaptRate(false, 10.0);
        if (m_state == State::Connected_IRS && m_transmitter) {
            QVector<float> nackAudio = m_transmitter->synthesizeAckBurst(false, 0);
            if (!nackAudio.isEmpty())
                emit m_transmitter->audioReady(nackAudio);
        }
    }
}

void SessionController::onRetryTimeout()
{
    if (++m_retryCount > kMaxRetries) {
        abortSession();
        return;
    }

    emit retryCountChanged(m_retryCount);
    adaptRate(false, 0.0); // Retrying -> degrade rate

    if (m_state == State::Connected_ISS) {
        transmitNextTurn();
    } else if (m_state == State::Calling) {
        connectTo(m_remoteCall);
    }
}

} // namespace Vara
} // namespace AetherSDR
