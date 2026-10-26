#pragma once

#include "core/backends/anan_original/P2Protocol.h"
#include "core/backends/anan_original/AnanSpeakerPacing.h"

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

namespace AetherSDR::anan_original {

//DH1KLM Owns the ANAN-G2 Protocol 2 UDP wire: session setup (General + DDC-Specific +
//DH1KLM High Priority with run=1), the keepalive that holds the radio in RUN, and DDC
//DH1KLM IQ ingest. Lives on AnanBackend's I/O thread (like MetisClient for HL2) so a
//DH1KLM GUI stall cannot starve the radio's watchdog.
//DH1KLM RX-ONLY: no PTT exists here or in buildHighPriority() (TX is RFC §2.11 Phase 3).
//DH1KLM The caller resolves the host (AnanDiscovery or a manual probe), but start()
//DH1KLM still sends its own Discovery first: every radio->PC stream goes to "the Source
//DH1KLM Port of the Host that initiated the Discovery Packet" (spec p.43, p.51, p.54),
//DH1KLM so it must come from THIS socket. The reply is not awaited; if it arrives it is
//DH1KLM parsed for gateware version, DDC count and board id (discoveryInfoReceived()).
class P2Client : public QObject {
    Q_OBJECT

public:
    explicit P2Client(QObject* parent = nullptr);
    ~P2Client() override;

    struct Params {
        QString host;
        //DH1KLM Valid rates: 48/96/192/384/768/1536 (spec p.24). Not validated here
        //DH1KLM -- P2Protocol::buildDdcSpecific() is the single place that would
        //DH1KLM reject or clamp one, and it does neither yet; an invalid value is
        //DH1KLM simply sent as-is and the radio's own reaction is the feedback.
        int ddc0RateKsps = 48;
        //DH1KLM Connect-time-only options: no live setter; a change takes a reconnect.
        bool ditherEnabled = true;
        bool randomEnabled = true;
        int ddc0AdcIndex = 0;          // 0 = ADC0, 1 = ADC1/RX2
        bool bypassAdc0Filters = true;
        bool bypassAdc1Filters = true;
        //DH1KLM Receive step attenuators, 0-31 dB (buildHighPriority()). Unlike the
        //DH1KLM options above these ARE live: setStepAttenuationDb() changes them
        //DH1KLM mid-session. The values here are what the session starts with.
        int adc0AttenuationDb = 0;
        int adc1AttenuationDb = 0;

        //DH1KLM Send demodulated RX audio to the radio's own speaker (DDC Audio,
        //DH1KLM kSpeakerAudioPort). DEFAULT OFF: it is the one option that originates a
        //DH1KLM continuous outbound stream (750 packets/s, ~260 kB/s).
        bool speakerAudioEnabled = false;

        //DH1KLM Live, like the attenuators: setBandFiltersEnabled()/setSpeakerAmpMuted()
        //DH1KLM change them mid-session, and these are what the session starts with.
        //DH1KLM Select the receive band-pass filter for the tuned frequency on both ADCs
        //DH1KLM instead of holding the banks bypassed (alexRxFilterFor()).
        bool bandFiltersEnabled = false;
        //DH1KLM Mute the radio's speaker amplifier (byte 1400 bit 1).
        bool speakerAmpMuted = false;

        //DH1KLM Multi-DDC session. EMPTY (default) = single DDC0 built from ddc0RateKsps/
        //DH1KLM ddc0AdcIndex. Non-empty is authoritative and those two fields are ignored;
        //DH1KLM entry n configures DDC n, capped at kMaxDdcs by the packet builders.
        std::vector<DdcConfig> activeDdcs;
    };

    //DH1KLM start()/stop()/setters MUST run on this object's thread: start() creates the
    //DH1KLM QUdpSocket, which takes that thread's affinity. connectTimeoutMs exceeds the
    //DH1KLM default for a rate-change restart, where the radio is still re-settling from
    //DH1KLM the stop (AnanBackend::kRateChangeConnectTimeoutMs).
    Q_INVOKABLE bool start(const Params& params, int connectTimeoutMs = kConnectTimeoutMs);
    Q_INVOKABLE void stop();

