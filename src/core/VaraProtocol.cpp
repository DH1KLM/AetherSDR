#include "VaraProtocol.h"

#include <QRegularExpression>

namespace AetherSDR::Vara {

namespace {

// Every command is terminated by a bare CR. Not CRLF — a trailing LF is
// rejected by the modem.
constexpr char kTerminator = '\r';

QByteArray command(const QString& text)
{
    QByteArray out = text.toLatin1();
    out.append(kTerminator);
    return out;
}

// "ON" / "OFF" appear in several commands and in the BUSY and PTT
// notifications, so keep one spelling of each.
QString onOff(bool on)
{
    return on ? QStringLiteral("ON") : QStringLiteral("OFF");
}

}  // namespace

QList<QByteArray> splitFrames(QByteArray& buffer)
{
    QList<QByteArray> frames;

    int start = 0;
    for (;;) {
        const int end = buffer.indexOf(kTerminator, start);
        if (end < 0) {
            break;
        }
        // A doubled terminator yields an empty frame, which is not a message.
        if (end > start) {
            frames.append(buffer.mid(start, end - start));
        }
        start = end + 1;
    }

    // Whatever follows the last CR is an incomplete message. Keep it so the
    // next read can finish it.
    buffer.remove(0, start);
    return frames;
}

Message parseMessage(const QString& line)
{
    Message msg;
    msg.raw = line;

    // ── Exact matches first. Order matters here: "ENCRYPTED LINK" and
    //    "UNENCRYPTED LINK" must be recognised before the "LINK " prefix test
    //    below, and they use the opposite word order from "LINK ENCRYPTED".
    //    The modem emits both spellings.
    if (line == QLatin1String("OK")) {
        msg.type = MessageType::Ok;
        return msg;
    }
    if (line == QLatin1String("WRONG")) {
        msg.type = MessageType::Wrong;
        return msg;
    }
    if (line == QLatin1String("IAMALIVE")) {
        msg.type = MessageType::IAmAlive;
        return msg;
    }
    if (line == QLatin1String("DISCONNECTED")) {
        msg.type = MessageType::Disconnected;
        return msg;
    }
    if (line == QLatin1String("PENDING")) {
        msg.type = MessageType::Pending;
        return msg;
    }
    if (line == QLatin1String("CANCELPENDING")) {
        msg.type = MessageType::CancelPending;
        return msg;
    }
    if (line == QLatin1String("MISSING SOUNDCARD")) {
        msg.type = MessageType::MissingSoundcard;
        return msg;
    }
    if (line == QLatin1String("ENCRYPTED LINK")) {
        msg.type = MessageType::Link;
        msg.link = LinkState::Encrypted;
        return msg;
    }
    if (line == QLatin1String("UNENCRYPTED LINK")) {
        msg.type = MessageType::Link;
        msg.link = LinkState::Unencrypted;
        return msg;
    }

    // ── Prefixed forms.
    if (line.startsWith(QLatin1String("CONNECTED "))) {
        // "CONNECTED <source> <destination> <bandwidthHz>". VARA FM omits the
        // bandwidth field, so treat it as optional rather than requiring 4.
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            msg.type = MessageType::Connected;
            msg.source = parts.at(1);
            msg.destination = parts.at(2);
            if (parts.size() >= 4) {
                msg.bandwidthHz = parts.at(3).toInt();
            }
            return msg;
        }
        return msg;  // malformed — Unknown, raw retained
    }

