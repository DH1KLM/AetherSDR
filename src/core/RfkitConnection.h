#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "RfkitProtocol.h"

namespace AetherSDR {

//DH1KLM Peripheral transport for an RF-KIT amplifier.
//DH1KLM
//DH1KLM A standalone Ethernet device with no FlexRadio awareness at all, so this is
//DH1KLM a peripheral(rfkit) accessory alongside AcomConnection/SpeConnection/
//DH1KLM VkampConnection/Kpa1500Connection, not an IRadioBackend implementor. See
//DH1KLM docs/architecture/rfkit-amplifier-design.md.
//DH1KLM
//DH1KLM The device speaks a REST API over HTTP/1.1 (RfkitProtocol.h), not a
//DH1KLM streaming socket protocol, and it never pushes: every reading below arrives
//DH1KLM because this class asked for it. So the shape differs from the socket-based
//DH1KLM peripherals in two ways:
//DH1KLM
//DH1KLM  * "Connected" is not a TCP state. connectNetwork() issues GET /info, and
//DH1KLM    the link counts as up only when that returns an RF-KIT-shaped document.
//DH1KLM    It counts as down again after kMaxConsecutiveFailures transport failures
//DH1KLM    in a row, so one dropped request does not flicker the applet.
//DH1KLM  * Requests are strictly serialized, one in flight at a time. The device is
//DH1KLM    a small embedded controller, and a command that has to queue behind a
//DH1KLM    full poll round on a congested link is better than a burst that makes it
//DH1KLM    drop requests. Commands jump the queue; polls are only enqueued when the
//DH1KLM    queue is empty, so a slow device lowers the poll rate instead of
//DH1KLM    building a backlog.
//DH1KLM
//DH1KLM Like every peripheral here, control writes never latch an optimistic value:
//DH1KLM the device is the authority on its own state, and the applet repaints from
//DH1KLM the reply (constitution Principle II).
class RfkitConnection : public QObject {
    Q_OBJECT

public:
    explicit RfkitConnection(QObject* parent = nullptr);
    ~RfkitConnection() override;

    bool isConnected() const { return m_connected; }
    QString description() const;  //DH1KLM "192.168.1.60:8080", for status display

    void connectNetwork(const QString& host, quint16 port = Rfkit::kDefaultPort);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    //DH1KLM Control (host -> amp). Each is a no-op, logged, when not connected.
    void setOperate(bool operate);
    //DH1KLM An antenna outside the API document's range for its type is refused,
    //DH1KLM logged, and never sent.
    void selectAntenna(const Rfkit::Antenna& antenna);
    //DH1KLM POST /error/reset. Clears the amplifier's error state; the device answers
    //DH1KLM 409 while the function is disabled, which is logged.
    void resetError();

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    //DH1KLM Full current snapshot, emitted when a reply actually changed something.
    void statusUpdated(const AetherSDR::Rfkit::Status& status);

private:
    struct Request {
        enum class Method { Get, Put, Post };
        Method method{Method::Get};
        QString path;
        QByteArray body;
        bool probe{false};  //DH1KLM the connect handshake (GET /info)
        bool command{false};  //DH1KLM operator-issued; kept in issue order ahead of polls
    };

    void enqueue(const Request& request, bool front = false);
    void pump();
    void onReplyFinished(QNetworkReply* reply, const Request& request, int epoch);
    //DH1KLM Returns true when the reply changed the snapshot.
    bool applyReply(const Request& request, const QByteArray& body);
    void onProbeSucceeded();
    void onTransportFailure(const QString& errorString, const Request& request);
    void onTransportDown(const QString& errorString);
    void poll();
    void enqueueFullRefresh();
    void armReconnect();
    void resetSession();

    QNetworkAccessManager m_nam;
    QUrl m_baseUrl;
    QPointer<QNetworkReply> m_reply;
    QQueue<Request> m_queue;
    bool m_inFlight{false};

    Rfkit::Status m_status;
    //DH1KLM The last body seen per path, so an unchanged slow reply does not
    //DH1KLM re-emit statusUpdated().
    QHash<QString, QByteArray> m_lastBody;
    //DH1KLM Paths already warned about for an unparseable 2xx body, so a firmware
    //DH1KLM that answers with an unexpected shape logs once per connection rather
    //DH1KLM than four times a second.
    QSet<QString> m_warnedPaths;

    QString m_lastHost;
    quint16 m_lastPort{Rfkit::kDefaultPort};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    //DH1KLM Bumped on every connect/disconnect. A reply carries the epoch it was
    //DH1KLM issued under, and one from a previous session is discarded unread: a
    //DH1KLM late answer from attempt N must never touch attempt N+1 (#2380).
    int m_epoch{0};
    int m_consecutiveFailures{0};
    static constexpr int kMaxConsecutiveFailures = 3;

    //DH1KLM Per-request transfer timeout. Also bounds the connect handshake, so an
    //DH1KLM unreachable address reports failure here rather than after the OS TCP
    //DH1KLM connect timeout (~21s on Windows).
    static constexpr int kRequestTimeoutMs = 4000;
    //DH1KLM The transfer timeout above only fires when no bytes move; this bounds the
    //DH1KLM whole request, so a peer that trickles bytes cannot hold the one serialized
    //DH1KLM slot (and every command queued behind it) indefinitely. Principle VII.
    static constexpr int kRequestDeadlineMs = 10000;

    QTimer m_reconnectTimer;
    static constexpr int kReconnectIntervalMs = 5000;

    //DH1KLM /power and /data every tick; one of the slower, configuration-style
    //DH1KLM endpoints per tick in rotation (see poll()).
    QTimer m_pollTimer;
    QTimer m_deadlineTimer;
    static constexpr int kPollIntervalMs = 250;
    int m_slowPollIndex{0};
};

}  //DH1KLM namespace AetherSDR