    //DH1KLM Retune DDC0. No frequency is set by start() itself (see the class
    //DH1KLM comment on connectRadio()/setSliceFrequency() being separate seam
    //DH1KLM calls) -- a caller retunes immediately after a successful start if it
    //DH1KLM wants a specific frequency, the same connect-then-tune shape used
    //DH1KLM above this seam. Takes effect immediately (not on the next keepalive
    //DH1KLM tick) and is what the keepalive resends from then on.
    Q_INVOKABLE void setDdc0FrequencyHz(double hz);

    //DH1KLM Set one ADC's receive step attenuator (0 = ADC0, 1 = ADC1), clamped to
    //DH1KLM 0-31 dB by the encoder. Sent at once when running, and carried by every
    //DH1KLM keepalive from then on -- the High Priority packet is resent every
    //DH1KLM 100 ms, so a value held anywhere else would be overwritten by the next
    //DH1KLM tick. An out-of-range adcIndex is ignored.
    Q_INVOKABLE void setStepAttenuationDb(int adcIndex, int db);

    //DH1KLM Select the band-pass filter for the tuned frequency on both ADCs (true),
    //DH1KLM or leave the banks to the bypass flags (false). Same cadence contract as
    //DH1KLM setStepAttenuationDb(): sent at once when running, then by every keepalive.
    Q_INVOKABLE void setBandFiltersEnabled(bool on);

    //DH1KLM Mute or unmute the radio's speaker amplifier. Same cadence contract.
    Q_INVOKABLE void setSpeakerAmpMuted(bool on);

    //DH1KLM Hand over demodulated receiver audio for the radio's speaker: L,R,... int16 in
    //DH1KLM HOST order at kSpeakerSampleRateHz (buildSpeakerAudio() byte-swaps). One call
    //DH1KLM per DSP block (~20 ms); queueing and pacing run on this object's thread. No-op
    //DH1KLM unless started with speakerAudioEnabled.
    Q_INVOKABLE void enqueueSpeakerAudio(const QByteArray& interleavedInt16);

    //DH1KLM Status packets that reported a speaker-FIFO underflow (healthy = zero for the
    //DH1KLM session), and the last reported level in FIFO locations (see
    //DH1KLM HighPriorityStatus::speakerFifoLevel).
    [[nodiscard]] int speakerUnderflowReports() const noexcept
    {
        return m_speakerUnderflowReports;
    }
    [[nodiscard]] std::uint16_t lastSpeakerFifoLevel() const noexcept
    {
        return m_lastSpeakerFifoLevel;
    }

    //DH1KLM Change one DDC's rate on a LIVE session, no restart. Returns false (sending
    //DH1KLM nothing) if not running or ddcIndex is not enabled. An unchanged rate still
    //DH1KLM sends: the caller retries across the settle window. p2app supports this:
    //DH1KLM IncomingDDCSpecific.c loops on DDC-Specific packets and calls
    //DH1KLM WriteP2DDCRateRegister() (RegisterWrite(VADDRDDCRATES)) with no teardown. The
    //DH1KLM caller still rebuilds its WdspChannel. Resends the whole packet, since it
    //DH1KLM carries the enable bitmap and every DDC's row.
    Q_INVOKABLE bool setDdcRateLive(int ddcIndex, int rateKsps);

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }

