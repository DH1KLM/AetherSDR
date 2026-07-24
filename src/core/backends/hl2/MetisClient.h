#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QList>
#include <QObject>

#include <complex>
#include <cstdint>
#include <vector>

#include "core/backends/hl2/MetisProtocol.h"

class QUdpSocket;

namespace AetherSDR::hl2 {

// Owns the Hermes-Lite 2 UDP wire (HPSDR Protocol 1 / "Metis"): discovery,
// start/stop, the config+gain+freq Command&Control round-robin (paced 1:1 with
// the EP6 IQ torrent), and EP6 ingest into normalized IQ blocks. Below the seam;
// the future Hl2Backend owns one MetisClient plus an Hl2RxDsp.
//
// Phase 1a drives the socket event-driven on the thread this object lives on;
// relocating it onto a dedicated RX thread (as FlexBackend does with
// PanadapterStream) is a later refinement once the data plane is wired through.
//
// RX-ONLY: every C&C it sends comes from MetisProtocol's even-C0 encoders, so
// the MOX bit is never set — this class cannot key the radio.
class MetisClient : public QObject {
    Q_OBJECT

public:
    explicit MetisClient(QObject* parent = nullptr);
    ~MetisClient() override;

    struct Params {
        QHostAddress host;
        quint16 port = kMetisPort;
        SampleRate sampleRate = SampleRate::R48k;
        std::uint32_t rxFrequencyHz = 10'000'000;
        int lnaGainDb = 20;
    };

    // A discovered radio: its Metis reply plus the address to connect to.
    struct Discovered {
        DiscoveryReply reply;
        QHostAddress address;
    };

    // Blocking discovery broadcast; returns HPSDR/HL2 replies (deduped by MAC)
    // seen within timeoutMs. Safe to call before start().
    QList<Discovered> discover(int timeoutMs = 2000,
                               const QHostAddress& broadcast = QHostAddress::Broadcast,
                               quint16 port = kMetisPort);

    bool start(const Params& params);   // bind, send start + priming C&C, begin ingest
    void stop();                        // send stop, close socket
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }

    // Live control — latched into the next C&C round sent to the radio.
    void setRxFrequencyHz(std::uint32_t hz);
    void setSampleRate(SampleRate rate);
    void setLnaGainDb(int db);

signals:
    void linkUp();                                                  // first EP6 seen
    void linkDown();                                               // stopped
    void iqBlockReady(const std::vector<std::complex<float>>& block);  // one per EP6 packet
    void dropsUpdated(quint64 drops);                             // cumulative EP6 gaps
    // No EP6 arrived within kConnectTimeoutMs of start() — the radio is off,
    // unreachable, or already streaming to a different client.
    void connectFailed(const QString& reason);

private slots:
    void onReadyRead();
    void onEp2PacerTick();
    void onWatchdogTick();

private:
    void sendControlPacket();           // one round-robin EP2 C&C packet
    // Send countPerBank C&C frames, pause, then countPerBank more. Run BEFORE
    // metis-start so the DDC latches sample rate / NCO / receiver count from a
    // real C&C frame; a stream started before any C&C has landed emits ADC-idle
    // samples (Q pinned to zero) until one does.
    void sendPrimingBurst(int countPerBank);

    // EP2 cadence follows the frame geometry, not the EP6 arrival rate: the
    // radio consumes one EP2 frame per kSamplesPerPacket samples, so at 48 kHz
    // that is 126/48000 s = 2625 us. Driving it from a wall clock (rather than
    // replying 1:1 to EP6) means a stalled receive path cannot starve the
    // radio's watchdog and deadlock the link.
    static constexpr int kEp2PacerTickMs     = 2;
    static constexpr int kEp2MaxBurstPerTick = 16;
    static constexpr int kWatchdogTickMs     = 25;
    static constexpr int kConnectTimeoutMs   = 2000;
    static constexpr int kSilenceTimeoutMs   = 2000;

    QTimer* m_ep2Timer = nullptr;         // paces EP2 off the wall clock
    QTimer* m_watchdogTimer = nullptr;    // EP6 silence detection
    QTimer* m_connectWatchdog = nullptr;  // single-shot: first-EP6 deadline
    QElapsedTimer m_ep2Clock;             // pacer reference clock
    QElapsedTimer m_sinceLastEp6;         // silence detection
    quint64 m_ep2Sent = 0;                // EP2 frames sent since m_ep2Clock
    qint64  m_ep2IntervalUs = 2625;       // derived from the sample rate
    bool    m_watchdogEnabled = true;     // gateware watchdog (anti-wedge)

    QUdpSocket* m_socket = nullptr;
    QHostAddress m_host;
    quint16 m_port = kMetisPort;
    Params m_params;

    // Current C&C, rebuilt from m_params on change. Touched only on this
    // object's thread (event-driven), so no synchronization is needed today.
    Cc m_ccConfig{};
    Cc m_ccGain{};
    Cc m_ccFreq{};

    std::uint32_t m_txSeq = 0;           // outgoing EP2 sequence
    unsigned m_roundRobin = 0;           // which register pair to send next
    std::uint32_t m_expectedRxSeq = 0;   // for EP6 drop detection
    bool m_haveRxSeq = false;
    quint64 m_drops = 0;
    bool m_running = false;
    bool m_linkUp = false;
    std::vector<std::complex<float>> m_block;   // reused per-packet decode buffer
};

}  // namespace AetherSDR::hl2
