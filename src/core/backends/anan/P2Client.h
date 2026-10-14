#pragma once

#include "core/backends/anan/P2Protocol.h"
#include "core/backends/anan/AnanSpeakerPacing.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

// Owns the ANAN-G2 Protocol 2 UDP wire: session setup (General + DDC-Specific +
// High Priority with run=1), the keepalive that holds the radio in RUN, and DDC
// IQ ingest. Lives on AnanBackend's I/O thread (like MetisClient for HL2) so a
// GUI stall cannot starve the radio's watchdog.
// RX-ONLY: no PTT exists here or in buildHighPriority() (TX is RFC §2.11 Phase 3).
// The caller resolves the host, but this socket sends Discovery and validates the
// physical board ID before any board-profile-specific session packets. Every
// radio->PC stream goes to the source port of the Discovery packet (spec p.43, p.51,
// p.54), so Discovery must originate from THIS socket.
class P2Client : public QObject {
    Q_OBJECT

public:
    explicit P2Client(QObject* parent = nullptr);
    ~P2Client() override;

    struct Params {
        QString host;
        //DH1KLM: -1 means a manual/unidentified target; otherwise require the
        //Discovery reply to match the exact P2 board ID selected by discovery.
        int expectedBoardId = -1;
        //DH1KLM: Explicit per-radio P1 model override; ignored by P2 transport.
        bool redPitayaP1 = false;
        // Valid rates: 48/96/192/384/768/1536 (spec p.24). Not validated here
        // -- P2Protocol::buildDdcSpecific() is the single place that would
        // reject or clamp one, and it does neither yet; an invalid value is
        // simply sent as-is and the radio's own reaction is the feedback.
        int ddc0RateKsps = 48;
        // Connect-time-only options: no live setter; a change takes a reconnect.
        bool ditherEnabled = true;
        bool randomEnabled = true;
        int ddc0AdcIndex = 0;          // 0 = ADC0, 1 = ADC1/RX2
        //DH1KLM: Physical ADC count is a board-profile property; Saturn remains 2.
        int numAdcs = 2;
        bool bypassAdc0Filters = false; //DH1KLM: use Thetis Saturn BPF1 by default
        bool bypassAdc1Filters = true;
        // Receive step attenuators, 0-31 dB (buildHighPriority()). Unlike the
        // options above these ARE live: setStepAttenuationDb() changes them
        // mid-session. The values here are what the session starts with.
        int adc0AttenuationDb = 0;
        int adc1AttenuationDb = 0;
        //DH1KLM: RX antenna number 1..3 maps to Thetis Alex0 bits 24..26.
        int rxAntenna = 1;

        // Send demodulated RX audio to the radio's own speaker (DDC Audio,
        // kSpeakerAudioPort). DEFAULT OFF: it is the one option that originates a
        // continuous outbound stream (750 packets/s, ~260 kB/s).
        bool speakerAudioEnabled = false;

        // Multi-DDC session. EMPTY (default) = single DDC0 built from ddc0RateKsps/
        // ddc0AdcIndex. Non-empty is authoritative and those two fields are ignored;
        //DH1KLM: Entry n configures logical RX n at the selected profile's
        // physical DDC offset. The validated range must fit within kMaxDdcs.
        std::vector<DdcConfig> activeDdcs;
    };

    // start()/stop()/setters MUST run on this object's thread: start() creates the
    // QUdpSocket, which takes that thread's affinity. connectTimeoutMs exceeds the
    // default for a rate-change restart, where the radio is still re-settling from
    // the stop (AnanBackend::kRateChangeConnectTimeoutMs).
    Q_INVOKABLE bool start(const Params& params, int connectTimeoutMs = kConnectTimeoutMs);
    Q_INVOKABLE void stop();

    //DH1KLM: Retune logical RX0 (physical DDC0 Saturn, DDC2 Orion MkII).
    // No frequency is set by start() itself (see the class
    // comment on connectRadio()/setSliceFrequency() being separate seam
    // calls) -- a caller retunes immediately after a successful start if it
    // wants a specific frequency, the same connect-then-tune shape used
    // above this seam. Takes effect immediately (not on the next keepalive
    // tick) and is what the keepalive resends from then on.
    Q_INVOKABLE void setDdc0FrequencyHz(double hz);

    // Set one ADC's receive step attenuator (0 = ADC0, 1 = ADC1), clamped to
    // 0-31 dB by the encoder. Sent at once when running, and carried by every
    // keepalive from then on -- the High Priority packet is resent every
    // 100 ms, so a value held anywhere else would be overwritten by the next
    // tick. An out-of-range adcIndex is ignored.
    Q_INVOKABLE void setStepAttenuationDb(int adcIndex, int db);
    //DH1KLM: Change the selected Saturn RX antenna without restarting P2.
    Q_INVOKABLE void setRxAntenna(int antenna);

