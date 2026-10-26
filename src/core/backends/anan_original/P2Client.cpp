#include "core/backends/anan_original/P2Client.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

Q_LOGGING_CATEGORY(lcOriginalAnanP2, "aether.anan.p2")

namespace AetherSDR::anan_original {

namespace {

//DH1KLM View a QByteArray as a byte span for the protocol decoders. Same helper
//DH1KLM MetisClient.cpp defines locally for the same purpose.
std::span<const std::uint8_t> asBytes(const QByteArray& d) noexcept
{
    return {reinterpret_cast<const std::uint8_t*>(d.constData()),
           static_cast<std::size_t>(d.size())};
}

//DH1KLM txAccum is REQUIRED, not defaulted: a new send site cannot compile without
//DH1KLM deciding where its bytes are counted, which is what keeps the LinkStats tx
//DH1KLM total from silently under-reporting as this client grows.
template <std::size_t N>
qint64 sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
             const QHostAddress& host, quint16 port, quint64& txAccum)
{
    const qint64 sent = s.writeDatagram(reinterpret_cast<const char*>(buf.data()),
                                        static_cast<qint64>(N), host, port);
    //DH1KLM Only what the socket ACCEPTED. Counting a refused datagram would inflate
    //DH1KLM the readout exactly when the link is failing, which is when an operator
    //DH1KLM is most likely to be reading it.
    if (sent > 0)
        txAccum += static_cast<quint64>(sent);
    return sent;
}

}  // namespace

P2Client::P2Client(QObject* parent) : QObject(parent)
{
    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(kKeepaliveMs);
    connect(m_keepaliveTimer, &QTimer::timeout, this, &P2Client::onKeepaliveTick);

    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &P2Client::onConnectTimeout);

    m_speakerDrainTimer = new QTimer(this);
    m_speakerDrainTimer->setInterval(kSpeakerDrainMs);
    //DH1KLM A realtime stream's pacing timer, so ask for the accurate one. Qt's default
    //DH1KLM CoarseTimer may be adjusted by up to 5% of the interval, which at 5 ms is
    //DH1KLM the same order as the interval itself; the pacer's target absorbs jitter but
    //DH1KLM there is no reason to hand it more than necessary.
    m_speakerDrainTimer->setTimerType(Qt::PreciseTimer);
    connect(m_speakerDrainTimer, &QTimer::timeout, this, &P2Client::onSpeakerDrainTick);

    m_linkCountersTimer = new QTimer(this);
    m_linkCountersTimer->setInterval(kLinkCountersMs);
    connect(m_linkCountersTimer, &QTimer::timeout, this, &P2Client::publishLinkCounters);
}

P2Client::~P2Client()
{
    stop();
}