    if (line.startsWith(QLatin1String("CQFRAME "))) {
        // "CQFRAME <source> <bandwidthHz>"
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            msg.type = MessageType::CqFrame;
            msg.source = parts.at(1);
            if (parts.size() >= 3) {
                msg.bandwidthHz = parts.at(2).toInt();
            }
            return msg;
        }
        return msg;
    }

    if (line.startsWith(QLatin1String("BUSY "))) {
        msg.type = MessageType::Busy;
        msg.flag = line.endsWith(QLatin1String("ON"));
        return msg;
    }

    if (line.startsWith(QLatin1String("PTT "))) {
        msg.type = MessageType::Ptt;
        msg.flag = line.endsWith(QLatin1String("ON"));
        return msg;
    }

    if (line.startsWith(QLatin1String("VERSION "))) {
        msg.type = MessageType::Version;
        msg.text = line.mid(8).trimmed();
        return msg;
    }

    if (line.startsWith(QLatin1String("REGISTERED "))) {
        msg.type = MessageType::Registered;
        msg.callsigns = line.mid(11).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        return msg;
    }

    // ── BITRATE reports the adaptive speed level and its throughput, e.g.
    //    "BITRATE (11)  7536 bps". This is the modem telling us which of the
    //    11 levels it settled on, which makes the level directly observable
    //    from the host interface rather than having to be inferred from the
    //    air. Whitespace between the level and the number varies.
    if (line.startsWith(QLatin1String("BITRATE"))) {
        // The trailing TX/RX token is optional — observed on v4.9.0, absent
        // from every published description, so treat it as an extra.
        static const QRegularExpression re(
            QStringLiteral(R"(BITRATE\s*\((\d+)\)\s+(\d+)\s*bps(?:\s+(TX|RX))?)"));
        const QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch()) {
            msg.type = MessageType::Bitrate;
            msg.speedLevel = m.captured(1).toInt();
            msg.bitsPerSecond = m.captured(2).toInt();
            const QString dir = m.captured(3);
            if (dir == QLatin1String("TX")) {
                msg.bitrateDirection = BitrateDirection::Transmit;
            } else if (dir == QLatin1String("RX")) {
                msg.bitrateDirection = BitrateDirection::Receive;
            }
            return msg;
        }
        return msg;
    }

    // CLEANTXBUFFER must be tested before BUFFER would ever be reached; it
    // does not actually collide (it starts "CLEAN"), but keep them adjacent so
    // a future edit does not introduce one.
    if (line.startsWith(QLatin1String("CLEANTXBUFFER "))) {
        msg.type = MessageType::CleanTxBuffer;
        const QString value = line.mid(14).trimmed();
        if (value == QLatin1String("BUFFEREMPTY")) {
            msg.cleanTx = CleanTxResult::BufferEmpty;
        } else if (value == QLatin1String("FAILED")) {
            msg.cleanTx = CleanTxResult::Failed;
        } else if (value == QLatin1String("OK")) {
            msg.cleanTx = CleanTxResult::Ok;
        }
        return msg;
    }

    if (line.startsWith(QLatin1String("BUFFER "))) {
        msg.type = MessageType::Buffer;
        msg.count = line.mid(7).trimmed().toInt();
        return msg;
    }

    if (line.startsWith(QLatin1String("LINK "))) {
        msg.type = MessageType::Link;
        const QString value = line.mid(5).trimmed();
        if (value == QLatin1String("REGISTERED")) {
            msg.link = LinkState::Registered;
        } else if (value == QLatin1String("UNREGISTERED")) {
            msg.link = LinkState::Unregistered;
        } else if (value == QLatin1String("ENCRYPTED")) {
            msg.link = LinkState::Encrypted;
        } else if (value == QLatin1String("UNENCRYPTED")) {
            msg.link = LinkState::Unencrypted;
        }
        return msg;
    }

    if (line.startsWith(QLatin1String("ENCRYPTION "))) {
        msg.type = MessageType::Encryption;
        const QString value = line.mid(11).trimmed();
        if (value == QLatin1String("DISABLED")) {
            msg.encryption = EncryptionState::Disabled;
        } else if (value == QLatin1String("READY")) {
            msg.encryption = EncryptionState::Ready;
        }
        return msg;
    }

    if (line.startsWith(QLatin1String("SN "))) {
        msg.type = MessageType::SignalToNoise;
        msg.value = line.mid(3).trimmed().toDouble();
        return msg;
    }

    if (line.startsWith(QLatin1String("TUNE "))) {
        msg.type = MessageType::Tune;
        msg.value = line.mid(5).trimmed().toDouble();
        return msg;
    }

    return msg;  // Unknown, with raw retained for logging
}

QByteArray cmdVersion()
{
    return command(QStringLiteral("VERSION"));
}

