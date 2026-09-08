#include "core/backends/anan/P1Client.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

#include <algorithm>
#include <span>

namespace AetherSDR::anan {

namespace {

std::span<const std::uint8_t> bytesOf(const QByteArray& data) noexcept
{
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        static_cast<std::size_t>(data.size()));
}

bool sendPacket(
    QUdpSocket& socket,
    const std::uint8_t* data,
    std::size_t size,
    const QHostAddress& host,
    quint16 port)
{
    const qint64 written = socket.writeDatagram(
        reinterpret_cast<const char*>(data),
        static_cast<qint64>(size),
        host,
        port);

    return written == static_cast<qint64>(size);
}

} // namespace

P1Client::P1Client(QObject* parent)
    : QObject(parent)
{
    //DH1KLM: EP2 is paced from a monotonic clock. It must not depend on EP6
    //DH1KLM: arrival because a receive stall must not stop the radio watchdog.
    m_ep2Timer = new QTimer(this);
    m_ep2Timer->setInterval(kEp2PacerTickMs);
    m_ep2Timer->setTimerType(Qt::PreciseTimer);
    connect(m_ep2Timer, &QTimer::timeout,
            this, &P1Client::onEp2PacerTick);

    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setInterval(kWatchdogTickMs);
    connect(m_watchdogTimer, &QTimer::timeout,
            this, &P1Client::onWatchdogTick);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (m_running && !m_linkUp) {
            emit connectFailed(QStringLiteral(
                "No Protocol 1 EP6 stream received within %1 ms")
                .arg(kConnectTimeoutMs));
        }
    });

    m_startRetryTimer = new QTimer(this);
    m_startRetryTimer->setInterval(kStartRetryMs);
    connect(m_startRetryTimer, &QTimer::timeout,
            this, &P1Client::onStartRetry);
}

P1Client::~P1Client()
{
    stop();
}

QList<P1Client::Discovered> P1Client::discover(
    int timeoutMs,
    const QHostAddress& broadcast,
    quint16 port)
{
    QList<Discovered> result;
    QUdpSocket socket;

    if (!socket.bind(QHostAddress::AnyIPv4, 0,
                     QUdpSocket::ShareAddress |
                     QUdpSocket::ReuseAddressHint)) {
        return result;
    }

    const auto request = p1::discoveryRequest();
    sendPacket(socket, request.data(), request.size(), broadcast, port);

    QList<QByteArray> seenMacs;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        const int remaining = std::max(
            1, timeoutMs - static_cast<int>(timer.elapsed()));

        if (!socket.waitForReadyRead(remaining))
            continue;

        while (socket.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = socket.receiveDatagram();
            const auto parsed = p1::parseDiscoveryReply(bytesOf(datagram.data()));
            if (!parsed)
                continue;

            const QByteArray mac(
                reinterpret_cast<const char*>(parsed->mac.data()),
                static_cast<qsizetype>(parsed->mac.size()));

            if (seenMacs.contains(mac))
                continue;

            seenMacs.append(mac);
            result.append(Discovered{*parsed, datagram.senderAddress()});
        }
    }

    return result;
}

int P1Client::effectiveReceiverCount(const Params& params) noexcept
{
    int count = std::clamp(params.receiverCount, 1, 7);
    if (params.boardMaxRx > 0)
        count = std::min(count, params.boardMaxRx);
    return count;
}

void P1Client::resetSessionState()
{
    m_sequence = 0;
    m_ep2Sent = 0;
    m_startAttempts = 0;
    m_linkUp = false;
    m_expectedRxSequence = 0;
    m_haveRxSequence = false;
    m_drops = 0;

    const int count = effectiveReceiverCount(m_params);
    m_rxFreqBanks.clear();
    m_rxFreqBanks.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        m_rxFreqBanks.push_back(
            p1::ccRxFrequency(i, m_params.rxFrequencyHz));
    }

    const std::uint8_t rateCode =
        m_params.sampleRate <= 48000 ? 0 :
        m_params.sampleRate <= 96000 ? 1 :
        m_params.sampleRate <= 192000 ? 2 : 3;

    m_configBank = p1::ccConfig(rateCode, count);
    m_txFreqBank = p1::ccTxFrequency(m_params.txFrequencyHz);
}

