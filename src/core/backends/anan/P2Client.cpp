#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/AnanFilterOptions.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

Q_LOGGING_CATEGORY(lcAnanP2, "aether.anan.p2")

namespace AetherSDR::anan {

namespace {

// View a QByteArray as a byte span for the protocol decoders. Same helper
// MetisClient.cpp defines locally for the same purpose.
std::span<const std::uint8_t> asBytes(const QByteArray& d) noexcept
{
    return {reinterpret_cast<const std::uint8_t*>(d.constData()),
           static_cast<std::size_t>(d.size())};
}

// txAccum is REQUIRED, not defaulted: a new send site cannot compile without
// deciding where its bytes are counted, which is what keeps the LinkStats tx
// total from silently under-reporting as this client grows.
template <std::size_t N>
qint64 sendTo(QUdpSocket& s, const std::array<std::uint8_t, N>& buf,
             const QHostAddress& host, quint16 port, quint64& txAccum)
{
    const qint64 sent = s.writeDatagram(reinterpret_cast<const char*>(buf.data()),
                                        static_cast<qint64>(N), host, port);
    // Only what the socket ACCEPTED. Counting a refused datagram would inflate
    // the readout exactly when the link is failing, which is when an operator
    // is most likely to be reading it.
    if (sent > 0)
        txAccum += static_cast<quint64>(sent);
    return sent;
}

}  // namespace

P2Client::P2Client(QObject* parent) : QObject(parent)
{
    connect(this, &P2Client::connectionError, this, [](const QString& reason) {
        qCWarning(lcAnanP2) << "ANAN P2: connection failed:" << reason;
    });
    m_keepaliveTimer = new QTimer(this);
    m_keepaliveTimer->setInterval(kKeepaliveMs);
    connect(m_keepaliveTimer, &QTimer::timeout, this, &P2Client::onKeepaliveTick);

    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &P2Client::onConnectTimeout);

    m_speakerDrainTimer = new QTimer(this);
    m_speakerDrainTimer->setInterval(kSpeakerDrainMs);
    // A realtime stream's pacing timer, so ask for the accurate one. Qt's default
    // CoarseTimer may be adjusted by up to 5% of the interval, which at 5 ms is
    // the same order as the interval itself; the pacer's target absorbs jitter but
    // there is no reason to hand it more than necessary.
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

    //DH1KLM: Do not silently turn a bad hardware profile into Saturn's two-ADC
    // layout. A future board profile must provide its verified ADC count.
    if (params.numAdcs < 1 || params.numAdcs > 2) {
        qCWarning(lcAnanP2) << "ANAN: refusing session with invalid board ADC count"
                            << params.numAdcs;
        return false;
    }

