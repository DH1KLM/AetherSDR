#include "core/backends/hl2/MetisClient.h"

#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtGlobal>

#include <algorithm>
#include <span>

#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR::hl2 {

namespace {

// View a QByteArray as a byte span for the protocol decoders.
std::span<const std::uint8_t> asBytes(const QByteArray& d) noexcept
{
    return {reinterpret_cast<const std::uint8_t*>(d.constData()), static_cast<std::size_t>(d.size())};
}

// Send a fixed-size wire buffer.
template <std::size_t N>
void sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
            const QHostAddress& host, quint16 port)
{
    s.writeDatagram(reinterpret_cast<const char*>(buf.data()), static_cast<qint64>(N), host, port);
}

// QUdpSocket does not enable SO_BROADCAST on its own; set it on the native
// descriptor so discovery datagrams reach the subnet broadcast address.
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

}  // namespace

MetisClient::MetisClient(QObject* parent) : QObject(parent) {}

MetisClient::~MetisClient()
{
    stop();
}

QList<MetisClient::Discovered> MetisClient::discover(int timeoutMs, const QHostAddress& broadcast,
                                                     quint16 port)
{
    QList<Discovered> found;
    QUdpSocket sock;
    if (!sock.bind(QHostAddress::AnyIPv4, 0))
        return found;
    enableBroadcast(sock);

    const auto req = discoveryRequest();
    sock.writeDatagram(reinterpret_cast<const char*>(req.data()), static_cast<qint64>(req.size()),
                       broadcast, port);

    QList<QByteArray> seenMacs;   // dedup by MAC
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
        if (!sock.waitForReadyRead(remaining))
            continue;
        while (sock.hasPendingDatagrams()) {
            const QNetworkDatagram dg = sock.receiveDatagram();
            const auto reply = parseDiscoveryReply(asBytes(dg.data()));
            if (!reply)
                continue;
            const QByteArray mac(reinterpret_cast<const char*>(reply->mac.data()),
                                 static_cast<qsizetype>(reply->mac.size()));
            if (seenMacs.contains(mac))
                continue;
            seenMacs.append(mac);
            found.append(Discovered{*reply, dg.senderAddress()});
        }
    }
    return found;
}

bool MetisClient::start(const Params& params)
{
    if (m_running)
        stop();

    m_params = params;
    m_host = params.host;
    m_port = params.port;
    m_ccConfig = ccConfig(m_params.sampleRate, 1);
    m_ccGain = ccRxGain(m_params.lnaGainDb);
    m_ccFreq = ccRx1Freq(m_params.rxFrequencyHz);
    m_txSeq = 0;
    m_roundRobin = 0;
    m_haveRxSeq = false;
    m_drops = 0;
    m_linkUp = false;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    m_socket->setReadBufferSize(1 << 21);   // absorb the continuous EP6 torrent
    connect(m_socket, &QUdpSocket::readyRead, this, &MetisClient::onReadyRead);

    sendTo(*m_socket, metisStart(), m_host, m_port);   // start IQ
    for (int i = 0; i < 4; ++i)                        // prime + latch registers
        sendControlPacket();

    m_running = true;
    return true;
}

void MetisClient::stop()
{
    if (m_socket) {
        sendTo(*m_socket, metisStop(), m_host, m_port);
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_running = false;
    if (m_linkUp) {
        m_linkUp = false;
        emit linkDown();
    }
}

void MetisClient::setRxFrequencyHz(std::uint32_t hz)
{
    m_params.rxFrequencyHz = hz;
    m_ccFreq = ccRx1Freq(hz);
}

void MetisClient::setSampleRate(SampleRate rate)
{
    m_params.sampleRate = rate;
    m_ccConfig = ccConfig(rate, 1);
}

void MetisClient::setLnaGainDb(int db)
{
    m_params.lnaGainDb = db;
    m_ccGain = ccRxGain(db);
}

void MetisClient::sendControlPacket()
{
    if (!m_socket)
        return;
    const Cc* regs[3] = {&m_ccConfig, &m_ccGain, &m_ccFreq};
    const Cc& a = *regs[m_roundRobin % 3];
    const Cc& b = *regs[(m_roundRobin + 1) % 3];
    ++m_roundRobin;
    sendTo(*m_socket, ep2Packet(m_txSeq++, a, b), m_host, m_port);
}

void MetisClient::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const auto bytes = asBytes(dg.data());

        const auto seq = ep6Seq(bytes);
        if (!seq)
            continue;   // not an EP6 packet (e.g. a stray discovery reply)

        if (!m_linkUp) {
            m_linkUp = true;
            emit linkUp();
        }
        if (m_haveRxSeq && *seq != m_expectedRxSeq) {
            const std::uint32_t gap = *seq - m_expectedRxSeq;   // unsigned wrap
            if (gap < 0x80000000u) {                            // forward gap = real loss
                m_drops += gap;
                emit dropsUpdated(m_drops);
            }
        }
        m_expectedRxSeq = *seq + 1;
        m_haveRxSeq = true;

        m_block.clear();
        if (ep6Samples(bytes, m_block) > 0)
            emit iqBlockReady(m_block);

        sendControlPacket();   // pace the C&C 1:1 with the EP6 stream
    }
}

}  // namespace AetherSDR::hl2