bool P2Client::start(const Params& params, int connectTimeoutMs)
{
    if (m_running)
        stop();

    m_host = QHostAddress(params.host);
    if (m_host.isNull())
        return false;   // not a parseable IPv4/IPv6 literal -- fail fast, no hostname lookup here

    m_ddc0FreqWord = 0;
    m_bypassAdc0Filters = params.bypassAdc0Filters;
    m_bypassAdc1Filters = params.bypassAdc1Filters;
    m_adc0AttenuationDb = params.adc0AttenuationDb;
    m_adc1AttenuationDb = params.adc1AttenuationDb;
    m_speakerAudioEnabled = params.speakerAudioEnabled;
    m_bandFiltersEnabled = params.bandFiltersEnabled;
    m_speakerAmpMuted = params.speakerAmpMuted;
    m_ddc0FreqHz = 0.0;   // cleared with m_ddc0FreqWord: nothing is tuned yet
    m_speakerPending.clear();
    m_speakerPacer.reset();
    //DH1KLM Sequence restarts per session, like the DDC expectations below: p2app
    //DH1KLM reads it only to spot gaps within one stream.
    m_speakerSequence = 0;
    m_speakerOverflowLogged = false;
    m_speakerUnderflowReports = 0;
    m_lastSpeakerFifoLevel = 0;
    //DH1KLM Every DDC's sequence tracker, not just DDC0's -- a stale expectation
    //DH1KLM carried across a restart would report a phantom gap on the new
    //DH1KLM session's first frame.
    m_expectedSeq.fill(std::nullopt);
    m_activeDdcCount = 1;   // real value set once the DDC list is resolved below
    m_drops = 0;
    m_rxBytes = 0;
    m_rxPackets = 0;
    m_txBytes = 0;
    m_warnedUnexpectedPorts.clear();
    m_warnedForeignSender = false;
    m_linkUp = false;
    m_discoveryInfoSent = false;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &P2Client::onReadyRead);

    m_running = true;

    //DH1KLM No artificial delay between these -- MetisClient::start() sends its
    //DH1KLM own startup sequence back-to-back too, and nothing in the spike's
    //DH1KLM proven session flow needed one either.
    //DH1KLM
    //DH1KLM Discovery FIRST, from this socket -- see the class comment for why:
    //DH1KLM it is the source port of THIS packet that decides where the radio
    //DH1KLM sends DDC0 IQ, not the source port of General/DDC-Specific/High-
    //DH1KLM Priority below. The reply (if any arrives here) is not parsed;
    //DH1KLM onReadyRead() already drops anything that isn't DDC0-shaped.
    sendTo(*m_socket, buildDiscovery(), m_host, kRadioPort, m_txBytes);
    sendTo(*m_socket, buildGeneral(), m_host, kRadioPort, m_txBytes);

    //DH1KLM Resolve the session's DDC list ONCE: either the caller's explicit
    //DH1KLM multi-DDC list, or a one-element list from the DDC0 shorthand. See
    //DH1KLM Params::activeDdcs for why these are not merged.
    std::vector<DdcConfig> ddcs = params.activeDdcs;
    if (ddcs.empty())
        ddcs.push_back(DdcConfig{params.ddc0RateKsps, params.ddc0AdcIndex});
    if (static_cast<int>(ddcs.size()) > kMaxDdcs)
        ddcs.resize(kMaxDdcs);
    m_activeDdcCount = static_cast<int>(ddcs.size());
    //DH1KLM Retained for setDdcRateLive() -- see its own comment for why the whole
    //DH1KLM list (not just the count) has to survive start().
    m_activeDdcs = ddcs;
    m_ditherEnabled = params.ditherEnabled;
    m_randomEnabled = params.randomEnabled;

    //DH1KLM Destination ports below are NOT interchangeable with kRadioPort -- see
    //DH1KLM kDdcSpecificPort/kHighPriorityPort's comment. p2app tells these two
    //DH1KLM packet types apart by which port they arrive on.
    sendTo(*m_socket,
          buildDdcSpecific(ddcs, /*numAdcs=*/2,
                           params.ditherEnabled, params.randomEnabled),
          m_host, kDdcSpecificPort, m_txBytes);
    //DH1KLM Every DDC gets the shared m_ddc0FreqWord (sharedFreqWords()): there is no
    //DH1KLM per-DDC tuning setter yet. All run=1 senders build the packet the same
    //DH1KLM way, so they cannot disagree (#5547).
    sendRunningHighPriority();

    m_keepaliveTimer->start();
    m_linkCountersTimer->start();
    if (m_speakerAudioEnabled) {
        //DH1KLM Started from zero at the same moment as the drain, so the pacer's
        //DH1KLM first advance() measures from the first tick rather than from
        //DH1KLM whenever this object happened to be constructed.
        m_speakerClock.restart();
        m_speakerDrainTimer->start();
    }
    m_activeConnectTimeoutMs = connectTimeoutMs;
    m_connectTimeoutTimer->start(connectTimeoutMs);
    return true;
}