    // Hand over demodulated receiver audio for the radio's speaker: L,R,... int16 in
    // HOST order at kSpeakerSampleRateHz (buildSpeakerAudio() byte-swaps). One call
    // per DSP block (~20 ms); queueing and pacing run on this object's thread. No-op
    // unless started with speakerAudioEnabled.
    Q_INVOKABLE void enqueueSpeakerAudio(const QByteArray& interleavedInt16);

    // Status packets that reported a speaker-FIFO underflow (healthy = zero for the
    // session), and the last reported level in FIFO locations (see
    // HighPriorityStatus::speakerFifoLevel).
    [[nodiscard]] int speakerUnderflowReports() const noexcept
    {
        return m_speakerUnderflowReports;
    }
    [[nodiscard]] std::uint16_t lastSpeakerFifoLevel() const noexcept
    {
        return m_lastSpeakerFifoLevel;
    }

    // Change one DDC's rate on a LIVE session, no restart. Returns false (sending
    // nothing) if not running or ddcIndex is not enabled. An unchanged rate still
    // sends: the caller retries across the settle window. p2app supports this:
    // IncomingDDCSpecific.c loops on DDC-Specific packets and calls
    // WriteP2DDCRateRegister() (RegisterWrite(VADDRDDCRATES)) with no teardown. The
    // caller still rebuilds its WdspChannel. Resends the whole packet, since it
    // carries the enable bitmap and every DDC's row.
    Q_INVOKABLE bool setDdcRateLive(int ddcIndex, int rateKsps);

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }

    // ---- transport counters, for IRadioBackend::LinkStats ----
    //
    // I/O-THREAD ONLY, exactly like droppedPackets() above: every one of these
    // is written from handleDatagram()/the send paths on this object's own
    // thread, with no lock. The backend lives on another thread and must NOT
    // read them directly -- it consumes linkCountersUpdated() below, which is
    // delivered to its thread by the queued connection. The LinkStats contract
    // says the same thing: a backend with worker-side values caches them on its
    // own thread.
    //
    // Session-cumulative, reset by start(). Only the radio's own datagrams
    // count (handleDatagramFrom()). Bytes count every one of them, so a session
    // whose IQ has stalled while Status packets still arrive does not read as
    // a dead link. Packets count DDC frames only: drops counts gaps in that
    // stream, and RadioModel's loss percentage divides one by the other.
    struct LinkCounters {
        quint64 rxBytes = 0;
        quint64 rxPackets = 0;
        // Only bytes the socket ACCEPTED; see sendTo() for why a refused
        // datagram is not counted.
        quint64 txBytes = 0;
        quint64 drops = 0;
        // "ip:port" of the bound local socket, empty when not bound. The port
        // is the one the radio streams DDC0 to (see the class comment).
        QString localEndpoint;
    };

signals:
    void linkUp();       // first valid DDC0 frame seen
    void linkDown();     // stop() called, or a link failure once that exists
    // No DDC0 frame arrived within the connect-timeout window -- the radio
    // is off, unreachable, or the port never saw a genuine DDC0 frame (only
    // Mic Data / Status traffic, which onReadyRead() rejects and does not
    // count as a connection).
    void connectionError(const QString& reason);
    //DH1KLM: Logical RX0's decoded IQ (physical DDC2 on Orion MkII).
    // RX0 is the receiver every session has, and it is what linkUp()/the connect
    // timeout are keyed on. Emitted for ddcIndex 0 only; ddcIqReady() below
    // fires for the same block as well.
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    // Decoded IQ for ANY active DDC, demultiplexed by the datagram's sender
    // port (see P2Protocol::ddcIndexForSenderPort()). This is the general
    // form; a multi-receiver consumer routes on ddcIndex rather than
    // connecting per-DDC signals.
    void ddcIqReady(int ddcIndex, const std::vector<std::complex<float>>& block);
    void dropsUpdated(quint64 totalDrops);
    // A transport snapshot, on a FIXED cadence while running rather than when
    // traffic arrives -- the tick has to keep coming after the radio goes
    // quiet, because "nothing arrived this second" is what the heartbeat's
    // alarm path is waiting for (IRadioBackend::LinkStats). Emitted from the
    // I/O thread; cross-thread consumers get it queued.
    void linkCountersUpdated(const P2Client::LinkCounters& counters);
    // This DDC's next IQ block is discontinuous, including accepted rewinds
    // and duplicates. Emitted before ddcIqReady/ddc0IqReady in the same
    // handleDatagram() call so direct consumers can clear partial FFTs first.
    // Other DDCs have independent sequence counters and are unaffected.
    void ddcSequenceGap(int ddcIndex);
    // This session's own Discovery reply (see the class comment). At most once per
    // start(), unordered relative to linkUp(); AnanBackend reports capabilities from
    // it after profile validation (field meanings: P2Protocol.h's DiscoveryReply).
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 reportedReceivers);
    // One status packet's supply-rail reading, raw counts
    // (HighPriorityStatus::supplyRailCounts), once per packet: every 200 ms
    // while receiving. Unsmoothed; AnanBackend averages it before publishing.
    void supplyRailSampled(int counts);