    //DH1KLM ---- transport counters, for IRadioBackend::LinkStats ----
    //DH1KLM
    //DH1KLM I/O-THREAD ONLY, exactly like droppedPackets() above: every one of these
    //DH1KLM is written from handleDatagram()/the send paths on this object's own
    //DH1KLM thread, with no lock. The backend lives on another thread and must NOT
    //DH1KLM read them directly -- it consumes linkCountersUpdated() below, which is
    //DH1KLM delivered to its thread by the queued connection. The LinkStats contract
    //DH1KLM says the same thing: a backend with worker-side values caches them on its
    //DH1KLM own thread.
    //DH1KLM
    //DH1KLM Session-cumulative, reset by start(). Only the radio's own datagrams
    //DH1KLM count (handleDatagramFrom()). Bytes count every one of them, so a session
    //DH1KLM whose IQ has stalled while Status packets still arrive does not read as
    //DH1KLM a dead link. Packets count DDC frames only: drops counts gaps in that
    //DH1KLM stream, and RadioModel's loss percentage divides one by the other.
    struct LinkCounters {
        quint64 rxBytes = 0;
        quint64 rxPackets = 0;
        //DH1KLM Only bytes the socket ACCEPTED; see sendTo() for why a refused
        //DH1KLM datagram is not counted.
        quint64 txBytes = 0;
        quint64 drops = 0;
        //DH1KLM "ip:port" of the bound local socket, empty when not bound. The port
        //DH1KLM is the one the radio streams DDC0 to (see the class comment).
        QString localEndpoint;
    };

signals:
    void linkUp();       // first valid DDC0 frame seen
    void linkDown();     // stop() called, or a link failure once that exists
    //DH1KLM No DDC0 frame arrived within the connect-timeout window -- the radio
    //DH1KLM is off, unreachable, or the port never saw a genuine DDC0 frame (only
    //DH1KLM Mic Data / Status traffic, which onReadyRead() rejects and does not
    //DH1KLM count as a connection).
    void connectionError(const QString& reason);
    //DH1KLM DDC0's decoded IQ. Kept as its own signal because DDC0 is the one
    //DH1KLM receiver every session always has, and it is what linkUp()/the connect
    //DH1KLM timeout are keyed on. Emitted for ddcIndex 0 only; ddcIqReady() below
    //DH1KLM fires for the same block as well.
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    //DH1KLM Decoded IQ for ANY active DDC, demultiplexed by the datagram's sender
    //DH1KLM port (see P2Protocol::ddcIndexForSenderPort()). This is the general
    //DH1KLM form; a multi-receiver consumer routes on ddcIndex rather than
    //DH1KLM connecting per-DDC signals.
    void ddcIqReady(int ddcIndex, const std::vector<std::complex<float>>& block);
    void dropsUpdated(quint64 totalDrops);
    //DH1KLM A transport snapshot, on a FIXED cadence while running rather than when
    //DH1KLM traffic arrives -- the tick has to keep coming after the radio goes
    //DH1KLM quiet, because "nothing arrived this second" is what the heartbeat's
    //DH1KLM alarm path is waiting for (IRadioBackend::LinkStats). Emitted from the
    //DH1KLM I/O thread; cross-thread consumers get it queued.
    void linkCountersUpdated(const P2Client::LinkCounters& counters);
    //DH1KLM This DDC's next IQ block is discontinuous, including accepted rewinds
    //DH1KLM and duplicates. Emitted before ddcIqReady/ddc0IqReady in the same
    //DH1KLM handleDatagram() call so direct consumers can clear partial FFTs first.
    //DH1KLM Other DDCs have independent sequence counters and are unaffected.
    void ddcSequenceGap(int ddcIndex);
    //DH1KLM This session's own Discovery reply (see the class comment). At most once per
    //DH1KLM start(), unordered relative to linkUp(); AnanBackend reports capabilities from
    //DH1KLM it (field meanings: P2Protocol.h's DiscoveryReply).
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 numDdc);
    //DH1KLM One status packet's supply-rail reading, raw counts
    //DH1KLM (HighPriorityStatus::supplyRailCounts), once per packet: every 200 ms
    //DH1KLM while receiving. Unsmoothed; AnanBackend averages it before publishing.
    void supplyRailSampled(int counts);

private slots:
    void onReadyRead();
    void onKeepaliveTick();
    void onConnectTimeout();
    void onSpeakerDrainTick();