void P2Client::stop()
{
    m_keepaliveTimer->stop();
    m_linkCountersTimer->stop();
    m_connectTimeoutTimer->stop();
    m_speakerDrainTimer->stop();
    //DH1KLM Dropped, not drained. These samples are older than the disconnect and the
    //DH1KLM radio's own FIFO does not survive it either, so playing them out on the
    //DH1KLM next session would start it with a fragment of the last one.
    m_speakerPending.clear();
    m_speakerPacer.reset();
    m_speakerSequence = 0;
    m_speakerOverflowLogged = false;
    m_speakerUnderflowReports = 0;
    m_lastSpeakerFifoLevel = 0;
    if (m_socket) {
        //DH1KLM The clean-stop packet, mirroring metisStop(): run=0 reaches the
        //DH1KLM radio before the socket that would carry any further keepalive
        //DH1KLM goes away. Without highPriorityOptions(): p2app keeps the last
        //DH1KLM byte 1400 and Alex words it was sent, and no client is left to
        //DH1KLM change them, so a clean stop hands the radio back unmuted, with the
        //DH1KLM bypass flags, not a band filter, deciding both banks.
        sendTo(*m_socket,
              buildHighPriority(false, 0, m_bypassAdc0Filters, m_bypassAdc1Filters,
                                m_adc0AttenuationDb, m_adc1AttenuationDb),
              m_host, kHighPriorityPort, m_txBytes);
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

void P2Client::publishLinkCounters()
{
    LinkCounters c;
    c.rxBytes = m_rxBytes;
    c.rxPackets = m_rxPackets;
    c.txBytes = m_txBytes;
    c.drops = m_drops;
    c.localEndpoint = localEndpoint();
    emit linkCountersUpdated(c);
}

QString P2Client::localEndpoint() const
{
    //DH1KLM Empty rather than a partial string when there is no socket or the bind
    //DH1KLM did not take: LinkStats::localEndpoint is documented as empty when not
    //DH1KLM bound, and the consumer falls back to its own source on empty.
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return {};
    const quint16 port = m_socket->localPort();
    if (port == 0)
        return {};
    //DH1KLM AnyIPv4 prints as "0.0.0.0", which is the honest answer for a wildcard
    //DH1KLM bind -- the socket genuinely is not pinned to one interface. The PORT is
    //DH1KLM the load-bearing half anyway: it is what the radio streams DDC0 to.
    return QStringLiteral("%1:%2").arg(m_socket->localAddress().toString())
                                  .arg(port);
}

void P2Client::noteSpeakerFifoStatus(const HighPriorityStatus& status)
{
    m_lastSpeakerFifoLevel = status.speakerFifoLevel;
    //DH1KLM EVERY status packet, healthy or not, at debug. The underflow bit only tells
    //DH1KLM us the stream ALREADY failed; the level is what shows it coming, and a bench
    //DH1KLM run needs to watch it hold steady rather than only learn afterwards that it
    //DH1KLM did not. Five lines a second, so debug-gated:
    //DH1KLM   QT_LOGGING_RULES="aether.anan.p2.debug=true"
    //DH1KLM
    //DH1KLM Logged even while the stream is disabled: then it is the radio's own idle
    //DH1KLM level, which is the baseline a later run is read against.
    if (m_speakerAudioEnabled) {
        qCDebug(lcOriginalAnanP2).nospace()
            << "ANAN: speaker FIFO level " << status.speakerFifoLevel
            << " (FIFO locations), underflow reports so far "
            << m_speakerUnderflowReports << ", queued "
            << (m_speakerPending.size() / (kSpeakerFramesPerPacket * kSpeakerChannels))
            << " packets" << (m_speakerPacer.catchingUp() ? ", CATCHING UP" : "");
    } else {
        qCDebug(lcOriginalAnanP2).nospace()
            << "ANAN: speaker FIFO level " << status.speakerFifoLevel
            << " (FIFO locations), stream disabled -- this is the radio's idle level";
    }
    if (!status.speakerUnderflow) {
        return;
    }
    ++m_speakerUnderflowReports;
    //DH1KLM The first underflow warns; later ones are only counted, since a starved
    //DH1KLM stream sets the latched bit on every ~200 ms status packet. p2app reports
    //DH1KLM underflow even when nobody feeds the FIFO, so it only means something while
    //DH1KLM we are sending.
    if (!m_speakerAudioEnabled) {
        return;
    }
    if (m_speakerUnderflowReports == 1) {
        qCWarning(lcOriginalAnanP2).nospace()
            << "ANAN: the radio's speaker FIFO underflowed -- audio to the radio "
               "is arriving too slowly and will click or stutter. FIFO level "
            << status.speakerFifoLevel
            << " (FIFO locations, not samples). Further reports are counted, not "
               "logged; read speakerUnderflowReports().";
    } else {
        qCDebug(lcOriginalAnanP2).nospace()
            << "ANAN: speaker FIFO underflow report " << m_speakerUnderflowReports
            << ", level " << status.speakerFifoLevel;
    }
}

void P2Client::enqueueSpeakerAudio(const QByteArray& interleavedInt16)
{
    if (!m_speakerAudioEnabled || !m_running || interleavedInt16.isEmpty()) {
        return;
    }
    const auto count = static_cast<std::size_t>(interleavedInt16.size())
        / sizeof(std::int16_t);
    const auto* samples = reinterpret_cast<const std::int16_t*>(interleavedInt16.constData());

    constexpr std::size_t kPacketSamples =
        kSpeakerFramesPerPacket * kSpeakerChannels;
    const std::size_t cap = kMaxQueuedSpeakerPackets * kPacketSamples;
    if (m_speakerPending.size() + count > cap) {
        //DH1KLM DROP THE OLDEST, not the newest. This is a realtime stream: the newest
        //DH1KLM audio is the audio the operator is listening for, and discarding it to
        //DH1KLM preserve a backlog they have already missed hearing keeps the latency
        //DH1KLM and loses the content. Dropping from the front loses the same amount of
        //DH1KLM audio and recovers the latency with it.
        const std::size_t overflow = m_speakerPending.size() + count - cap;
        const std::size_t drop = std::min(overflow, m_speakerPending.size());
        m_speakerPending.erase(m_speakerPending.begin(),
                               m_speakerPending.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!m_speakerOverflowLogged) {
            m_speakerOverflowLogged = true;
            qCWarning(lcOriginalAnanP2) << "ANAN: speaker audio queue overflowed, dropping oldest"
                              << drop << "samples -- audio to the radio will have a gap."
                              << "Logged once per session.";
        }
    }
    m_speakerPending.insert(m_speakerPending.end(), samples, samples + count);
}

void P2Client::onSpeakerDrainTick()
{
    if (!m_speakerAudioEnabled || !m_running || !m_socket) {
        return;
    }
    constexpr std::size_t kPacketSamples =
        kSpeakerFramesPerPacket * kSpeakerChannels;

    //DH1KLM Elapsed first: the pacer's whole estimate is "sent minus drained", and
    //DH1KLM draining is what happened between ticks.
    const qint64 elapsedNs = m_speakerClock.nsecsElapsed();
    m_speakerClock.restart();
    m_speakerPacer.advance(static_cast<double>(elapsedNs) * 1.0e-9);

    const int queued = static_cast<int>(m_speakerPending.size() / kPacketSamples);
    //DH1KLM Tell the pacer how far behind WE are before asking what may go. Without
    //DH1KLM this it only ever knows about the radio's side, which is correct for the
    //DH1KLM rate and blind to a backlog on ours -- measured on the G2 as a permanent
    //DH1KLM 26-45 packets held here for the life of the session.
    m_speakerPacer.setBacklog(queued, kMaxQueuedSpeakerPackets);
    int toSend = m_speakerPacer.packetsToSend(queued);
    std::size_t consumed = 0;
    for (; toSend > 0; --toSend) {
        const std::span<const std::int16_t> block(m_speakerPending.data() + consumed,
                                                  kPacketSamples);
        const auto pkt = buildSpeakerAudio(m_speakerSequence, block);
        const qint64 sent = m_socket->writeDatagram(
            reinterpret_cast<const char*>(pkt.data()),
            static_cast<qint64>(pkt.size()), m_host, kSpeakerAudioPort);
        if (sent != static_cast<qint64>(pkt.size())) {
            //DH1KLM Neither counted into the pacer nor consumed from the queue: a
            //DH1KLM packet the socket refused never reached the radio's FIFO, so
            //DH1KLM crediting it would read a dead link as a full buffer and stop the
            //DH1KLM stream permanently. Retried on the next tick.
            break;
        }
        //DH1KLM Counted here rather than in sendTo(): this is the one send that does
        //DH1KLM not go through it. Reached only on the accepted path -- the refused
        //DH1KLM case broke out above.
        m_txBytes += static_cast<quint64>(sent);
        ++m_speakerSequence;
        m_speakerPacer.onPacketSent();
        consumed += kPacketSamples;
    }
    if (consumed > 0) {
        m_speakerPending.erase(m_speakerPending.begin(),
                               m_speakerPending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}

void P2Client::setDdc0FrequencyHz(double hz)
{
    m_ddc0FreqWord = phaseWord(hz);
    //DH1KLM Moves with m_ddc0FreqWord and nothing else: a stale value would energise
    //DH1KLM the filter for a band the receiver has left.
    m_ddc0FreqHz = hz;
    if (m_running && m_socket) {
        sendRunningHighPriority();
    }
}

void P2Client::setStepAttenuationDb(int adcIndex, int db)
{
    if (adcIndex == 0) {
        m_adc0AttenuationDb = db;
    } else if (adcIndex == 1) {
        m_adc1AttenuationDb = db;
    } else {
        return;
    }
    //DH1KLM Now rather than on the next keepalive: the operator is moving a
    //DH1KLM control and should hear the change, not wait up to 100 ms for it.
    if (m_running && m_socket) {
        sendRunningHighPriority();
    }
}

void P2Client::setBandFiltersEnabled(bool on)
{
    m_bandFiltersEnabled = on;
    if (m_running && m_socket) {
        sendRunningHighPriority();
    }
}

void P2Client::setSpeakerAmpMuted(bool on)
{
    m_speakerAmpMuted = on;
    if (m_running && m_socket) {
        sendRunningHighPriority();
    }
}

HighPriorityOptions P2Client::highPriorityOptions() const
{
    HighPriorityOptions opts;
    opts.muteSpeakerAmp = m_speakerAmpMuted;
    if (m_bandFiltersEnabled) {
        //DH1KLM Both banks follow DDC0's frequency, as deskHPSDR's do with one
        //DH1KLM receiver: an unset bank would keep whatever the last session
        //DH1KLM energised. The selection replaces the bypass flags; they are the
        //DH1KLM same relay bit.
        //DH1KLM
        //DH1KLM The relays switch only when the tuned frequency crosses a band
        //DH1KLM edge, and AnanBackend throttles retunes before they reach here
        //DH1KLM (scheduleTuneApply()), so a drag across bands sends a few packets.
        const AlexRxFilter filter = alexRxFilterFor(m_ddc0FreqHz);
        opts.adc0Filter = filter;
        opts.adc1Filter = filter;
    }
    return opts;
}

void P2Client::sendRunningHighPriority()
{
    sendTo(*m_socket,
          buildHighPriority(true, sharedFreqWords(),
                            m_bypassAdc0Filters, m_bypassAdc1Filters,
                            m_adc0AttenuationDb, m_adc1AttenuationDb,
                            highPriorityOptions()),
          m_host, kHighPriorityPort, m_txBytes);
}

bool P2Client::setDdcRateLive(int ddcIndex, int rateKsps)
{
    if (!m_running || !m_socket)
        return false;
    if (ddcIndex < 0 || ddcIndex >= static_cast<int>(m_activeDdcs.size()))
        return false;

    auto& slot = m_activeDdcs[static_cast<std::size_t>(ddcIndex)];
    const bool rateChanged = slot.rateKsps != rateKsps;
    slot.rateKsps = rateKsps;
    //DH1KLM Whole packet, every DDC's row -- see this function's declaration
    //DH1KLM comment. Same destination port start() used: p2app tells DDC-Specific
    //DH1KLM from High Priority by which port it arrives on, so this is not
    //DH1KLM interchangeable with kRadioPort. Sent even when rateChanged is false:
    //DH1KLM AnanBackend retries this call at 60/140 ms for UDP loss, and those
    //DH1KLM retries must not no-op after the first write (#5547).
    sendTo(*m_socket,
          buildDdcSpecific(m_activeDdcs, /*numAdcs=*/2,
                           m_ditherEnabled, m_randomEnabled),
          m_host, kDdcSpecificPort, m_txBytes);

    //DH1KLM The stream's sample cadence changes underneath us from here, so the
    //DH1KLM sequence expectation for THIS DDC is no longer meaningful -- clear it
    //DH1KLM rather than let the next frame look like a gap and inflate the drop
    //DH1KLM counter for what is a deliberate, operator-initiated change. Retries
    //DH1KLM of the same rate leave the tracker alone: the cadence did not change
    //DH1KLM again.
    if (rateChanged) {
        m_expectedSeq[static_cast<std::size_t>(ddcIndex)].reset();
    }
    return true;
}

void P2Client::onKeepaliveTick()
{
    if (!m_running || !m_socket)
        return;
    //DH1KLM This IS the "any C&C packet" the watchdog needs (p.8) -- no need to
    //DH1KLM also replay General/DDC-Specific on this cadence, same as the spike.
    sendRunningHighPriority();
}

void P2Client::onConnectTimeout()
{
    if (!m_linkUp)
        emit connectionError(QStringLiteral(
            "no DDC0 IQ from the radio within %1 ms of start").arg(m_activeConnectTimeoutMs));
}

void P2Client::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        handleDatagramFrom(dg.senderAddress(), asBytes(dg.data()),
                           static_cast<quint16>(dg.senderPort()));
    }
}

void P2Client::handleDatagramFrom(const QHostAddress& sender,
                                  std::span<const std::uint8_t> bytes, quint16 senderPort)
{
    //DH1KLM The radio answers from the address start() sent to: Discovery is unicast
    //DH1KLM to m_host from this same socket, so its reply passes too. A spoofed
    //DH1KLM source still passes; this keeps out stray LAN traffic, not an attacker.
    if (!sender.isEqual(m_host, QHostAddress::TolerantConversion)) {
        if (!m_warnedForeignSender) {
            m_warnedForeignSender = true;
            qCWarning(lcOriginalAnanP2).nospace()
                << "ANAN: ignoring datagram from " << sender.toString()
                << " (the radio is " << m_host.toString()
                << ") -- further foreign senders this session are not logged";
        }
        return;
    }
    handleDatagram(bytes, senderPort);
}

//DH1KLM Socket-free ingest seam: production and regression tests share validation,
//DH1KLM per-DDC sequence tracking, notification ordering and sample delivery.
void P2Client::handleDatagram(std::span<const std::uint8_t> bytes, quint16 senderPort)
{
    //DH1KLM Before any parse decides what this datagram IS: the transport carried it
    //DH1KLM either way, and LinkStats::alive reads bytes, so a session whose IQ has
    //DH1KLM stalled while Status packets still arrive does not read as a dead link.
    m_rxBytes += static_cast<quint64>(bytes.size());

    const auto frame = parseDdcFrame(bytes);
    if (!frame) {
        //DH1KLM Not DDC0-shaped: Mic Data, High Priority Status, or this session's
        //DH1KLM Discovery reply. Parse the reply on EVERY datagram (notify once): skipping it
        //DH1KLM would let a duplicate reply fall through and decode as a phantom underflow.
        if (const auto reply = parseDiscoveryReply(bytes)) {
            if (!m_discoveryInfoSent) {
                m_discoveryInfoSent = true;
                emit discoveryInfoReceived(reply->boardId, reply->firmwareVer,
                                           reply->numDdc);
            }
            return;
        }
        //DH1KLM Tried after the Discovery reply (both 60 bytes): a status packet with
        //DH1KLM sequence 0 and byte 4 = 0x02/0x03 reads as a reply and is skipped -- one
        //DH1KLM diagnostic packet, never a fake fault.
        if (const auto status = parseHighPriorityStatus(bytes)) {
            noteSpeakerFifoStatus(*status);
            emit supplyRailSampled(status->supplyRailCounts);
        }
        return;
    }

    //DH1KLM Which DDC sent this. The IQ packet carries no DDC index of its
    //DH1KLM own, so the sender port is the only discriminator -- see
    //DH1KLM ddcIndexForSenderPort()'s comment for how that is verified.
    const auto ddcIndex = ddcIndexForSenderPort(
        static_cast<std::uint16_t>(senderPort), m_activeDdcCount);
    if (!ddcIndex) {
        //DH1KLM DDC-shaped but from a port this session did not enable (another client, or a
        //DH1KLM DDC left running): drop it without counting a drop. Warn once per port, since
        //DH1KLM a source-port mismatch (other firmware, NAT, negotiated ports) would otherwise
        //DH1KLM present only as onConnectTimeout()'s "no DDC0 IQ".
        if (!m_warnedUnexpectedPorts.contains(senderPort)) {
            m_warnedUnexpectedPorts.insert(senderPort);
            qCWarning(lcOriginalAnanP2).nospace()
                << "ANAN: dropping DDC-shaped datagram from unexpected "
                   "sender port " << senderPort << " (expected "
                << kDdc0DefaultPort << ".."
                << (kDdc0DefaultPort + m_activeDdcCount - 1)
                << " for " << m_activeDdcCount
                << " active DDC(s)) -- not counted as a drop";
        }
        return;
    }
    const std::size_t slot = static_cast<std::size_t>(*ddcIndex);
    //DH1KLM The denominator rxPacketsLost is a fraction of: sequence-tracked DDC
    //DH1KLM frames only. Mic Data and Status would dilute RadioModel's loss
    //DH1KLM percentage several-fold at low DDC rates.
    ++m_rxPackets;

    //DH1KLM Per-DDC gap detection; see m_expectedSeq's own comment for why a
    //DH1KLM shared counter would manufacture drops once a second DDC streams.
    if (m_expectedSeq[slot] && frame->seq != *m_expectedSeq[slot]) {
        ++m_drops;
        emit dropsUpdated(m_drops);
        //DH1KLM A rewind or duplicate is not forward loss, but its samples still
        //DH1KLM break continuity. Notify before delivery, independently of the legacy
        //DH1KLM drop counter (which counts each mismatch, not each missing packet).
        emit ddcSequenceGap(*ddcIndex);
    }
    m_expectedSeq[slot] = frame->seq + 1;

    //DH1KLM linkUp stays keyed on DDC0: it is the receiver every session has,
    //DH1KLM and the connect timeout's message says "no DDC0 IQ". A session
    //DH1KLM whose DDC1 streamed but whose DDC0 never did is a real failure,
    //DH1KLM not a connected session.
    if (!m_linkUp && *ddcIndex == 0) {
        m_linkUp = true;
        m_connectTimeoutTimer->stop();
        emit linkUp();
    }

    m_decodeScratch.clear();
    decodeIq(*frame, m_decodeScratch);
    emit ddcIqReady(*ddcIndex, m_decodeScratch);
    if (*ddcIndex == 0)
        emit ddc0IqReady(m_decodeScratch);
}

}  // namespace AetherSDR::anan_original