    m_ddc0FreqWord = 0;
    m_firstDdcIndex = 0;
    m_selectReceiveLpf = false;
    m_receiveFilterKind = ReceiveFilterKind::MkIIBpf;
    m_swapSpeakerChannels = false;
    m_filters = params.filters;
    m_receiveFilterPlan = {};
    m_bypassAdc0Filters = params.bypassAdc0Filters;
    m_bypassAdc1Filters = params.bypassAdc1Filters;
    m_alexAttenuationDb = std::clamp(params.alexAttenuationDb / 10, 0, 3) * 10;
    m_adc0AttenuationDb = params.adc0AttenuationDb;
    m_adc1AttenuationDb = params.adc1AttenuationDb;
    m_rxAntenna = (params.rxAntenna >= 1 && params.rxAntenna <= 3) ? params.rxAntenna : 1;
    //DH1KLM: Keep the validated hardware ADC count with the session; never infer it from the DDC count.
    m_numAdcs = params.numAdcs;
    m_speakerAudioEnabled = params.speakerAudioEnabled;
    m_speakerPending.clear();
    m_speakerPacer.reset();
    // Sequence restarts per session, like the DDC expectations below: p2app
    // reads it only to spot gaps within one stream.
    m_speakerSequence = 0;
    m_speakerOverflowLogged = false;
    m_speakerUnderflowReports = 0;
    m_lastSpeakerFifoLevel = 0;
    // Every DDC's sequence tracker, not just DDC0's -- a stale expectation
    // carried across a restart would report a phantom gap on the new
    // session's first frame.
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
    m_discoveryValidated = false;
    m_generalPacket.reset();
    m_generalClock.invalidate();
    m_expectedBoardId = params.expectedBoardId;
    m_model = params.model;
    m_overrideDetection = params.overrideDetection;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &P2Client::onReadyRead);

    m_running = true;

    //DH1KLM: Send only Discovery initially. Until its reply proves this is
    // a supported RX board (matching the selected ID when known), do not
    // send any board-specific General/DDC-Specific/High-Priority packets.
    sendTo(*m_socket, buildDiscovery(), m_host, kRadioPort, m_txBytes);

    // Resolve the session's DDC list ONCE: either the caller's explicit
    // multi-DDC list, or a one-element list from the DDC0 shorthand. See
    // Params::activeDdcs for why these are not merged.
    std::vector<DdcConfig> ddcs = params.activeDdcs;
    if (ddcs.empty())
        ddcs.push_back(DdcConfig{params.ddc0RateKsps, params.ddc0AdcIndex});
    if (static_cast<int>(ddcs.size()) > kMaxDdcs)
        ddcs.resize(kMaxDdcs);
    m_activeDdcCount = static_cast<int>(ddcs.size());
    // Retained for setDdcRateLive() -- see its own comment for why the whole
    // list (not just the count) has to survive start().
    m_activeDdcs = ddcs;
    m_independentTuning = false;
    m_ditherEnabled = params.ditherEnabled;
    m_randomEnabled = params.randomEnabled;

    //DH1KLM: The resolved DDC list and flags are retained in m_activeDdcs
    // above until Discovery validates the board. No profile-specific packet
    // is emitted in start().
    qCInfo(lcAnanP2) << "ANAN P2: discovery sent to" << m_host.toString() << "local endpoint" << localEndpoint();
    m_activeConnectTimeoutMs = connectTimeoutMs;
    m_linkCountersTimer->start();
    m_connectTimeoutTimer->start(connectTimeoutMs);
    return true;
}

