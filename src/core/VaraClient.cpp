#include "VaraClient.h"

#include "LogManager.h"

namespace AetherSDR {

VaraClient::VaraClient(QObject* parent)
    : QObject(parent)
    , m_command(new QTcpSocket(this))
    , m_data(new QTcpSocket(this))
{
    connect(m_command, &QTcpSocket::readyRead, this, &VaraClient::onCommandReadyRead);
    connect(m_command, &QTcpSocket::connected, this, &VaraClient::onCommandConnected);
    connect(m_command, &QTcpSocket::disconnected, this, &VaraClient::onSocketDisconnected);
    connect(m_command, &QTcpSocket::errorOccurred, this, &VaraClient::onSocketError);

    connect(m_data, &QTcpSocket::readyRead, this, &VaraClient::onDataReadyRead);
    connect(m_data, &QTcpSocket::connected, this, &VaraClient::onDataConnected);
    connect(m_data, &QTcpSocket::disconnected, this, &VaraClient::onSocketDisconnected);
    connect(m_data, &QTcpSocket::errorOccurred, this, &VaraClient::onSocketError);
}

void VaraClient::connectToModem(const QString& host, quint16 commandPort, quint16 dataPort)
{
    // The modem's own convention is that the data port is the command port
    // plus one. Honour an explicit override for unusual setups.
    const quint16 effectiveDataPort = dataPort != 0 ? dataPort
                                                    : static_cast<quint16>(commandPort + 1);

    qCInfo(lcVara) << "connecting to VARA modem at" << host << commandPort << "/"
                   << effectiveDataPort;

    disconnectFromModem();
    m_command->connectToHost(host, commandPort);
    m_data->connectToHost(host, effectiveDataPort);
}

void VaraClient::disconnectFromModem()
{
    m_command->abort();
    m_data->abort();
    m_commandBuffer.clear();
    m_pending.clear();
    resetSessionState();
    m_modemVersion.clear();
    m_announcedConnected = false;
}

bool VaraClient::isModemConnected() const
{
    return m_command->state() == QAbstractSocket::ConnectedState
           && m_data->state() == QAbstractSocket::ConnectedState;
}

void VaraClient::onCommandConnected()
{
    // Both sockets must be up before the modem is usable, and they connect
    // independently, so whichever finishes second announces.
    if (isModemConnected() && !m_announcedConnected) {
        m_announcedConnected = true;
        emit modemConnected();
    }
}

void VaraClient::onDataConnected()
{
    if (isModemConnected() && !m_announcedConnected) {
        m_announcedConnected = true;
        emit modemConnected();
    }
}

void VaraClient::onSocketDisconnected()
{
    if (!m_announcedConnected) {
        return;
    }
    m_announcedConnected = false;
    resetSessionState();
    m_pending.clear();
    m_commandBuffer.clear();
    emit modemDisconnected();
}

void VaraClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    const QString which = (socket == m_data) ? QStringLiteral("data")
                                             : QStringLiteral("command");
    const QString message = socket ? socket->errorString() : QStringLiteral("unknown error");
    qCWarning(lcVara) << "VARA" << which << "socket error:" << message;
    emit modemError(QStringLiteral("%1 channel: %2").arg(which, message));
}

void VaraClient::onCommandReadyRead()
{
    m_commandBuffer.append(m_command->readAll());

    const QList<QByteArray> frames = Vara::splitFrames(m_commandBuffer);
    for (const QByteArray& frame : frames) {
        const Vara::Message msg = Vara::parseMessage(QString::fromLatin1(frame));
        handleMessage(msg);
    }
}

void VaraClient::onDataReadyRead()
{
    const QByteArray payload = m_data->readAll();
    if (!payload.isEmpty()) {
        emit dataReceived(payload);
    }
}

void VaraClient::handleMessage(const Vara::Message& msg)
{
    switch (msg.type) {
    case Vara::MessageType::Ok:
    case Vara::MessageType::Wrong: {
        // Replies carry no identity — the oldest outstanding command owns
        // this one. If the queue is empty the modem has answered something we
        // did not send (or we lost sync), which is worth a warning rather than
        // a silent drop.
        if (m_pending.isEmpty()) {
            qCWarning(lcVara) << "unmatched" << msg.raw << "- no command outstanding";
            return;
        }
        const QString label = m_pending.dequeue();
        if (msg.type == Vara::MessageType::Ok) {
            emit commandAccepted(label);
        } else {
            qCWarning(lcVara) << "modem rejected command:" << label;
            emit commandRejected(label);
        }
        return;
    }

    case Vara::MessageType::IAmAlive:
        // Keepalive. Nothing to do; the socket state is our liveness signal.
        return;

    case Vara::MessageType::Connected:
        m_remoteCall = msg.destination;
        emit linkEstablished(msg.source, msg.destination, msg.bandwidthHz);
        return;

    case Vara::MessageType::Disconnected:
        resetSessionState();
        emit linkClosed();
        return;

    case Vara::MessageType::Pending:
        emit pendingChanged(true);
        return;

    case Vara::MessageType::CancelPending:
        emit pendingChanged(false);
        return;

    case Vara::MessageType::Version:
        m_modemVersion = msg.text;
        emit versionReceived(msg.text);
        return;

    case Vara::MessageType::Registered:
        emit registeredCallsigns(msg.callsigns);
        return;

    case Vara::MessageType::Busy:
        emit busyChanged(msg.flag);
        return;

    case Vara::MessageType::Ptt:
        emit pttChanged(msg.flag);
        return;

    case Vara::MessageType::SignalToNoise:
        emit signalToNoiseChanged(msg.value);
        return;

    case Vara::MessageType::Buffer:
        emit bufferChanged(msg.count);
        return;

    case Vara::MessageType::CleanTxBuffer:
        emit cleanTxBufferResult(msg.cleanTx);
        return;

    case Vara::MessageType::Link:
        emit linkStateChanged(msg.link);
        return;

    case Vara::MessageType::Encryption:
        emit encryptionStateChanged(msg.encryption);
        return;

    case Vara::MessageType::Bitrate:
        emit bitrateChanged(msg.speedLevel, msg.bitsPerSecond, msg.bitrateDirection);
        return;

    case Vara::MessageType::CqFrame:
        emit cqFrameReceived(msg.source, msg.bandwidthHz);
        return;

    case Vara::MessageType::MissingSoundcard:
        // Repeats every few seconds while unresolved, so warn rather than
        // spam at a higher level, and let the consumer latch it.
        qCWarning(lcVara) << "modem reports no usable soundcard - no link is possible";
        emit missingSoundcard();
        return;

    case Vara::MessageType::Tune:
        // We never issue TUNE (see VaraProtocol.h), but the modem can report
        // one driven from its own UI. Surface it rather than dropping it.
        qCDebug(lcVara) << "modem reported TUNE" << msg.value;
        return;

    case Vara::MessageType::Unknown:
        qCDebug(lcVara) << "unrecognised VARA message:" << msg.raw;
        emit unknownMessage(msg.raw);
        return;
    }
}

