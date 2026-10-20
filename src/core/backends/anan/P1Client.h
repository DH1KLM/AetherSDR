#pragma once

#include "core/backends/anan/P1Protocol.h"
#include "core/backends/anan/P2Client.h"

#include <QElapsedTimer>
#include <functional>

namespace AetherSDR::anan {

//DH1KLM: P1 RX transport on the existing ANAN IO thread. P2 and HL2 keep
// their own wire clients; this class exposes no transmit operation.
class P1Client : public QObject {
    Q_OBJECT
public:
    explicit P1Client(QObject* parent = nullptr);
    ~P1Client() override;
    using Params = P2Client::Params;
    using LinkCounters = P2Client::LinkCounters;
    struct Profile {
        int numAdcs;
        int frameSlotCount;
        bool swapSpeaker;
        ReceiveFilterKind filterKind;
    };
    [[nodiscard]] static std::optional<Profile> profile(int boardId, bool redPitaya = false) noexcept;
    [[nodiscard]] static int maximumRateKsps(int boardId, bool redPitaya = false) noexcept;
    bool start(const Params& params, int timeoutMs = 2000);
    void stop();
    void setDdc0FrequencyHz(double hz);
    bool setReceiveStreams(const std::vector<DdcConfig>& streams,
                           const std::vector<double>& centers);
    Q_INVOKABLE void setFrontEndAttenuation(int adc, int db, int alexDb);
    void setStepAttenuationDb(int adc, int db);
    void setRxAntenna(int antenna);
    void setHardwareFilters(const QVariantMap& options);
    bool setDdcRateLive(int ddc, int rateKsps);
    void enqueueSpeakerAudio(const QByteArray& audio);
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }

signals:
    void linkUp();
    void linkDown();
    void connectionError(const QString& reason);
    void ddcIqReady(int index, const std::vector<std::complex<float>>& block);
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    void ddcSequenceGap(int ddcIndex);
    void linkCountersUpdated(const P2Client::LinkCounters& counters);
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 reportedReceivers);

private:
    friend struct P1ClientTestAccess;
    bool prepare(const Params& params);
    void handleDatagram(const QHostAddress& sender, quint16 port,
                        std::span<const std::uint8_t> bytes);
    bool send(std::span<const std::uint8_t> bytes);
    void drain();
    void drainAt(qint64 nowNs);
    void publishCounters();
    void fail(const QString& reason);
    void updateFilters();
    std::uint8_t nextAddress();
    QUdpSocket* m_socket = nullptr;
    QTimer* m_drainTimer = nullptr;
    QTimer* m_timeoutTimer = nullptr;
    QTimer* m_countersTimer = nullptr;
    QElapsedTimer m_clock;
    qint64 m_lastClockNs = 0;
    double m_frameCredit = 0;
    QHostAddress m_host;
    Params m_params;
    p1::ReceiveConfig m_config;
    std::optional<Profile> m_profile;
    std::vector<std::uint8_t> m_addresses;
    std::size_t m_addressIndex = 0;
    std::vector<std::int16_t> m_audio;
    std::vector<std::complex<float>> m_iq;
    std::optional<std::uint32_t> m_expectedSequence;
    std::uint32_t m_txSequence = 0;
    LinkCounters m_counters;
    bool m_running = false;
    bool m_validated = false;
    bool m_linkUp = false;
    bool m_audioPrimed = false;
    // Injection stays private; production always sends through its UDP socket.
    std::function<bool(std::span<const std::uint8_t>)> m_sendOverride;
};
} // namespace AetherSDR::anan