void P2Client::stop()
{
    m_keepaliveTimer->stop();
    m_linkCountersTimer->stop();
    m_connectTimeoutTimer->stop();
    m_speakerDrainTimer->stop();
    // Dropped, not drained. These samples are older than the disconnect and the
    // radio's own FIFO does not survive it either, so playing them out on the
    // next session would start it with a fragment of the last one.
    m_speakerPending.clear();
    m_speakerPacer.reset();
    m_speakerSequence = 0;
    m_speakerOverflowLogged = false;
    m_speakerUnderflowReports = 0;
    m_lastSpeakerFifoLevel = 0;
    if (m_socket) {
        //DH1KLM: Only send the selected profile's stop after its identity
        // was validated. Preserve physical DDC mapping through teardown.
        if (m_discoveryValidated) {
            //DH1KLM: Retain Orion MkII filter selection during stop; keep
            // the existing all-zero Saturn frequency words unchanged.
            const std::vector<std::uint32_t> stopWords = m_selectReceiveLpf
                ? sharedFreqWords()
                : std::vector<std::uint32_t>(static_cast<std::size_t>(m_activeDdcCount), 0);
            sendTo(*m_socket,
                  buildHighPriority(false, stopWords, m_bypassAdc0Filters, m_bypassAdc1Filters,
                                    m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna,
                                    m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
                  m_host, kHighPriorityPort, m_txBytes);
        }
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_running = false;
    m_generalPacket.reset();
    m_generalClock.invalidate();
    if (m_linkUp) {
        m_linkUp = false;
        qCInfo(lcAnanP2) << "ANAN P2: session stopped";
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
    // Empty rather than a partial string when there is no socket or the bind
    // did not take: LinkStats::localEndpoint is documented as empty when not
    // bound, and the consumer falls back to its own source on empty.
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return {};
    const quint16 port = m_socket->localPort();
    if (port == 0)
        return {};
    // AnyIPv4 prints as "0.0.0.0", which is the honest answer for a wildcard
    // bind -- the socket genuinely is not pinned to one interface. The PORT is
    // the load-bearing half anyway: it is what the radio streams DDC0 to.
    return QStringLiteral("%1:%2").arg(m_socket->localAddress().toString())
                                  .arg(port);
}

void P2Client::noteSpeakerFifoStatus(const HighPriorityStatus& status)
{
    m_lastSpeakerFifoLevel = status.speakerFifoLevel;
    // EVERY status packet, healthy or not, at debug. The underflow bit only tells
    // us the stream ALREADY failed; the level is what shows it coming, and a bench
    // run needs to watch it hold steady rather than only learn afterwards that it
    // did not. Five lines a second, so debug-gated:
    //   QT_LOGGING_RULES="aether.anan.p2.debug=true"
    //
    // Logged even while the stream is disabled: then it is the radio's own idle
    // level, which is the baseline a later run is read against.
    if (m_speakerAudioEnabled) {
        qCDebug(lcAnanP2).nospace()
            << "ANAN: speaker FIFO level " << status.speakerFifoLevel
            << " (FIFO locations), underflow reports so far "
            << m_speakerUnderflowReports << ", queued "
            << (m_speakerPending.size() / (kSpeakerFramesPerPacket * kSpeakerChannels))
            << " packets" << (m_speakerPacer.catchingUp() ? ", CATCHING UP" : "");
    } else {
        qCDebug(lcAnanP2).nospace()
            << "ANAN: speaker FIFO level " << status.speakerFifoLevel
            << " (FIFO locations), stream disabled -- this is the radio's idle level";
    }
    if (!status.speakerUnderflow) {
        return;
    }
    ++m_speakerUnderflowReports;
    // The first underflow warns; later ones are only counted, since a starved
    // stream sets the latched bit on every ~200 ms status packet. p2app reports
    // underflow even when nobody feeds the FIFO, so it only means something while
    // we are sending.
    if (!m_speakerAudioEnabled) {
        return;
    }
    if (m_speakerUnderflowReports == 1) {
        qCWarning(lcAnanP2).nospace()
            << "ANAN: the radio's speaker FIFO underflowed -- audio to the radio "
               "is arriving too slowly and will click or stutter. FIFO level "
            << status.speakerFifoLevel
            << " (FIFO locations, not samples). Further reports are counted, not "
               "logged; read speakerUnderflowReports().";
    } else {
        qCDebug(lcAnanP2).nospace()
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
        // DROP THE OLDEST, not the newest. This is a realtime stream: the newest
        // audio is the audio the operator is listening for, and discarding it to
        // preserve a backlog they have already missed hearing keeps the latency
        // and loses the content. Dropping from the front loses the same amount of
        // audio and recovers the latency with it.
        const std::size_t overflow = m_speakerPending.size() + count - cap;
        const std::size_t drop = std::min(overflow, m_speakerPending.size());
        m_speakerPending.erase(m_speakerPending.begin(),
                               m_speakerPending.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!m_speakerOverflowLogged) {
            m_speakerOverflowLogged = true;
            qCWarning(lcAnanP2) << "ANAN: speaker audio queue overflowed, dropping oldest"
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

    // Elapsed first: the pacer's whole estimate is "sent minus drained", and
    // draining is what happened between ticks.
    const qint64 elapsedNs = m_speakerClock.nsecsElapsed();
    m_speakerClock.restart();
    m_speakerPacer.advance(static_cast<double>(elapsedNs) * 1.0e-9);

    const int queued = static_cast<int>(m_speakerPending.size() / kPacketSamples);
    // Tell the pacer how far behind WE are before asking what may go. Without
    // this it only ever knows about the radio's side, which is correct for the
    // rate and blind to a backlog on ours -- measured on the G2 as a permanent
    // 26-45 packets held here for the life of the session.
    m_speakerPacer.setBacklog(queued, kMaxQueuedSpeakerPackets);
    int toSend = m_speakerPacer.packetsToSend(queued);
    std::size_t consumed = 0;
    for (; toSend > 0; --toSend) {
        const std::span<const std::int16_t> block(m_speakerPending.data() + consumed,
                                                  kPacketSamples);
        const auto pkt = buildSpeakerAudio(m_speakerSequence, block, m_swapSpeakerChannels);
        const qint64 sent = m_socket->writeDatagram(
            reinterpret_cast<const char*>(pkt.data()),
            static_cast<qint64>(pkt.size()), m_host, kSpeakerAudioPort);
        if (sent != static_cast<qint64>(pkt.size())) {
            // Neither counted into the pacer nor consumed from the queue: a
            // packet the socket refused never reached the radio's FIFO, so
            // crediting it would read a dead link as a full buffer and stop the
            // stream permanently. Retried on the next tick.
            break;
        }
        // Counted here rather than in sendTo(): this is the one send that does
        // not go through it. Reached only on the accepted path -- the refused
        // case broke out above.
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
    //DH1KLM: Match the P1 tuning boundary before floating-to-integer encoding;
    // malformed input must leave the last valid receive word unchanged.
    if (!std::isfinite(hz) || hz < 0 || hz > static_cast<double>(kDspClockHz) / 2.0) {
        return;
    }
    m_ddc0FreqWord = phaseWord(hz);
    m_receiveWords[0] = m_ddc0FreqWord;
    if (m_running && m_socket && m_discoveryValidated)
        sendTo(*m_socket,
              buildHighPriority(true, sharedFreqWords(),
                                m_bypassAdc0Filters, m_bypassAdc1Filters,
                                m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna,
                                m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
              m_host, kHighPriorityPort, m_txBytes);
}

//DH1KLM: Apply ANT1/ANT2/ANT3 through Thetis' Alex0 antenna bits.
void P2Client::setHardwareFilters(const QVariantMap& options)
{
    const auto config = parseFilterOptions(options);
    if (!config) { return; }
    m_filters = *config;
    m_bypassAdc0Filters = config->bypassAdc0;
    m_bypassAdc1Filters = config->bypassAdc1;
    setRxAntenna(m_rxAntenna);
}

void P2Client::setRxAntenna(int antenna)
{
    if (antenna < 1 || antenna > 3)
        return;
    m_rxAntenna = antenna;
    if (m_running && m_socket && m_discoveryValidated) {
        sendTo(*m_socket,
              buildHighPriority(true, sharedFreqWords(),
                                m_bypassAdc0Filters, m_bypassAdc1Filters,
                                m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna,
                                m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
              m_host, kHighPriorityPort, m_txBytes);
    }
}

void P2Client::setFrontEndAttenuation(int adc, int db, int alexDb)
{
    if (adc < 0 || adc >= m_numAdcs) { return; }
    if (adc == 0) {
        m_alexAttenuationDb = m_receiveFilterKind == ReceiveFilterKind::LegacyHpf
            ? std::clamp(alexDb / 10, 0, 3) * 10 : 0;
    }
    setStepAttenuationDb(adc, db);
}
void P2Client::setStepAttenuationDb(int adcIndex, int db)
{
    if (adcIndex == 0) {
        m_adc0AttenuationDb = std::clamp(db, 0, kMaxStepAttenuationDb);
    } else if (adcIndex == 1 && m_numAdcs == 2) {
        m_adc1AttenuationDb = std::clamp(db, 0, kMaxStepAttenuationDb);
    } else {
        return;
    }
    // Now rather than on the next keepalive: the operator is moving a
    // control and should hear the change, not wait up to 100 ms for it.
    if (m_running && m_socket && m_discoveryValidated) {
        sendTo(*m_socket,
              buildHighPriority(true, sharedFreqWords(),
                                m_bypassAdc0Filters, m_bypassAdc1Filters,
                                m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna,
                                m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
              m_host, kHighPriorityPort, m_txBytes);
    }
}

bool P2Client::setDdcRateLive(int ddcIndex, int rateKsps)
{
    if (!m_socket && !m_ddcRateSendOverride) { return false; }
    const auto packet = prepareDdcRateChange(ddcIndex, rateKsps);
    if (!packet) { return false; }
    //DH1KLM: Repeat unchanged valid rates for the backend's UDP-loss retries.
    qint64 sent = 0;
    if (m_ddcRateSendOverride) {
        sent = m_ddcRateSendOverride(*packet);
        if (sent > 0) { m_txBytes += static_cast<quint64>(sent); }
    } else {
        sent = sendTo(*m_socket, *packet, m_host, kDdcSpecificPort, m_txBytes);
    }
    if (sent != static_cast<qint64>(packet->size())) { return false; }
    auto& slot = m_activeDdcs[static_cast<std::size_t>(ddcIndex)];
    if (slot.rateKsps != rateKsps) {
        slot.rateKsps = rateKsps;
        m_expectedSeq[static_cast<std::size_t>(ddcIndex)].reset();
    }
    return true;
}

std::optional<std::array<std::uint8_t, 1444>> P2Client::prepareDdcRateChange(int ddcIndex, int rateKsps) const
{
    if (!m_running || !m_discoveryValidated
        || ddcIndex < 0 || ddcIndex >= static_cast<int>(m_activeDdcs.size())
        || std::find(kDdc0RatesKsps.begin(), kDdc0RatesKsps.end(), rateKsps) == kDdc0RatesKsps.end()) {
        return std::nullopt;
    }
    auto ddcs = m_activeDdcs;
    ddcs[static_cast<std::size_t>(ddcIndex)].rateKsps = rateKsps;
    //DH1KLM: Preserve every other enabled DDC, ADC route and linearisation
    // flag when replacing one rate. Disabled physical rows remain disabled.
    return buildDdcSpecific(ddcs, m_numAdcs,
        m_ditherEnabled, m_randomEnabled, m_firstDdcIndex);
}

std::optional<std::array<std::uint8_t, 60>> P2Client::takeGeneralRefresh(qint64 nowMs)
{
    if (!m_running || !m_discoveryValidated || !m_generalPacket
        || nowMs < m_nextGeneralRefreshMs) {
        return std::nullopt;
    }
    //DH1KLM: Coalesce late ticks instead of bursting stale retries after a stall.
    m_nextGeneralRefreshMs = nowMs + kGeneralRefreshMs;
    return m_generalPacket;
}

void P2Client::onKeepaliveTick()
{
    if (!m_running || !m_socket || !m_discoveryValidated)
        return;
    //DH1KLM: Match Thetis KeepAliveLoop's 500 ms General refresh. HP alone
    // keeps RUN alive but cannot recover a lost ALEX-enable General packet.
    if (m_generalClock.isValid()) {
        if (const auto general = takeGeneralRefresh(m_generalClock.elapsed())) {
            if (m_independentTuning)
                sendTo(*m_socket,
                       buildDdcSpecific(m_activeDdcs, m_numAdcs, m_ditherEnabled, m_randomEnabled,
                                        m_firstDdcIndex),
                       m_host, kDdcSpecificPort, m_txBytes);
            sendTo(*m_socket, *general, m_host, kRadioPort, m_txBytes);
        }
    }
    sendTo(*m_socket,
          buildHighPriority(true, sharedFreqWords(),
                            m_bypassAdc0Filters, m_bypassAdc1Filters,
                            m_adc0AttenuationDb, m_adc1AttenuationDb, m_rxAntenna,
                            m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
          m_host, kHighPriorityPort, m_txBytes);
}

void P2Client::onConnectTimeout()
{
    if (m_linkUp)
        return;
    qCWarning(lcAnanP2) << "ANAN P2: connection timeout; phase"
        << (m_discoveryValidated ? "waiting for logical RX0 IQ" : "waiting for valid P2 discovery")
        << "timeout ms" << m_activeConnectTimeoutMs;
    if (!m_discoveryValidated) {
        emit connectionError(QStringLiteral(
            "ANAN: no valid P2 Discovery reply from the radio within %1 ms; "
            "no board-specific session packets were sent").arg(m_activeConnectTimeoutMs));
        stop();
        return;
    }
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
    // The radio answers from the address start() sent to: Discovery is unicast
    // to m_host from this same socket, so its reply passes too. A spoofed
    // source still passes; this keeps out stray LAN traffic, not an attacker.
    if (!sender.isEqual(m_host, QHostAddress::TolerantConversion)) {
        if (!m_warnedForeignSender) {
            m_warnedForeignSender = true;
            qCWarning(lcAnanP2).nospace()
                << "ANAN: ignoring datagram from " << sender.toString()
                << " (the radio is " << m_host.toString()
                << ") -- further foreign senders this session are not logged";
        }
        return;
    }
    handleDatagram(bytes, senderPort);
}

// Socket-free ingest seam: production and regression tests share validation,
// per-DDC sequence tracking, notification ordering and sample delivery.
void P2Client::handleDatagram(std::span<const std::uint8_t> bytes, quint16 senderPort)
{
    // Before any parse decides what this datagram IS: the transport carried it
    // either way, and LinkStats::alive reads bytes, so a session whose IQ has
    // stalled while Status packets still arrive does not read as a dead link.
    m_rxBytes += static_cast<quint64>(bytes.size());

    const auto frame = parseDdcFrame(bytes);
    if (!frame) {
        // Not DDC0-shaped: Mic Data, High Priority Status, or this session's
        // Discovery reply. Parse the reply on EVERY datagram (notify once): skipping it
        // would let a duplicate reply fall through and decode as a phantom underflow.
        //DH1KLM: Session discovery must come from its discovery endpoint,
        // not a status/IQ source whose payload happens to resemble a reply.
        if (const auto reply = parseDiscoveryReply(bytes)) {
            // Still consume reply-shaped duplicates from other ports: they
            // must neither authorize the session nor become speaker status.
            if (senderPort != 0 && senderPort != kRadioPort)
                return;
            if (!m_discoveryValidated) {
                if (reply->streaming) {
                    emit connectionError(QStringLiteral("ANAN: the P2 radio is already in use"));
                    stop();
                    return;
                }
                //DH1KLM: Require an implemented RX profile and matching board ID.
                // A recognized family alone does not authorize session packets.
                const auto resolvedProfile = receiveProfile(reply->boardId, m_model, m_overrideDetection);
                if (!resolvedProfile || (m_expectedBoardId >= 0
                        && reply->boardId != m_expectedBoardId)) {
                    const QString expected = m_expectedBoardId >= 0
                        ? QString::number(m_expectedBoardId)
                        : QStringLiteral("an implemented receive profile");
                    emit connectionError(QStringLiteral(
                        "ANAN: Discovery reports board ID %1, but this connection "
                        "requires a supported matching RX profile (%2); no board-specific session packets were sent")
                        .arg(static_cast<int>(reply->boardId)).arg(expected));
                    stop();
                    return;
                }

                //DH1KLM: Select the physical RX layout from the actual reply,
                // not from the reported receiver count or the display name.
                const ReceiveProfile profile = *resolvedProfile;
                m_streamLimit = std::min(5, profile.physicalDdcLimit - profile.firstDdcIndex);
                if (const auto physical = receiveProfile(reply->boardId))
                    m_streamLimit =
                        std::min(m_streamLimit, physical->physicalDdcLimit - profile.firstDdcIndex);
                if (reply->reportedReceivers > 0)
                    m_streamLimit = std::min(m_streamLimit, int(reply->reportedReceivers));
                const bool invalidDdc = std::any_of(m_activeDdcs.begin(), m_activeDdcs.end(),
                    [&profile](const DdcConfig& ddc) {
                        return ddc.adcIndex < 0 || ddc.adcIndex >= profile.numAdcs
                            || std::find(kDdc0RatesKsps.begin(), kDdc0RatesKsps.end(), ddc.rateKsps)
                                == kDdc0RatesKsps.end();
                    });
                //DH1KLM: An address-only caller cannot know the ADC count;
                // derive it from the actual board's explicit profile.
                if (m_expectedBoardId < 0) {
                    m_numAdcs = profile.numAdcs;
                }
                if (profile.firstDdcIndex + m_activeDdcCount > profile.physicalDdcLimit
                    || m_numAdcs != profile.numAdcs || invalidDdc) {
                    emit connectionError(QStringLiteral("ANAN: DDC/ADC configuration exceeds the selected board profile"));
                    stop();
                    return;
                }
                m_firstDdcIndex = profile.firstDdcIndex;
                m_selectReceiveLpf = profile.selectReceiveLpf;
                m_receiveFilterKind = profile.filterKind;
                if (m_receiveFilterKind == ReceiveFilterKind::MkIIBpf && m_alexAttenuationDb != 0) {
                    m_adc0AttenuationDb = std::min(31, m_adc0AttenuationDb + m_alexAttenuationDb);
                    m_alexAttenuationDb = 0;
                }
                m_swapSpeakerChannels = profile.swapSpeakerChannels;
                m_discoveryValidated = true;
                qCInfo(lcAnanP2) << "ANAN P2: validated board" << reply->boardId
                    << "model" << QString::fromLatin1(ananModelInfo(m_model)->name.data()) << "override" << m_overrideDetection
                    << "first physical DDC" << m_firstDdcIndex << "ADCs" << m_numAdcs
                    << "logical RX0 ADC" << (m_activeDdcs.empty() ? -1 : m_activeDdcs.front().adcIndex)
                    << "rate ksps" << (m_activeDdcs.empty() ? 0 : m_activeDdcs.front().rateKsps);
                //DH1KLM: Cache only the resolved session profile; never rebuild
                // retries from an automatic/default model or unvalidated reply.
                m_generalPacket = buildGeneral(profile.alexEnableMask, profile.disablePa);
                m_generalClock.start();
                m_nextGeneralRefreshMs = kGeneralRefreshMs;
                //DH1KLM: Publish identity only after profile validation, so the
                // backend never seeds defaults or capabilities for a rejected board.
                if (!m_discoveryInfoSent) {
                    m_discoveryInfoSent = true;
                    emit discoveryInfoReceived(reply->boardId, reply->firmwareVer,
                                               reply->reportedReceivers);
                }
                if (m_socket) {
                    sendTo(*m_socket, *m_generalPacket, m_host, kRadioPort, m_txBytes);
                    sendTo(*m_socket,
                          buildDdcSpecific(m_activeDdcs, m_numAdcs,
                                           m_ditherEnabled, m_randomEnabled, m_firstDdcIndex),
                          m_host, kDdcSpecificPort, m_txBytes);
                    sendTo(*m_socket,
                          buildHighPriority(true, sharedFreqWords(),
                                            m_bypassAdc0Filters, m_bypassAdc1Filters,
                                            m_adc0AttenuationDb, m_adc1AttenuationDb,
                                            m_rxAntenna, m_firstDdcIndex, m_selectReceiveLpf, m_receiveFilterKind, m_numAdcs, m_filters ? &*m_filters : nullptr, m_alexAttenuationDb, &m_receiveFilterPlan),
                          m_host, kHighPriorityPort, m_txBytes);
                    m_keepaliveTimer->start();
                    if (m_speakerAudioEnabled) {
                        m_speakerClock.restart();
                        m_speakerDrainTimer->start();
                    }
                    // Discovery latency must not consume the DDC stream's
                    // entire connect window.
                    m_connectTimeoutTimer->start(m_activeConnectTimeoutMs);
                }
            }
            return;
        }
        // Tried after the Discovery reply (both 60 bytes): a status packet with
        // sequence 0 and byte 4 = 0x02/0x03 reads as a reply and is skipped -- one
        // diagnostic packet, never a fake fault.
        if (const auto status = parseHighPriorityStatus(bytes)) {
            noteSpeakerFifoStatus(*status);
            emit supplyRailSampled(status->supplyRailCounts);
        }
        return;
    }

    // Which DDC sent this. The IQ packet carries no DDC index of its
    // own, so the sender port is the only discriminator -- see
    // ddcIndexForSenderPort()'s comment for how that is verified.
    //DH1KLM: A pending session must not link up on stale DDC0 traffic
    // before Discovery has selected its physical receiver layout.
    if (m_running && !m_discoveryValidated) {
        return;
    }
    const auto ddcIndex = ddcIndexForSenderPort(
        static_cast<std::uint16_t>(senderPort), m_activeDdcCount,
        static_cast<std::uint16_t>(kDdc0DefaultPort + m_firstDdcIndex));
    if (!ddcIndex) {
        // DDC-shaped but from a port this session did not enable (another client, or a
        // DDC left running): drop it without counting a drop. Warn once per port, since
        // a source-port mismatch (other firmware, NAT, negotiated ports) would otherwise
        // present only as onConnectTimeout()'s "no DDC0 IQ".
        if (!m_warnedUnexpectedPorts.contains(senderPort)) {
            m_warnedUnexpectedPorts.insert(senderPort);
            qCWarning(lcAnanP2).nospace()
                << "ANAN: dropping DDC-shaped datagram from unexpected "
                   "sender port " << senderPort << " (expected "
                << (kDdc0DefaultPort + m_firstDdcIndex) << ".."
                << (kDdc0DefaultPort + m_firstDdcIndex + m_activeDdcCount - 1)
                << " for " << m_activeDdcCount
                << " active DDC(s)) -- not counted as a drop";
        }
        return;
    }
    const std::size_t slot = static_cast<std::size_t>(*ddcIndex);
    // The denominator rxPacketsLost is a fraction of: sequence-tracked DDC
    // frames only. Mic Data and Status would dilute RadioModel's loss
    // percentage several-fold at low DDC rates.
    ++m_rxPackets;

    // Per-DDC gap detection; see m_expectedSeq's own comment for why a
    // shared counter would manufacture drops once a second DDC streams.
    if (m_expectedSeq[slot] && frame->seq != *m_expectedSeq[slot]) {
        ++m_drops;
        emit dropsUpdated(m_drops);
        // A rewind or duplicate is not forward loss, but its samples still
        // break continuity. Notify before delivery, independently of the legacy
        // drop counter (which counts each mismatch, not each missing packet).
        emit ddcSequenceGap(*ddcIndex);
    }
    m_expectedSeq[slot] = frame->seq + 1;

    //DH1KLM: linkUp stays keyed on logical RX0 (DDC0 Saturn, DDC2 MkII):
    // and the connect timeout's message says "no DDC0 IQ". A session
    // whose DDC1 streamed but whose DDC0 never did is a real failure,
    // not a connected session.
    if (!m_linkUp && *ddcIndex == 0) {
        qCInfo(lcAnanP2) << "ANAN P2: RX IQ started; logical RX0, physical DDC" << m_firstDdcIndex;
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

}  // namespace AetherSDR::anan

namespace AetherSDR::anan {
bool P2Client::setReceiveStreams(const std::vector<DdcConfig>& streams,
                                 const std::vector<double>& centers)
{
    if (!m_running || !m_discoveryValidated || (!m_socket && !m_ddcRateSendOverride) ||
        streams.empty() || streams.size() != centers.size() ||
        streams.size() > std::size_t(m_streamLimit))
        return false;
    for (std::size_t i = 0; i < streams.size(); ++i)
        if (streams[i].adcIndex < 0 || streams[i].adcIndex >= m_numAdcs ||
            std::find(kDdc0RatesKsps.begin(), kDdc0RatesKsps.end(), streams[i].rateKsps) ==
                kDdc0RatesKsps.end() ||
            !std::isfinite(centers[i]) || centers[i] < 0 || centers[i] > 61'440'000)
            return false;
    const bool changed =
        streams.size() != m_activeDdcs.size() ||
        !std::equal(streams.begin(), streams.end(), m_activeDdcs.begin(), [](auto a, auto b) {
            return a.rateKsps == b.rateKsps && a.adcIndex == b.adcIndex;
        });
    if (changed) {
        const auto packet =
            buildDdcSpecific(streams, m_numAdcs, m_ditherEnabled, m_randomEnabled, m_firstDdcIndex);
        const auto sent = m_ddcRateSendOverride
                              ? m_ddcRateSendOverride(packet)
                              : sendTo(*m_socket, packet, m_host, kDdcSpecificPort, m_txBytes);
        if (sent != qint64(packet.size()))
            return false;
    }
    if (changed)
        qCInfo(lcAnanP2) << "ANAN P2: receive-stream configuration updated; logical streams"
                         << streams.size();
    for(std::size_t i=0;i<m_expectedSeq.size();++i) {
        if(i>=streams.size()||i>=m_activeDdcs.size()||streams[i].rateKsps!=m_activeDdcs[i].rateKsps||streams[i].adcIndex!=m_activeDdcs[i].adcIndex)
            m_expectedSeq[i].reset();
    }
    m_activeDdcs = streams;
    m_activeDdcCount = int(streams.size());
    m_independentTuning = true;
    for (std::size_t i = 0; i < centers.size(); ++i)
        m_receiveWords[i] = phaseWord(centers[i]);
    m_ddc0FreqWord = m_receiveWords[0];
    onKeepaliveTick();
    return true;
}
} // namespace AetherSDR::anan