private:
    friend struct P2ClientTestAccess;
    //DH1KLM onReadyRead()'s socket-free half: drops anything not from m_host, so
    //DH1KLM foreign LAN traffic can neither hold LinkStats::alive true after the
    //DH1KLM radio has gone nor reach the parsers.
    void handleDatagramFrom(const QHostAddress& sender,
                            std::span<const std::uint8_t> bytes, quint16 senderPort);
    void handleDatagram(std::span<const std::uint8_t> bytes, quint16 senderPort);
    void noteSpeakerFifoStatus(const HighPriorityStatus& status);

    //DH1KLM p.8: a C&C packet must arrive at least every second (100 ms recommended) or a
    //DH1KLM radio in RUN drops to standby; buildGeneral() leaves that watchdog enabled.
    //DH1KLM Same cadence as anan/spike/phase1a.py's CC_KEEPALIVE_INTERVAL.
    static constexpr int kKeepaliveMs = 100;
    //DH1KLM No packet ever arrived from a genuine DDC0 frame within this long of
    //DH1KLM start() -- mirrors MetisClient's kConnectTimeoutMs.
    static constexpr int kConnectTimeoutMs = 2000;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_keepaliveTimer = nullptr;
    //DH1KLM Releases queued speaker packets. The rate is set by how many packets the
    //DH1KLM pacer releases, not by how often this fires -- but the two are coupled
    //DH1KLM through the pacer's target, which has to be big enough to cover several of
    //DH1KLM these ticks. See SpeakerAudioPacer::kTargetFifoFrames: a 5 ms tick drains
    //DH1KLM 240 frames from the radio, and a target that could not hold more than that
    //DH1KLM made the radio's audio unintelligible.
    QTimer* m_speakerDrainTimer = nullptr;
    static constexpr int kSpeakerDrainMs = 5;
    //DH1KLM Queued whole packets' worth of samples, oldest first. Bounded: audio the
    //DH1KLM radio cannot take is dropped rather than accumulated, because unbounded
    //DH1KLM queueing of a realtime stream converts a transient stall into permanent
    //DH1KLM latency that never recovers.
    static constexpr int kMaxQueuedSpeakerPackets = 64;  // ~85 ms
    std::vector<std::int16_t> m_speakerPending;
    SpeakerAudioPacer m_speakerPacer;
    std::uint32_t m_speakerSequence = 0;
    QElapsedTimer m_speakerClock;
    bool m_speakerAudioEnabled = false;
    //DH1KLM Logged once per session, not per drop: this fires in the audio path.
    bool m_speakerOverflowLogged = false;
    int m_speakerUnderflowReports = 0;
    std::uint16_t m_lastSpeakerFifoLevel = 0;
    QTimer* m_connectTimeoutTimer = nullptr;
    //DH1KLM Whatever start() was actually called with -- onConnectTimeout()'s
    //DH1KLM message reports this, not kConnectTimeoutMs, so the number an operator
    //DH1KLM sees always matches the window that was really used.
    int m_activeConnectTimeoutMs = kConnectTimeoutMs;
    //DH1KLM Guards discoveryInfoReceived() to at most once per start() -- a stray
    //DH1KLM reply to someone ELSE's broadcast discovery landing on this socket
    //DH1KLM (unlikely, but this parses unauthenticated UDP) must not re-fire it
    //DH1KLM repeatedly for the life of the session.
    bool m_discoveryInfoSent = false;

    QHostAddress m_host;
    std::uint32_t m_ddc0FreqWord = 0;   // last value sent; the keepalive resends this
    //DH1KLM Set once from Params at start(), since there is no live setter for
    //DH1KLM either (see Params' own comment on why these are connect-time-only).
    //DH1KLM While m_bandFiltersEnabled is set, a run=1 packet's band filter
    //DH1KLM replaces them (highPriorityOptions()); the stop packet always uses them.
    bool m_bypassAdc0Filters = true;
    bool m_bypassAdc1Filters = true;
    //DH1KLM Step attenuation per ADC, seeded from Params at start() and changed
    //DH1KLM live by setStepAttenuationDb(); every buildHighPriority() carries it.
    int m_adc0AttenuationDb = 0;
    int m_adc1AttenuationDb = 0;
    //DH1KLM The two live options, seeded from Params at start() and changed by their
    //DH1KLM setters. m_ddc0FreqHz moves with m_ddc0FreqWord: the filter is chosen in
    //DH1KLM hertz, which the phase word cannot give back without the sample clock.
    bool m_bandFiltersEnabled = false;
    bool m_speakerAmpMuted = false;
    double m_ddc0FreqHz = 0.0;

    //DH1KLM The session's resolved DDC list and the dither/random flags that went
    //DH1KLM with it, RETAINED (rather than consumed and dropped in start()) so
    //DH1KLM setDdcRateLive() can rebuild a complete DDC-Specific packet: that
    //DH1KLM packet carries every DDC's row plus the enable bitmap, so resending it
    //DH1KLM with only the changed rate known would disable every other DDC.
    std::vector<DdcConfig> m_activeDdcs;
    bool m_ditherEnabled = true;
    bool m_randomEnabled = true;

    bool m_running = false;
    bool m_linkUp = false;

    //DH1KLM Sequence-gap detection is PER DDC: every DDC runs its own independent
    //DH1KLM sequence counter on its own socket, so a single shared "expected next"
    //DH1KLM would report a gap on every alternating frame the moment a second DDC
    //DH1KLM started streaming -- a fault indication manufactured entirely by the
    //DH1KLM client. Indexed by ddcIndex; nullopt until that DDC's first frame.
    std::array<std::optional<std::uint32_t>, kMaxDdcs> m_expectedSeq{};
    //DH1KLM Drops stay a single session-wide counter (what dropsUpdated() has
    //DH1KLM always reported) -- an operator watching for a lossy link cares that
    //DH1KLM the session is dropping, not which receiver.
    quint64 m_drops = 0;
    //DH1KLM What each counts, and why they are I/O-thread-only, is on LinkCounters.
    quint64 m_rxBytes = 0;
    quint64 m_rxPackets = 0;
    quint64 m_txBytes = 0;
    //DH1KLM Matches the status bar's own refresh rate. Faster would publish snapshots
    //DH1KLM no one reads; slower would delay the heartbeat's silence verdict.
    static constexpr int kLinkCountersMs = 1000;
    QTimer* m_linkCountersTimer = nullptr;
    void publishLinkCounters();
    [[nodiscard]] QString localEndpoint() const;

    //DH1KLM How many DDCs this session enabled, so onReadyRead() knows which
    //DH1KLM sender ports belong to it. Kept alongside m_activeDdcs (rather than
    //DH1KLM read from its size) because onReadyRead() consults it per datagram.
    int m_activeDdcCount = 1;

    //DH1KLM One shared phase word per active DDC. Every High Priority sender uses
    //DH1KLM this, so the keepalive (100 ms) can never contradict what start() sent:
    //DH1KLM a single-DDC overload on that path would pin DDC1..N-1 at word 0
    //DH1KLM regardless. Per-DDC tuning has no setter yet, so one word for all of
    //DH1KLM them is the honest encoding -- see start()'s own comment.
    [[nodiscard]] std::vector<std::uint32_t> sharedFreqWords() const
    {
        return std::vector<std::uint32_t>(
            static_cast<std::size_t>(m_activeDdcCount), m_ddc0FreqWord);
    }

    //DH1KLM The live options every run=1 High Priority packet carries, built from the
    //DH1KLM members above in one place so the four senders (start, retune,
    //DH1KLM attenuator, keepalive) cannot disagree.
    [[nodiscard]] HighPriorityOptions highPriorityOptions() const;

    //DH1KLM Send one run=1 High Priority packet from the current members. Callers
    //DH1KLM check m_running and m_socket.
    void sendRunningHighPriority();
    //DH1KLM Sender ports already warned about, so a port mismatch -- which by
    //DH1KLM nature affects every packet -- logs once per distinct port rather than
    //DH1KLM per datagram. Session-scoped; cleared with the rest of the state.
    QSet<quint16> m_warnedUnexpectedPorts;
    //DH1KLM One warning per session, not per sender: a set keyed on the source
    //DH1KLM address would grow without bound under a flood of spoofed ones.
    bool m_warnedForeignSender = false;

    //DH1KLM Reused decode buffer, cleared and refilled per frame rather than
    //DH1KLM reallocated -- matches MetisClient's m_blocks for the same reason.
    //DH1KLM Shared across DDCs deliberately: onReadyRead() fully consumes each
    //DH1KLM block (the emit is a same-thread direct connection) before touching
    //DH1KLM the next datagram, so there is never more than one live at a time.
    std::vector<std::complex<float>> m_decodeScratch;
};

}  // namespace AetherSDR::anan_original

//DH1KLM Not a seam payload (IRadioBackend.h rule 4 covers those): this one stays
//DH1KLM inside the ANAN backend, client -> backend. Declared anyway because Qt 6's
//DH1KLM function-pointer queued delivery works without it but the NAME-based paths do
//DH1KLM not -- QSignalSpy in the tests being the one that would otherwise capture
//DH1KLM nothing and pass vacuously.
Q_DECLARE_METATYPE(AetherSDR::anan_original::P2Client::LinkCounters)
