#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QList>
// For QNetworkAddressEntry, broadcastTargets()' parameter. The whole-module
// include rather than the per-class one because this tree already depends on
// it in six places and it is the form every one of them uses.
#include <QNetworkInterface>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

//DH1KLM: OpenHPSDR P1/P2 discovery with normalized board identities.
// Only implemented receive profiles are advertised. Status 0x03 means in use.
class AnanDiscovery : public QObject {
    Q_OBJECT

public:
    explicit AnanDiscovery(QObject* parent = nullptr);
    ~AnanDiscovery() override;

    // Begin periodic sweeps. Safe to call twice; restarts the cadence.
    void start(int intervalMs = 5000);
    void stop();
    // Broadcast one discovery datagram now (also called by the interval timer).
    void sweepNow();

    [[nodiscard]] bool isRunning() const noexcept;

    // Every address one sweep sends its discovery datagram to, built from the
    // host's own interface addresses.
    //
    // Directed broadcasts reach a second NIC on a distinct subnet even when
    // the global broadcast follows a different route. Targets are per subnet,
    // not per interface: two NICs sharing a subnet still use the preferred route.
    //
    // 255.255.255.255 IS ALWAYS FIRST IN THE RESULT, and that is the point: the
    // directed addresses are added to what this already did rather than
    // replacing it, so a host where enumeration comes back empty or wrong is no
    // worse off than before. Entries are deduplicated and IPv4-only; loopback
    // and entries without a broadcast domain (/31, /32 or no address) are skipped.
    //
    // Static and taking its input so it can be tested without a host that has
    // the interesting network shape -- which is most hosts.
    [[nodiscard]] static QList<QHostAddress> broadcastTargets(
        const QList<QNetworkAddressEntry>& entries);

    // Canonical "AA:BB:CC:DD:EE:FF" rendering of a discovery reply's MAC, which IS
    // RadioInfo::serial for this family. Must match Hl2Discovery::macToSerial and be
    // byte-identical between broadcast sweeps and directed probes: the serial is the
    // auto-reconnect key and the nickname key.
    static QString macToSerial(const std::array<std::uint8_t, 6>& mac);

    //DH1KLM: The operator's custom name, or `fallback`. The nickname is
    // persisted client-side by MAC/serial, independently of the physical board.
    static QString effectiveNickname(const QString& family, const QString& serial,
                                     const QString& fallback);
    // Store (or clear, with an empty name) the client-side nickname.
    static void setNickname(const QString& family, const QString& serial,
                            const QString& name);

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private slots:
    void onReadyRead();
    void onSweepTimer();

private:
    // Radios not seen for this many consecutive sweeps are reported lost.
    // Same slack Hl2Discovery uses, for the same reason: absorb one dropped
    // reply on a busy LAN without flapping the picker.
    static constexpr int kMissedSweepsBeforeLost = 3;

    struct Seen {
        RadioInfo info;
        int missedSweeps = 0;
        int preferredProtocolMissedSweeps = 0;
    };

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHash<QString, Seen> m_seen;   // keyed by serial (the MAC string)
};

}  // namespace AetherSDR::anan
