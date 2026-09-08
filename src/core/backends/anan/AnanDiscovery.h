#pragma once

#include "core/RadioDiscovery.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

//DH1KLM: ANAN discovery is protocol-aware. P1 and P2 both use UDP/1024, so the
//DH1KLM: discovery socket sends both probes and classifies each reply by wire format.
//DH1KLM: Stage 1 enables only the ANAN-200D/Orion P1 picker entry; the existing
//DH1KLM: ANAN-G2/Saturn P2 entry remains unchanged.
class AnanDiscovery : public QObject {
    Q_OBJECT
public:
    enum class Protocol : std::uint8_t { Unknown = 0, P1 = 1, P2 = 2 };

    explicit AnanDiscovery(QObject* parent = nullptr);
    ~AnanDiscovery() override;

    void start(int intervalMs = 5000);
    void stop();
    void sweepNow();
    [[nodiscard]] bool isRunning() const noexcept;

    static QString macToSerial(const std::array<std::uint8_t, 6>& mac);
    static QString effectiveNickname(const QString& family, const QString& serial,
                                     const QString& fallback);
    static void setNickname(const QString& family, const QString& serial,
                            const QString& name);

    //DH1KLM: The connection layer can query the protocol selected from the actual
    //DH1KLM: discovery reply without changing the generic RadioInfo structure yet.
    [[nodiscard]] Protocol protocolForSerial(const QString& serial) const noexcept;
    [[nodiscard]] static Protocol protocolForModel(const QString& model) noexcept;

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

    //DH1KLM: Explicit discovery-to-connection handoff signal. The next backend
    //DH1KLM: step connects this signal to the ANAN P1Client factory/start path.
    void p1RadioReady(const RadioInfo& info);
    void protocolDetected(const QString& serial, int protocol);

private slots:
    void onReadyRead();
    void onSweepTimer();

private:
    struct Seen {
        RadioInfo info;
        Protocol protocol = Protocol::Unknown;
        int missedSweeps = 0;
    };

    void processP1Reply(const QByteArray& data, const QHostAddress& sender);
    void processP2Reply(const QByteArray& data, const QHostAddress& sender);
    void upsert(const RadioInfo& info, Protocol protocol);

    //DH1KLM: Standard OpenHPSDR P1 discovery response fields, confirmed against
    //DH1KLM: Thetis/NereusSDR: bytes 3..8 MAC, byte 9 firmware, byte 10 board ID,
    //DH1KLM: byte 19 Metis version and byte 20 reported receiver count.
    struct P1Reply {
        bool busy = false;
        std::array<std::uint8_t, 6> mac{};
        std::uint8_t firmware = 0;
        std::uint8_t boardId = 0;
        std::uint8_t metisVersion = 0;
        std::uint8_t numRxs = 0;
    };

    static bool parseP1Reply(const QByteArray& data, P1Reply& out) noexcept;
    static QString p1ModelName(std::uint8_t boardId);

    static constexpr int kMissedSweepsBeforeLost = 3;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHash<QString, Seen> m_seen;
};

} // namespace AetherSDR::anan