private slots:
    void onReadyRead();
    void onKeepaliveTick();
    void onConnectTimeout();
    void onSpeakerDrainTick();

private:
    friend struct P2ClientTestAccess;
    // onReadyRead()'s socket-free half: drops anything not from m_host, so
    // foreign LAN traffic can neither hold LinkStats::alive true after the
    // radio has gone nor reach the parsers.
    void handleDatagramFrom(const QHostAddress& sender,
                            std::span<const std::uint8_t> bytes, quint16 senderPort);
    void handleDatagram(std::span<const std::uint8_t> bytes, quint16 senderPort);
    void noteSpeakerFifoStatus(const HighPriorityStatus& status);

    // p.8: a C&C packet must arrive at least every second (100 ms recommended) or a
    // radio in RUN drops to standby; buildGeneral() leaves that watchdog enabled.
    // Same cadence as anan/spike/phase1a.py's CC_KEEPALIVE_INTERVAL.
    static constexpr int kKeepaliveMs = 100;
    // No packet ever arrived from a genuine DDC0 frame within this long of
    // start() -- mirrors MetisClient's kConnectTimeoutMs.
    static constexpr int kConnectTimeoutMs = 2000;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_keepaliveTimer = nullptr;
    // Releases queued speaker packets. The rate is set by how many packets the
    // pacer releases, not by how often this fires -- but the two are coupled
    // through the pacer's target, which has to be big enough to cover several of
    // these ticks. See SpeakerAudioPacer::kTargetFifoFrames: a 5 ms tick drains
    // 240 frames from the radio, and a target that could not hold more than that
    // made the radio's audio unintelligible.
    QTimer* m_speakerDrainTimer = nullptr;
    static constexpr int kSpeakerDrainMs = 5;
    // Queued whole packets' worth of samples, oldest first. Bounded: audio the
    // radio cannot take is dropped rather than accumulated, because unbounded
    // queueing of a realtime stream converts a transient stall into permanent
    // latency that never recovers.
    static constexpr int kMaxQueuedSpeakerPackets = 64;  // ~85 ms
    std::vector<std::int16_t> m_speakerPending;
    SpeakerAudioPacer m_speakerPacer;
    std::uint32_t m_speakerSequence = 0;
    QElapsedTimer m_speakerClock;
    bool m_speakerAudioEnabled = false;
    // Logged once per session, not per drop: this fires in the audio path.
    bool m_speakerOverflowLogged = false;
    int m_speakerUnderflowReports = 0;
    std::uint16_t m_lastSpeakerFifoLevel = 0;
    QTimer* m_connectTimeoutTimer = nullptr;
    // Whatever start() was actually called with -- onConnectTimeout()'s
    // message reports this, not kConnectTimeoutMs, so the number an operator
    // sees always matches the window that was really used.
    int m_activeConnectTimeoutMs = kConnectTimeoutMs;
    // Guards discoveryInfoReceived() to at most once per start() -- a stray
    // reply to someone ELSE's broadcast discovery landing on this socket
    // (unlikely, but this parses unauthenticated UDP) must not re-fire it
    // repeatedly for the life of the session.
    bool m_discoveryInfoSent = false;
    //DH1KLM: No profile packets are sent until this session's reply
    // identifies an implemented RX profile and matches any selected board ID.
    bool m_discoveryValidated = false;
    int m_expectedBoardId = -1;
    //DH1KLM: Stream indices stay logical; packet rows/ports use this RX offset.
    int m_firstDdcIndex = 0;
    bool m_selectReceiveLpf = false;
    ReceiveFilterKind m_receiveFilterKind = ReceiveFilterKind::MkIIBpf;
    bool m_swapSpeakerChannels = false;

    QHostAddress m_host;
    std::uint32_t m_ddc0FreqWord = 0;   // last value sent; the keepalive resends this
    // Set once from Params at start(); every buildHighPriority() call for
    // the rest of this session (the keepalive tick included) carries these
    // same values, since there is no live setter for either (see Params'
    // own comment on why these are connect-time-only).
    bool m_bypassAdc0Filters = false; //DH1KLM: use Thetis Saturn BPF1 by default
    bool m_bypassAdc1Filters = true;
    // Step attenuation per ADC, seeded from Params at start() and changed
    // live by setStepAttenuationDb(); every buildHighPriority() carries it.
    int m_adc0AttenuationDb = 0;
    int m_adc1AttenuationDb = 0;
    int m_rxAntenna = 1;
    int m_numAdcs = 2; //DH1KLM: board profile; defaults to the currently supported Saturn family.

    // The session's resolved DDC list and the dither/random flags that went
    // with it, RETAINED (rather than consumed and dropped in start()) so
    // setDdcRateLive() can rebuild a complete DDC-Specific packet: that
    // packet carries every DDC's row plus the enable bitmap, so resending it
    // with only the changed rate known would disable every other DDC.
    std::vector<DdcConfig> m_activeDdcs;
    bool m_ditherEnabled = true;
    bool m_randomEnabled = true;

    bool m_running = false;
    bool m_linkUp = false;

    // Sequence-gap detection is PER DDC: every DDC runs its own independent
    // sequence counter on its own socket, so a single shared "expected next"
    // would report a gap on every alternating frame the moment a second DDC
    // started streaming -- a fault indication manufactured entirely by the
    // client. Indexed by ddcIndex; nullopt until that DDC's first frame.
    std::array<std::optional<std::uint32_t>, kMaxDdcs> m_expectedSeq{};
    // Drops stay a single session-wide counter (what dropsUpdated() has
    // always reported) -- an operator watching for a lossy link cares that
    // the session is dropping, not which receiver.
    quint64 m_drops = 0;
    // What each counts, and why they are I/O-thread-only, is on LinkCounters.
    quint64 m_rxBytes = 0;
    quint64 m_rxPackets = 0;
    quint64 m_txBytes = 0;
    // Matches the status bar's own refresh rate. Faster would publish snapshots
    // no one reads; slower would delay the heartbeat's silence verdict.
    static constexpr int kLinkCountersMs = 1000;
    QTimer* m_linkCountersTimer = nullptr;
    void publishLinkCounters();
    [[nodiscard]] QString localEndpoint() const;

    // How many DDCs this session enabled, so onReadyRead() knows which
    // sender ports belong to it. Kept alongside m_activeDdcs (rather than
    // read from its size) because onReadyRead() consults it per datagram.
    int m_activeDdcCount = 1;

    // One shared phase word per active DDC. Every High Priority sender uses
    // this, so the keepalive (100 ms) can never contradict what start() sent:
    // a single-DDC overload on that path would pin DDC1..N-1 at word 0
    // regardless. Per-DDC tuning has no setter yet, so one word for all of
    // them is the honest encoding -- see start()'s own comment.
    [[nodiscard]] std::vector<std::uint32_t> sharedFreqWords() const
    {
        return std::vector<std::uint32_t>(
            static_cast<std::size_t>(m_activeDdcCount), m_ddc0FreqWord);
    }
    // Sender ports already warned about, so a port mismatch -- which by
    // nature affects every packet -- logs once per distinct port rather than
    // per datagram. Session-scoped; cleared with the rest of the state.
    QSet<quint16> m_warnedUnexpectedPorts;
    // One warning per session, not per sender: a set keyed on the source
    // address would grow without bound under a flood of spoofed ones.
    bool m_warnedForeignSender = false;

    // Reused decode buffer, cleared and refilled per frame rather than
    // reallocated -- matches MetisClient's m_blocks for the same reason.
    // Shared across DDCs deliberately: onReadyRead() fully consumes each
    // block (the emit is a same-thread direct connection) before touching
    // the next datagram, so there is never more than one live at a time.
    std::vector<std::complex<float>> m_decodeScratch;
};

}  // namespace AetherSDR::anan

// Not a seam payload (IRadioBackend.h rule 4 covers those): this one stays
// inside the ANAN backend, client -> backend. Declared anyway because Qt 6's
// function-pointer queued delivery works without it but the NAME-based paths do
// not -- QSignalSpy in the tests being the one that would otherwise capture
// nothing and pass vacuously.
Q_DECLARE_METATYPE(AetherSDR::anan::P2Client::LinkCounters)
