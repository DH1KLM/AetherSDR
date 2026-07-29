#pragma once

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTcpSocket>

#include "VaraProtocol.h"

namespace AetherSDR {

// Host-side client for a VARA modem (VARA HF / VARA FM / VARA SAT).
//
// The modem is a separate process — ours or someone else's, local or across
// the network — that owns the soundcard and the RF waveform. This class speaks
// its published TCP host interface: a command channel and a data channel. It
// is NOT a radio backend: VARA sits above the radio, driving whatever
// transceiver the operator has keyed up, so it does not implement
// IRadioBackend and does not belong under src/core/backends/.
//
// See VaraProtocol.h for the wire vocabulary and
// docs/varac-cleanroom-design.md for where that vocabulary came from.
//
// THREADING. Main thread only, like the other Qt socket clients in core.
// Nothing here touches the audio path.
//
// TRANSMIT (Principle VI). This class never transmits on its own. It has no
// auto-connect, no beacon, no periodic CQ, and no reconnect-and-retry that
// could key a radio unattended. connect(), sendCq() and write() transmit, and
// only when a caller invokes them. listen() is receive-only.
class VaraClient : public QObject {
    Q_OBJECT

public:
    explicit VaraClient(QObject* parent = nullptr);

    // Open both sockets. `dataPort` defaults to commandPort + 1, matching the
    // modem's own convention (8300/8301, second instance 8302/8303).
    void connectToModem(const QString& host,
                        quint16 commandPort = Vara::kDefaultCommandPort,
                        quint16 dataPort = 0);
    void disconnectFromModem();

    // True once BOTH sockets are up. The command channel alone is not enough
    // to carry a session.
    bool isModemConnected() const;

    // True between CONNECTED and DISCONNECTED — i.e. an ARQ link to a peer.
    bool isLinked() const { return !m_remoteCall.isEmpty(); }
    QString remoteCall() const { return m_remoteCall; }
    QString modemVersion() const { return m_modemVersion; }

    // ── Commands. Each queues an entry for OK/WRONG correlation and returns
    //    the queued label, so a caller can match commandAccepted/Rejected.
    //    All are no-ops when the command socket is down.
    QString requestVersion();
    // Registers 1..kMaxMyCallCallsigns callsigns. Rejects an empty or
    // oversized list locally rather than letting the modem answer WRONG.
    QString setMyCalls(const QStringList& callsigns);
    QString setBandwidth(Vara::Bandwidth bandwidth);
    QString setCompression(Vara::Compression compression);
    QString setChat(bool on);
    QString listen(bool on);
    QString listenCq();
    QString cleanTxBuffer();

    // These three key the transmitter.
    QString connectTo(const QString& source, const QString& destination);
    QString sendCq(const QString& callsign, Vara::Bandwidth bandwidth);
    QString disconnectLink();

    // Aborts the current transmission/session immediately.
    QString abort();

    // Payload onto the data channel. Returns bytes written, or -1 if the data
    // socket is not open. The modem meters its own progress via the BUFFER
    // notification (see bufferChanged).
    qint64 write(const QByteArray& payload);

signals:
    void modemConnected();
    void modemDisconnected();
    void modemError(const QString& message);

    // Correlated replies. `command` is the label returned by the call that
    // issued it.
    void commandAccepted(const QString& command);
    void commandRejected(const QString& command);

    void linkEstablished(const QString& source, const QString& destination, int bandwidthHz);
    void linkClosed();
    void pendingChanged(bool pending);

    void versionReceived(const QString& version);
    void registeredCallsigns(const QStringList& callsigns);
    void busyChanged(bool busy);
    void pttChanged(bool keyed);
    void signalToNoiseChanged(double snDb);
    void bufferChanged(int bytesOutstanding);
    void cleanTxBufferResult(Vara::CleanTxResult result);
    void linkStateChanged(Vara::LinkState state);
    void encryptionStateChanged(Vara::EncryptionState state);

    // The modem's adaptive speed level (1..11) and its throughput. Reported by
    // the modem itself, so the level is observable without demodulating.
    void bitrateChanged(int speedLevel, int bitsPerSecond);

    void cqFrameReceived(const QString& source, int bandwidthHz);

    // The modem has no usable audio device and cannot carry a session. It
    // repeats this every few seconds, so consumers should latch it rather than
    // treat each one as a new event.
    void missingSoundcard();

    // Payload off the data channel.
    void dataReceived(const QByteArray& payload);

    // Any message we did not recognise, so a newer modem is visible rather
    // than silently ignored.
    void unknownMessage(const QString& raw);

private slots:
    void onCommandReadyRead();
    void onDataReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onCommandConnected();
    void onDataConnected();
    void onSocketDisconnected();

private:
    // Writes a command and records `label` for OK/WRONG correlation. Returns
    // the label, or an empty string if the socket was not writable.
    QString send(const QByteArray& bytes, const QString& label);
    void handleMessage(const Vara::Message& msg);
    void resetSessionState();

    QTcpSocket* m_command = nullptr;
    QTcpSocket* m_data = nullptr;

    QByteArray m_commandBuffer;  // partial-frame carry-over

    // Outstanding commands awaiting OK/WRONG, oldest first. The modem's
    // replies carry no identity, so this ordering IS the correlation.
    QQueue<QString> m_pending;

    QString m_remoteCall;
    QString m_modemVersion;
    bool m_announcedConnected = false;
};

}  // namespace AetherSDR
