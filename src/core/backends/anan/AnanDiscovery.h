#pragma once

#include "core/RadioDiscovery.h"   //DH1KLM RadioInfo

#include <QHash>
#include <QHostAddress>
#include <QSet>
#include <QList>
//DH1KLM For QNetworkAddressEntry, broadcastTargets()' parameter. The whole-module
//DH1KLM include rather than the per-class one because this tree already depends on
//DH1KLM it in six places and it is the form every one of them uses.
#include <QNetworkInterface>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

//DH1KLM Decides which discovery problems reach the log (category aether.discovery).
//DH1KLM Sweeps repeat every few seconds, so each condition is logged once when it
//DH1KLM starts and again only after it has cleared and come back. AnanDiscovery
//DH1KLM reports every bind, target list, send, listed reply and closed sweep here;
//DH1KLM socket-free, so tests/anan_discovery_test drives it without a network.
class AnanDiscoveryLog {
public:
    //DH1KLM 3 unanswered sweeps that sent a probe: at AnanDiscovery's default 5000 ms
    //DH1KLM interval, at least 15 s of silence. A sweep no probe left adds time, not
    //DH1KLM a count.
    static constexpr int kSilentSweepsBeforeNotice = 3;

    //DH1KLM The wildcard socket did not bind; the next sweep tries again.
    void bindFailed(const QString& error);
    //DH1KLM A logged bind failure logs again only after a bind has succeeded.
    void bindSucceeded();
    //DH1KLM One sweep's targets, as broadcastTargets() returned them. The global
    //DH1KLM broadcast alone means no interface offered a subnet to probe.
    void targetsListed(const QList<QHostAddress>& targets);
    //DH1KLM No datagram left for `target`. Logged once, and again only after a
    //DH1KLM probe has left for it.
    void sendFailed(const QHostAddress& target, const QString& error, int protocol = 2);
    //DH1KLM A probe left for `target`: the open sweep can count as unanswered.
    void sendSucceeded(const QHostAddress& target, int protocol = 2);
    //DH1KLM An accepted P1/P2 reply was listed in the picker during the open sweep.
    void replyListed();
    //DH1KLM The open sweep's reply window ended. A sweep no probe left is not
    //DH1KLM silence: bindFailed or sendFailed has named its cause. The silence line
    //DH1KLM counts the targets a probe left in the sweep it closes.
    void sweepClosed();
    void reset();

private:
    QSet<QString> m_sendFailures;   //DH1KLM targets logged and not sent to since
    QSet<QString> m_sentTargets;    //DH1KLM targets a probe left in the open sweep
    int m_silentSweeps = 0;
    bool m_answered = false;        //DH1KLM a reply was listed in the open sweep
    bool m_bindFailureReported = false;
    bool m_noSubnetReported = false;
};

//DH1KLM: OpenHPSDR P1/P2 discovery with normalized board identities.
//DH1KLM Only implemented receive profiles are advertised. Status 0x03 means in use.
class AnanDiscovery : public QObject {
    Q_OBJECT

public:
    explicit AnanDiscovery(QObject* parent = nullptr);
    ~AnanDiscovery() override;

    //DH1KLM Begin periodic sweeps, retrying socket creation after a bind failure.
    void start(int intervalMs = 5000);
    void stop();
    //DH1KLM Broadcast P1/P2 probes now when started; a stopped scan opens no socket.
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
    friend class AnanDiscoveryTestAccess;
    bool openSocket();

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
    AnanDiscoveryLog m_log;
    QHash<QString, Seen> m_seen;   // keyed by serial (the MAC string)
};

}  // namespace AetherSDR::anan
