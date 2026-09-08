#include "core/backends/anan/AnanDiscovery.h"

#include "core/AppSettings.h"
#include "core/backends/anan/P2Protocol.h"

#include <QJsonObject>
#include <QNetworkDatagram>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR::anan {
namespace {

constexpr char kIdentityFeature[] = "Identity";
constexpr char kNicknameField[] = "nickname";

void enableBroadcast(QUdpSocket& s) noexcept
{
    const qintptr fd = s.socketDescriptor();
    if (fd < 0)
        return;
    const int on = 1;
#ifdef Q_OS_WIN
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
#else
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
}

} // namespace

AnanDiscovery::AnanDiscovery(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AnanDiscovery::onSweepTimer);
}

AnanDiscovery::~AnanDiscovery() = default;

bool AnanDiscovery::isRunning() const noexcept
{
    return m_timer && m_timer->isActive();
}

void AnanDiscovery::start(int intervalMs)
{
    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress::AnyIPv4, 0,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            m_socket->deleteLater();
            m_socket = nullptr;
            return;
        }
        enableBroadcast(*m_socket);
        connect(m_socket, &QUdpSocket::readyRead,
                this, &AnanDiscovery::onReadyRead);
    }

    m_timer->start(intervalMs);
    sweepNow();
}

void AnanDiscovery::stop()
{
    if (m_timer)
        m_timer->stop();
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_seen.clear();
}

void AnanDiscovery::sweepNow()
{
    if (!m_socket)
        return;

    //DH1KLM: Protocol 1 discovery is 63 bytes: EF FE 02 followed by zero padding.
    //DH1KLM: Do NOT put 0xFF into byte 3/4 here; Thetis/NereusSDR's proven request
    //DH1KLM: is EF FE 02 + 60 zero bytes.
    QByteArray p1(63, '\0');
    p1[0] = char(0xEF);
    p1[1] = char(0xFE);
    p1[2] = char(0x02);
    m_socket->writeDatagram(p1, QHostAddress::Broadcast, 1024);

    //DH1KLM: Preserve the existing Protocol 2 discovery packet byte-for-byte.
    const auto p2 = buildDiscovery();
    m_socket->writeDatagram(reinterpret_cast<const char*>(p2.data()),
                            static_cast<qint64>(p2.size()),
                            QHostAddress::Broadcast, kRadioPort);
}

void AnanDiscovery::onSweepTimer()
{
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (++it.value().missedSweeps > kMissedSweepsBeforeLost) {
            const QString serial = it.key();
            it = m_seen.erase(it);
            emit radioLost(serial);
        } else {
            ++it;
        }
    }
    sweepNow();
}

bool AnanDiscovery::parseP1Reply(const QByteArray& data, P1Reply& out) noexcept
{
    //DH1KLM: The standard P1 reply is at least 21 bytes. The byte layout below is
    //DH1KLM: taken from Thetis clsRadioDiscovery.cs and the NereusSDR P1 capture
    //DH1KLM: reference; no guessed fields are used.
    if (data.size() < 21)
        return false;

    const auto u8 = [&data](int i) -> std::uint8_t {
        return static_cast<std::uint8_t>(data.at(i));
    };

    if (u8(0) != 0xEF || u8(1) != 0xFE)
        return false;
    if (u8(2) != 0x02 && u8(2) != 0x03)
        return false;

    out.busy = (u8(2) == 0x03);
    for (int i = 0; i < 6; ++i)
        out.mac[static_cast<std::size_t>(i)] = u8(3 + i);
    out.firmware = u8(9);
    out.boardId = u8(10);
    out.metisVersion = u8(19);
    out.numRxs = u8(20);
    return true;
}

void AnanDiscovery::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const QByteArray data = dg.data();

        //DH1KLM: P1 and P2 share UDP/1024. The EF FE prefix is the authoritative
        //DH1KLM: discriminator, not packet length or source port.
        if (data.size() >= 3
            && static_cast<std::uint8_t>(data.at(0)) == 0xEF
            && static_cast<std::uint8_t>(data.at(1)) == 0xFE) {
            processP1Reply(data, dg.senderAddress());
        } else {
            processP2Reply(data, dg.senderAddress());
        }
    }
}

void AnanDiscovery::processP1Reply(const QByteArray& data,
                                    const QHostAddress& sender)
{
    P1Reply reply;
    if (!parseP1Reply(data, reply))
        return;

    const QString model = p1ModelName(reply.boardId);

    //DH1KLM: Stage 1 deliberately exposes only Orion/ANAN-200D. Other P1 boards
    //DH1KLM: are enabled after the ANAN capability registry is completed.
    if (model.isEmpty())
        return;

    RadioInfo info;
    info.family = QStringLiteral("anan");
    info.address = sender;
    info.port = 1024;
    info.model = model;
    info.name = model;
    info.serial = macToSerial(reply.mac);
    info.nickname = effectiveNickname(QStringLiteral("anan"), info.serial, model);
    info.version = QString::number(reply.firmware);
    info.versionLabel = QStringLiteral("P1 Gateware");
    info.inUse = reply.busy;
    info.status = reply.busy ? QStringLiteral("In_Use") : QStringLiteral("Available");

    upsert(info, Protocol::P1);
}

