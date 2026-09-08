#pragma once

#include "core/backends/anan/P1Protocol.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QList>
#include <QObject>

#include <complex>
#include <cstdint>
#include <vector>

class QUdpSocket;

namespace AetherSDR::anan {

//DH1KLM: Standard OpenHPSDR Protocol 1 transport client for ANAN radios.
//DH1KLM: Stage 1 is intentionally RX-focused: discovery, Metis start/stop,
//DH1KLM: EP2 C&C pacing and EP6 IQ reception. TX IQ is not enabled here yet.
//DH1KLM: The byte-level representation remains in P1Protocol so this class only
//DH1KLM: owns sockets, timers and connection state.
class P1Client final : public QObject {
    Q_OBJECT

public:
    explicit P1Client(QObject* parent = nullptr);
    ~P1Client() override;

    struct Params {
        QHostAddress host;
        quint16 port = p1::kMetisPort;
        int sampleRate = 48000;
        int receiverCount = 1;
        int boardMaxRx = 7;
        std::uint32_t rxFrequencyHz = 14'200'000;
        std::uint32_t txFrequencyHz = 14'200'000;
        bool watchdogEnabled = true;
    };

    struct Discovered {
        p1::DiscoveryReply reply;
        QHostAddress address;
    };

    //DH1KLM: Discovery uses a temporary UDP socket so it can be called before
    //DH1KLM: start() and does not interfere with an active radio session.
    QList<Discovered> discover(
        int timeoutMs = 2000,
        const QHostAddress& broadcast = QHostAddress::Broadcast,
        quint16 port = p1::kMetisPort);

    Q_INVOKABLE bool start(const Params& params);
    Q_INVOKABLE void stop();

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] bool isLinkUp() const noexcept { return m_linkUp; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }

    Q_INVOKABLE void setRxFrequencyHz(std::uint32_t hz);
    Q_INVOKABLE void setRxFrequencyHz(int receiverIndex, std::uint32_t hz);
    Q_INVOKABLE void setTxFrequencyHz(std::uint32_t hz);
    Q_INVOKABLE void setSampleRate(int sampleRate);
    Q_INVOKABLE void setReceiverCount(int count);

    //DH1KLM: Socket-free packet builder used by unit tests and later backend code.
    std::array<std::uint8_t, p1::kUsbPacketSize> buildNextControlPacket();

signals:
    void linkUp();
    void linkDown();
    void connectFailed(const QString& reason);
    void iqBlocksReady(
        const std::vector<std::vector<std::complex<float>>>& blocks);
    void dropsUpdated(quint64 drops);

private slots:
    void onReadyRead();
    void onEp2PacerTick();
    void onWatchdogTick();
    void onStartRetry();

private:
    static int effectiveReceiverCount(const Params& params) noexcept;

    void resetSessionState();
    void sendStart();
    void sendStop();
    void sendControlPacket();
    void sendPrimingBurst(int countPerBurst);

    static constexpr int kEp2AudioRateHz = 48000;
    static constexpr int kEp2SamplesPerPacket = 126;
    static constexpr int kEp2PacerTickMs = 2;
    static constexpr int kEp6FlowingWithinMs = 100;
    static constexpr int kWatchdogTickMs = 25;
    static constexpr int kConnectTimeoutMs = 2000;
    static constexpr int kSilenceTimeoutMs = 2000;
    static constexpr int kStartRetryMs = 300;
    static constexpr int kMaxStartAttempts = 5;
    static constexpr int kMaxBurstPerTick = 16;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_ep2Timer = nullptr;
    QTimer* m_watchdogTimer = nullptr;
    QTimer* m_connectTimer = nullptr;
    QTimer* m_startRetryTimer = nullptr;

    QHostAddress m_host;
    quint16 m_port = p1::kMetisPort;
    Params m_params;

    std::vector<p1::Cc> m_rxFreqBanks;
    p1::Cc m_txFreqBank{};
    p1::Cc m_configBank{};

    std::uint32_t m_sequence = 0;
    std::uint64_t m_ep2Sent = 0;
    int m_startAttempts = 0;

    bool m_running = false;
    bool m_linkUp = false;

    QElapsedTimer m_ep2Clock;
    QElapsedTimer m_sinceLastEp6;

    std::uint32_t m_expectedRxSequence = 0;
    bool m_haveRxSequence = false;
    quint64 m_drops = 0;
};

} // namespace AetherSDR::anan