bool P1Client::start(const Params& params)
{
    if (m_running)
        stop();

    m_params = params;
    m_host = params.host;
    m_port = params.port;
    resetSessionState();

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0,
                       QUdpSocket::ShareAddress |
                       QUdpSocket::ReuseAddressHint)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }

    m_socket->setReadBufferSize(2 * 1024 * 1024);
    connect(m_socket, &QUdpSocket::readyRead,
            this, &P1Client::onReadyRead);

    m_running = true;
    m_ep2Clock.start();
    m_sinceLastEp6.start();

    //DH1KLM: The standard P1 startup sequence is START followed by regularly
    //DH1KLM: paced EP2 traffic. Priming C&C is sent before and after START so
    //DH1KLM: the FPGA receives valid NCO/configuration state during bring-up.
    sendPrimingBurst(2);
    sendStart();
    sendPrimingBurst(2);

    m_ep2Timer->start();
    m_watchdogTimer->start();
    m_connectTimer->start(kConnectTimeoutMs);

    m_startAttempts = 1;
    m_startRetryTimer->start(kStartRetryMs);
    return true;
}

void P1Client::stop()
{
    if (m_startRetryTimer)
        m_startRetryTimer->stop();
    if (m_ep2Timer)
        m_ep2Timer->stop();
    if (m_watchdogTimer)
        m_watchdogTimer->stop();
    if (m_connectTimer)
        m_connectTimer->stop();

    if (m_socket) {
        sendStop();
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    const bool wasUp = m_linkUp;
    m_running = false;
    m_linkUp = false;

    if (wasUp)
        emit linkDown();
}

void P1Client::sendStart()
{
    if (!m_socket)
        return;

    //DH1KLM: P1 Metis START is exactly EF FE 04 01. The watchdog policy is
    //DH1KLM: represented by the higher-level session handling, not by changing
    //DH1KLM: the documented four-byte start command itself.
    const auto packet = p1::metisCommand(0x01);
    sendPacket(*m_socket, packet.data(), packet.size(), m_host, m_port);
}

void P1Client::sendStop()
{
    if (!m_socket)
        return;

    const auto packet = p1::metisCommand(0x00);
    sendPacket(*m_socket, packet.data(), packet.size(), m_host, m_port);
}

void P1Client::sendPrimingBurst(int countPerBurst)
{
    for (int i = 0; i < countPerBurst; ++i)
        sendControlPacket();
}

void P1Client::onStartRetry()
{
    if (!m_running || !m_socket || m_linkUp)
        return;

    if (m_startAttempts >= kMaxStartAttempts) {
        m_startRetryTimer->stop();
        return;
    }

    ++m_startAttempts;
    sendStart();
}

void P1Client::onEp2PacerTick()
{
    if (!m_running || !m_socket)
        return;

    //DH1KLM: At 48 kHz, a 126-sample EP2 audio block represents 2625 us.
    //DH1KLM: The 2 ms timer is only a wakeup granularity; the monotonic elapsed
    //DH1KLM: time decides exactly how many packets are due.
    constexpr std::int64_t intervalUs =
        (static_cast<std::int64_t>(kEp2SamplesPerPacket) * 1'000'000)
        / kEp2AudioRateHz;

    const std::int64_t elapsedUs =
        m_ep2Clock.nsecsElapsed() / 1000;

    const std::uint64_t due =
        static_cast<std::uint64_t>(elapsedUs / intervalUs);

    int sentThisTick = 0;
    while (m_ep2Sent < due && sentThisTick < kMaxBurstPerTick) {
        sendControlPacket();
        ++m_ep2Sent;
        ++sentThisTick;
    }

    //DH1KLM: Drop excessive catch-up after a long application stall instead of
    //DH1KLM: sending a historical burst. Continuous watchdog service is the goal.
    if (m_ep2Sent + 32 < due)
        m_ep2Sent = due;
}

void P1Client::onWatchdogTick()
{
    if (!m_running || !m_linkUp)
        return;

    if (m_sinceLastEp6.elapsed() > kSilenceTimeoutMs) {
        m_linkUp = false;
        emit linkDown();
    }
}

void P1Client::onReadyRead()
{
    if (!m_socket)
        return;

    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();
        const auto bytes = bytesOf(datagram.data());
        const auto sequence = p1::ep6Sequence(bytes);
        if (!sequence)
            continue;

        if (m_haveRxSequence) {
            const std::uint32_t gap =
                *sequence - m_expectedRxSequence;
            if (gap != 0) {
                m_drops += gap;
                emit dropsUpdated(m_drops);
            }
        }

        m_expectedRxSequence = *sequence + 1;
        m_haveRxSequence = true;
        m_sinceLastEp6.restart();

        if (!m_linkUp) {
            m_linkUp = true;
            m_connectTimer->stop();
            m_startRetryTimer->stop();
            emit linkUp();
        }

        const int count = effectiveReceiverCount(m_params);
        std::vector<std::vector<std::complex<float>>> blocks(
            static_cast<std::size_t>(count));

        if (p1::decodeEp6SamplesMulti(
                bytes,
                std::span<std::vector<std::complex<float>>>(
                    blocks.data(), blocks.size())) > 0) {
            emit iqBlocksReady(blocks);
        }
    }
}