void AnanDiscovery::processP2Reply(const QByteArray& data,
                                    const QHostAddress& sender)
{
    const auto reply = parseDiscoveryReply(
        {reinterpret_cast<const std::uint8_t*>(data.constData()),
         static_cast<std::size_t>(data.size())});
    if (!reply || !reply->isSaturn())
        return;

    RadioInfo info;
    info.family = QStringLiteral("anan");
    info.address = sender;
    info.port = kRadioPort;
    info.model = QStringLiteral("ANAN-G2");
    info.name = info.model;
    info.serial = macToSerial(reply->mac);
    info.nickname = effectiveNickname(QStringLiteral("anan"), info.serial, info.model);
    info.version = QString::number(reply->firmwareVer);
    info.versionLabel = QStringLiteral("P2 Gateware");
    info.inUse = reply->streaming;
    info.status = reply->streaming ? QStringLiteral("In_Use") : QStringLiteral("Available");

    upsert(info, Protocol::P2);
}

void AnanDiscovery::upsert(const RadioInfo& info, Protocol protocol)
{
    auto it = m_seen.find(info.serial);
    if (it == m_seen.end()) {
        m_seen.insert(info.serial, Seen{info, protocol, 0});
        emit protocolDetected(info.serial, static_cast<int>(protocol));
        emit radioDiscovered(info);
        if (protocol == Protocol::P1)
            emit p1RadioReady(info);
        return;
    }

    const Protocol oldProtocol = it.value().protocol;
    const RadioInfo previous = it.value().info;
    const bool changed = previous.address != info.address
                      || previous.port != info.port
                      || previous.model != info.model
                      || previous.status != info.status
                      || previous.version != info.version
                      || previous.versionLabel != info.versionLabel
                      || oldProtocol != protocol;

    it.value().info = info;
    it.value().protocol = protocol;
    it.value().missedSweeps = 0;

    if (oldProtocol != protocol)
        emit protocolDetected(info.serial, static_cast<int>(protocol));
    if (changed)
        emit radioUpdated(info);
    if (protocol == Protocol::P1 && (oldProtocol != protocol || changed))
        emit p1RadioReady(info);
}

QString AnanDiscovery::p1ModelName(std::uint8_t boardId)
{
    //DH1KLM: Protocol 1 board ID 5 is Orion. For Stage 1 the physical Orion
    //DH1KLM: implementation is exposed as the ANAN-200D picker model.
    if (boardId == 5)
        return QStringLiteral("ANAN-200D");
    return {};
}

AnanDiscovery::Protocol AnanDiscovery::protocolForSerial(const QString& serial) const noexcept
{
    const auto it = m_seen.constFind(serial);
    return it == m_seen.cend() ? Protocol::Unknown : it.value().protocol;
}

AnanDiscovery::Protocol AnanDiscovery::protocolForModel(const QString& model) noexcept
{
    if (model.compare(QStringLiteral("ANAN-200D"), Qt::CaseInsensitive) == 0)
        return Protocol::P1;
    if (model.compare(QStringLiteral("ANAN-G2"), Qt::CaseInsensitive) == 0)
        return Protocol::P2;
    return Protocol::Unknown;
}

QString AnanDiscovery::macToSerial(const std::array<std::uint8_t, 6>& mac)
{
    QStringList parts;
    parts.reserve(6);
    for (const auto b : mac)
        parts << QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

QString AnanDiscovery::effectiveNickname(const QString& family,
                                         const QString& serial,
                                         const QString& fallback)
{
    auto& settings = AppSettings::instance();
    const QString custom = settings.radioFeature(
        family, serial, QString::fromLatin1(kIdentityFeature))
        .value(QLatin1String(kNicknameField)).toString().trimmed();
    return custom.isEmpty() ? fallback : custom;
}

void AnanDiscovery::setNickname(const QString& family,
                                const QString& serial,
                                const QString& name)
{
    auto& settings = AppSettings::instance();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        settings.removeRadioFeature(family, serial,
                                     QString::fromLatin1(kIdentityFeature));
    } else {
        settings.setRadioFeature(
            family, serial, QString::fromLatin1(kIdentityFeature), 1,
            QJsonObject{{QLatin1String(kNicknameField), trimmed}});
    }
    settings.save();
}

} // namespace AetherSDR::anan