void VaraClient::resetSessionState()
{
    m_remoteCall.clear();
}

QString VaraClient::send(const QByteArray& bytes, const QString& label)
{
    if (m_command->state() != QAbstractSocket::ConnectedState) {
        qCWarning(lcVara) << "dropping" << label << "- command socket not connected";
        return QString();
    }
    m_command->write(bytes);
    m_pending.enqueue(label);
    return label;
}

QString VaraClient::requestVersion()
{
    return send(Vara::cmdVersion(), QStringLiteral("VERSION"));
}

QString VaraClient::setMyCalls(const QStringList& callsigns)
{
    // Reject locally rather than spending a round trip to be told WRONG.
    if (callsigns.isEmpty() || callsigns.size() > Vara::kMaxMyCallCallsigns) {
        qCWarning(lcVara) << "MYCALL needs 1 to" << Vara::kMaxMyCallCallsigns
                          << "callsigns, got" << callsigns.size();
        return QString();
    }
    return send(Vara::cmdMyCall(callsigns), QStringLiteral("MYCALL"));
}

QString VaraClient::setBandwidth(Vara::Bandwidth bandwidth)
{
    if (bandwidth == Vara::Bandwidth::Unknown) {
        qCWarning(lcVara) << "refusing to send an unknown bandwidth";
        return QString();
    }
    return send(Vara::cmdBandwidth(bandwidth), QStringLiteral("BW"));
}

QString VaraClient::setCompression(Vara::Compression compression)
{
    return send(Vara::cmdCompression(compression), QStringLiteral("COMPRESSION"));
}

QString VaraClient::setChat(bool on)
{
    return send(Vara::cmdChat(on), QStringLiteral("CHAT"));
}

QString VaraClient::listen(bool on)
{
    return send(Vara::cmdListen(on), QStringLiteral("LISTEN"));
}

QString VaraClient::listenCq()
{
    return send(Vara::cmdListenCq(), QStringLiteral("LISTEN CQ"));
}

QString VaraClient::cleanTxBuffer()
{
    return send(Vara::cmdCleanTxBuffer(), QStringLiteral("CLEANTXBUFFER"));
}

void VaraClient::setTransmitInhibited(bool inhibited)
{
    if (m_txInhibited == inhibited)
        return;
    m_txInhibited = inhibited;
    qCInfo(lcVara) << (inhibited ? "transmit inhibited" : "transmit permitted");
}

QString VaraClient::connectTo(const QString& source, const QString& destination)
{
    if (m_txInhibited) {
        // Refused rather than silently dropped: a caller that wired this up by
        // mistake should be able to see why nothing happened.
        qCWarning(lcVara) << "CONNECT refused - transmit is inhibited";
        emit transmitInhibited(QStringLiteral("CONNECT"));
        return QString();
    }
    qCInfo(lcVara) << "CONNECT" << source << "->" << destination;
    return send(Vara::cmdConnect(source, destination), QStringLiteral("CONNECT"));
}

QString VaraClient::sendCq(const QString& callsign, Vara::Bandwidth bandwidth)
{
    if (m_txInhibited) {
        qCWarning(lcVara) << "CQFRAME refused - transmit is inhibited";
        emit transmitInhibited(QStringLiteral("CQFRAME"));
        return QString();
    }
    if (bandwidth == Vara::Bandwidth::Unknown) {
        qCWarning(lcVara) << "refusing to send CQ with an unknown bandwidth";
        return QString();
    }
    qCInfo(lcVara) << "CQFRAME" << callsign << Vara::toString(bandwidth);
    return send(Vara::cmdCqFrame(callsign, bandwidth), QStringLiteral("CQFRAME"));
}

QString VaraClient::disconnectLink()
{
    return send(Vara::cmdDisconnect(), QStringLiteral("DISCONNECT"));
}

QString VaraClient::abort()
{
    return send(Vara::cmdAbort(), QStringLiteral("ABORT"));
}

qint64 VaraClient::write(const QByteArray& payload)
{
    if (m_txInhibited) {
        qCWarning(lcVara) << "refusing to write" << payload.size()
                          << "bytes - transmit is inhibited";
        emit transmitInhibited(QStringLiteral("DATA"));
        return -1;
    }
    if (m_data->state() != QAbstractSocket::ConnectedState) {
        qCWarning(lcVara) << "dropping" << payload.size() << "bytes - data socket not connected";
        return -1;
    }
    return m_data->write(payload);
}

}  // namespace AetherSDR
