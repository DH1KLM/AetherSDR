#pragma once

#include "core/VaraProtocol.h"
#include "core/VaraReceiver.h"
#include "core/VaraTransmitter.h"

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QTimer>

namespace AetherSDR {
namespace Vara {

// Autonomous ARQ Session and Turn-Taking Controller.
//
// Coordinates bidirectional data exchange between the native Receiver and
// Transmitter, executing the ISS (Information Sending Station) and IRS
// (Information Receiving Station) protocol states.
class SessionController : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Calling,
        Listening,
        Connected_ISS,
        Connected_IRS,
        Disconnecting
    };

    explicit SessionController(QObject* parent = nullptr);

    // Initialisation and dependency injection
    void setTransmitter(Transmitter* transmitter);
    void setReceiver(Receiver* receiver);

    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected_ISS || m_state == State::Connected_IRS; }

    QString myCall() const { return m_myCall; }
    QString remoteCall() const { return m_remoteCall; }
    int currentSpeedLevel() const { return m_speedLevel; }

    // ── Session Control Actions ─────────────────────────────────────────

    void setMyCall(const QString& callsign);
    void startListening();
    void connectTo(const QString& remoteCall);
    void disconnectSession();
    void abortSession();

    // Queue payload data for ARQ transmission
    void sendData(const QByteArray& data);

signals:
    void stateChanged(SessionController::State newState);
    void connected(const QString& remoteCall);
    void disconnected();
    void dataReceived(const QByteArray& payload);
    void speedLevelChanged(int newLevel);
    void snrReported(double snrDb);
    void retryCountChanged(int retries);

private slots:
    void onFrameDetected(const QVector<int>& symbols, double confidence);
    void onFramePayload(const QByteArray& payload, int residual);
    void onRetryTimeout();

private:
    void setState(State newState);
    void transmitNextTurn();
    void adaptRate(bool ackReceived, double snr);

    Transmitter* m_transmitter{nullptr};
    Receiver* m_receiver{nullptr};

    State m_state{State::Disconnected};
    QString m_myCall;
    QString m_remoteCall;
    int m_speedLevel{1};

    QByteArray m_txBuffer;
    QTimer m_retryTimer;
    int m_retryCount{0};
    static constexpr int kMaxRetries = 5;
    static constexpr int kRetryTimeoutMs = 6000;

    int m_consecutiveAcks{0};
    int m_consecutiveNacks{0};
};

} // namespace Vara
} // namespace AetherSDR