QByteArray cmdMyCall(const QStringList& callsigns)
{
    return command(QStringLiteral("MYCALL %1").arg(callsigns.join(QLatin1Char(' '))));
}

QByteArray cmdListen(bool on)
{
    return command(QStringLiteral("LISTEN %1").arg(onOff(on)));
}

QByteArray cmdListenCq()
{
    return command(QStringLiteral("LISTEN CQ"));
}

QByteArray cmdPublic(bool on)
{
    return on ? QByteArrayLiteral("PUBLIC ON\r") : QByteArrayLiteral("PUBLIC OFF\r");
}

QByteArray cmdConnect(const QString& source, const QString& destination)
{
    return command(QStringLiteral("CONNECT %1 %2").arg(source, destination));
}

QByteArray cmdDisconnect()
{
    return command(QStringLiteral("DISCONNECT"));
}

QByteArray cmdAbort()
{
    return command(QStringLiteral("ABORT"));
}

QByteArray cmdBandwidth(Bandwidth bandwidth)
{
    // Note the absent space: the command is "BW2300", not "BW 2300".
    return command(QStringLiteral("BW%1").arg(static_cast<int>(bandwidth)));
}

QByteArray cmdCqFrame(const QString& callsign, Bandwidth bandwidth)
{
    return command(
        QStringLiteral("CQFRAME %1 %2").arg(callsign).arg(static_cast<int>(bandwidth)));
}

QByteArray cmdCompression(Compression compression)
{
    QString value;
    switch (compression) {
    case Compression::Off:
        value = QStringLiteral("OFF");
        break;
    case Compression::Text:
        value = QStringLiteral("TEXT");
        break;
    case Compression::Files:
        value = QStringLiteral("FILES");
        break;
    }
    return command(QStringLiteral("COMPRESSION %1").arg(value));
}

QByteArray cmdChat(bool on)
{
    return command(QStringLiteral("CHAT %1").arg(onOff(on)));
}

QByteArray cmdCleanTxBuffer()
{
    return command(QStringLiteral("CLEANTXBUFFER"));
}

QString toString(MessageType type)
{
    switch (type) {
    case MessageType::Unknown:
        return QStringLiteral("Unknown");
    case MessageType::Ok:
        return QStringLiteral("Ok");
    case MessageType::Wrong:
        return QStringLiteral("Wrong");
    case MessageType::IAmAlive:
        return QStringLiteral("IAmAlive");
    case MessageType::Pending:
        return QStringLiteral("Pending");
    case MessageType::CancelPending:
        return QStringLiteral("CancelPending");
    case MessageType::Connected:
        return QStringLiteral("Connected");
    case MessageType::Disconnected:
        return QStringLiteral("Disconnected");
    case MessageType::Busy:
        return QStringLiteral("Busy");
    case MessageType::Ptt:
        return QStringLiteral("Ptt");
    case MessageType::Version:
        return QStringLiteral("Version");
    case MessageType::Registered:
        return QStringLiteral("Registered");
    case MessageType::SignalToNoise:
        return QStringLiteral("SignalToNoise");
    case MessageType::Tune:
        return QStringLiteral("Tune");
    case MessageType::Buffer:
        return QStringLiteral("Buffer");
    case MessageType::CleanTxBuffer:
        return QStringLiteral("CleanTxBuffer");
    case MessageType::Link:
        return QStringLiteral("Link");
    case MessageType::Bitrate:
        return QStringLiteral("Bitrate");
    case MessageType::Encryption:
        return QStringLiteral("Encryption");
    case MessageType::CqFrame:
        return QStringLiteral("CqFrame");
    case MessageType::MissingSoundcard:
        return QStringLiteral("MissingSoundcard");
    }
    return QStringLiteral("Unknown");
}

QString toString(Bandwidth bandwidth)
{
    switch (bandwidth) {
    case Bandwidth::Bw500:
        return QStringLiteral("500");
    case Bandwidth::Bw2300:
        return QStringLiteral("2300");
    case Bandwidth::Bw2750:
        return QStringLiteral("2750");
    case Bandwidth::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

}  // namespace AetherSDR::Vara