void P1Client::setRxFrequencyHz(std::uint32_t hz)
{
    setRxFrequencyHz(0, hz);
}

void P1Client::setRxFrequencyHz(
    int receiverIndex,
    std::uint32_t hz)
{
    if (receiverIndex < 0 ||
        receiverIndex >= static_cast<int>(m_rxFreqBanks.size()))
        return;

    m_rxFreqBanks[static_cast<std::size_t>(receiverIndex)] =
        p1::ccRxFrequency(receiverIndex, hz);
}

void P1Client::setTxFrequencyHz(std::uint32_t hz)
{
    m_txFreqBank = p1::ccTxFrequency(hz);
}

void P1Client::setSampleRate(int sampleRate)
{
    if (sampleRate != 48000 && sampleRate != 96000 &&
        sampleRate != 192000 && sampleRate != 384000)
        return;

    m_params.sampleRate = sampleRate;

    const std::uint8_t rateCode =
        sampleRate <= 48000 ? 0 :
        sampleRate <= 96000 ? 1 :
        sampleRate <= 192000 ? 2 : 3;

    m_configBank = p1::ccConfig(
        rateCode,
        effectiveReceiverCount(m_params));
}

void P1Client::setReceiverCount(int count)
{
    m_params.receiverCount =
        std::clamp(count, 1, std::max(1, m_params.boardMaxRx));

    //DH1KLM: Receiver count changes the EP6 payload layout. Stage 1 therefore
    //DH1KLM: rebuilds the C&C state and leaves stream restart/wiring to the next
    //DH1KLM: backend integration step rather than silently changing layouts live.
    resetSessionState();
}

std::array<std::uint8_t, p1::kUsbPacketSize>
P1Client::buildNextControlPacket()
{
    p1::Cc second = m_txFreqBank;
    if (!m_rxFreqBanks.empty()) {
        const std::size_t index =
            static_cast<std::size_t>(m_sequence) % m_rxFreqBanks.size();
        second = m_rxFreqBanks[index];
    }

    return p1::ep2ControlPacket(
        m_sequence++,
        m_configBank,
        second);
}

void P1Client::sendControlPacket()
{
    if (!m_socket)
        return;

    const auto packet = buildNextControlPacket();
    sendPacket(*m_socket, packet.data(), packet.size(), m_host, m_port);
}

} // namespace AetherSDR::anan
