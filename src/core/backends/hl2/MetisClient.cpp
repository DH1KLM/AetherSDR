#include "core/backends/hl2/MetisClient.h"

#include <QElapsedTimer>
#include <QThread>
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

MetisClient::MetisClient(QObject* parent) : QObject(parent) {
    // Paces EP2 from a wall clock (see kEp2PacerTickMs) so C&C keeps flowing
    // even if the EP6 receive path stalls.
    m_ep2Timer = new QTimer(this);
    m_ep2Timer->setInterval(kEp2PacerTickMs);
    m_ep2Timer->setTimerType(Qt::PreciseTimer);
    connect(m_ep2Timer, &QTimer::timeout, this, &MetisClient::onEp2PacerTick);

    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setInterval(kWatchdogTickMs);
    connect(m_watchdogTimer, &QTimer::timeout, this, &MetisClient::onWatchdogTick);

    m_connectWatchdog = new QTimer(this);
    m_connectWatchdog->setSingleShot(true);
    connect(m_connectWatchdog, &QTimer::timeout, this, [this] {
        if (!m_linkUp)
            emit connectFailed(QStringLiteral(
                "no IQ stream from the radio within %1 ms of start")
                    .arg(kConnectTimeoutMs));
    });
}

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

    // Order matters. Prime with real C&C frames FIRST so the DDC latches the
    // sample rate, NCO and receiver count, then start the stream, then prime
    // again so nothing is lost to the start transition. Starting before any C&C
    // has landed makes the firmware stream ADC-idle samples (Q pinned to zero).
    m_running = true;
    sendPrimingBurst(3);
    sendTo(*m_socket, metisStart(m_watchdogEnabled), m_host, m_port);
    sendPrimingBurst(3);

    m_ep2IntervalUs = (sampleRateHz(m_params.sampleRate) > 0)
        ? (static_cast<qint64>(kSamplesPerPacket) * 1'000'000
           / sampleRateHz(m_params.sampleRate))
        : 2625;
    m_ep2Sent = 0;
    m_ep2Clock.restart();
    m_ep2Timer->start();
    m_sinceLastEp6.restart();
    m_watchdogTimer->start();
    m_connectWatchdog->start(kConnectTimeoutMs);
    return true;
}

void MetisClient::sendPrimingBurst(int countPerBank)
{
    for (int i = 0; i < countPerBank; ++i)
        sendControlPacket();
    QThread::msleep(10);
    for (int i = 0; i < countPerBank; ++i)
        sendControlPacket();
    QThread::msleep(10);
}

void MetisClient::onEp2PacerTick()
{
    if (!m_running || !m_socket || m_ep2IntervalUs <= 0)
        return;
    // Catch-up: emit however many frames the wall clock says are due, capped so
    // a long stall cannot produce an unbounded burst.
    const qint64 elapsedUs = m_ep2Clock.nsecsElapsed() / 1000;
    const quint64 due = static_cast<quint64>(elapsedUs / m_ep2IntervalUs);
    int burst = 0;
    while (m_ep2Sent < due && burst < kEp2MaxBurstPerTick) {
        sendControlPacket();
        ++m_ep2Sent;
        ++burst;
    }
}

void MetisClient::onWatchdogTick()
{
    if (!m_running || !m_linkUp)
        return;
    if (m_sinceLastEp6.isValid() && m_sinceLastEp6.elapsed() > kSilenceTimeoutMs) {
        // Socket still open but the radio went quiet — surface it as link loss
        // rather than sitting in a permanently "connected" state.
        m_linkUp = false;
        emit linkDown();
    }
}

void MetisClient::stop()
{
    if (m_ep2Timer)        m_ep2Timer->stop();
    if (m_watchdogTimer)   m_watchdogTimer->stop();
    if (m_connectWatchdog) m_connectWatchdog->stop();
    if (m_socket) {
        sendTo(*m_socket, metisStop(m_watchdogEnabled), m_host, m_port);
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

        m_sinceLastEp6.restart();
        if (!m_linkUp) {
            m_linkUp = true;
            if (m_connectWatchdog)
                m_connectWatchdog->stop();   // first EP6 — the link is alive
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

    }
}

}  // namespace AetherSDR::hl2
