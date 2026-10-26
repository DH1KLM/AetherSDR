#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/anan_original/AnanRxDsp.h"
#include "core/backends/anan_original/AnanSliceAudio.h"
#include "core/Resampler.h"
#include "core/backends/anan_original/AnanDroopCalibrator.h"
#include "core/backends/anan_original/P2Client.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QMap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR::anan_original {

//DH1KLM IRadioBackend for the ANAN-G2 (openHPSDR Protocol 2). Owns one P2Client and
//DH1KLM one AnanRxDsp on a dedicated I/O thread, like Hl2Backend with MetisClient +
//DH1KLM Hl2RxDsp. Scope: one DDC, RX only.
//DH1KLM   - No NCO/WDSP-shift split: DDC0 retunes on every setSliceFrequency()/
//DH1KLM     setPanCenter(), so "the shift is ALWAYS exactly -cwBfoHz(mode), because
//DH1KLM     the NCO IS the slice frequency".
//DH1KLM   - capabilities()' identity strings are fixed; gateware, DDC count and board
//DH1KLM     id come from this session's Discovery reply once it lands.
//DH1KLM   - setKeying() is a no-op: canTransmit is false and P2Client has no PTT
//DH1KLM     (TX is RFC §2.11 Phase 3).
//DH1KLM Receive handedness lives entirely in AnanRxDsp (see docs/HERMES.md §16).
class AnanBackend : public IRadioBackend {
    Q_OBJECT

public:
    //DH1KLM Public for testing. The seeded defaults are reported here alongside a
    //DH1KLM sweep's measured tables, so the one invariant worth pinning is that a
    //DH1KLM rate claims NO correction until connectRadio() has actually seeded one
    //DH1KLM -- reporting a default as live on a disconnected radio would be the
    //DH1KLM same lie in the other direction as the "nothing measured" this replaced.
    //DH1KLM See anan_backend_test.
    [[nodiscard]] QVariantMap droopStatus() const;

    explicit AnanBackend(QObject* parent = nullptr);
    ~AnanBackend() override;

    RadioCapabilities capabilities() const override;
    bool ownsRxAudio() const override { return true; }

    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    //DH1KLM The AGC-off level, which the base class drops. Mode and threshold edits
    //DH1KLM go to setSliceAgc() as before.
    void requestSliceAgc(int sliceId, const SliceAgcRequest& request) override;
    void setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                              int level, AetherSDR::NoiseBlankerFill fill) override;
    ReceiveDispatch requestSliceDsp(int sliceId, const SliceDspRequest& request) override;
    ReceiveDispatch requestSliceAudio(int sliceId, const SliceAudioRequest& request) override;

    //DH1KLM The receiver's own audio stage. Without these three the operator's mute,
    //DH1KLM AF fader and balance moved and nothing happened: IRadioBackend's defaults
    //DH1KLM are no-ops, and a backend that demodulates on this host is the only thing
    //DH1KLM in the chain that can apply them (see AnanSliceAudio.h).
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;

    //DH1KLM The RADIO's own output level and mute, applied to the speaker stream only.
    //DH1KLM
    //DH1KLM Separate from the per-slice stage above, and both apply: the slice stage is
    //DH1KLM how loud this receiver is wherever it is heard, this is how loud the RADIO
    //DH1KLM plays. Without it the radio gets the samples at whatever the slice stage
    //DH1KLM left them -- full scale by default -- and the G2 has no speaker volume
    //DH1KLM register to turn that down with.
    void setLineoutGain(int percent) override;
    void setLineoutMute(bool mute) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    //DH1KLM The G2's receive step attenuator, presented as RF gain: -31..0 dB,
    //DH1KLM where -12 means 12 dB of attenuation. See the definition.
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;
    void setPanPixelWidth(const QString& panId, int pixels) override;
    void setCwPitch(int hz) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;
    //DH1KLM Transport snapshot for the status bar's Network field and Network
    //DH1KLM Diagnostics. Synchronous, and reads only the cache onLinkCounters() keeps
    //DH1KLM on this thread -- never the client's own counters, which belong to the
    //DH1KLM I/O thread. The push half is onLinkCounters(), on the client's tick.
    [[nodiscard]] LinkStats linkStats() const override;

    //DH1KLM ---- pure-function pieces, exposed static for testability ----
    //DH1KLM (matches the WdspChannel/Hl2RxDsp precedent of exposing normally-
    //DH1KLM internal helpers specifically so a test can pin them directly, rather
    //DH1KLM than only indirectly through the class's live behaviour.)

    //DH1KLM Mode string -> WdspChannel::Mode. Unknown strings fall back to USB
    //DH1KLM (matches Hl2Backend's own modeFromString fallback) -- HERMES.md §16.7's
    //DH1KLM own regression: "CW" (not just "CWU") and "NFM" both need entries or a
    //DH1KLM radio reporting either falls through silently and demodulates as SSB.
    [[nodiscard]] static WdspChannel::Mode modeFromString(const QString& mode) noexcept;

    //DH1KLM Per-mode default passband, operator-facing (marker-relative) Hz.
    //DH1KLM Vocabulary matches modeFromString's.
    [[nodiscard]] static std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept;

    //DH1KLM Every spelling modeFromString() maps, case-insensitive. The restore needs
    //DH1KLM it because modeFromString() falls back to USB instead of failing.
    [[nodiscard]] static bool isKnownModeString(const QString& mode) noexcept;

    //DH1KLM Where a session starts when this radio has no remembered state.
    static constexpr double kFirstConnectFrequencyHz = 14'175'000.0;
    static constexpr const char* kFirstConnectMode = "USB";

    //DH1KLM Bounded by the DDS encoding, not a band edge: tuningMinHz/MaxHz are not
    //DH1KLM reported. Refused, never clamped, at or above Nyquist.
    [[nodiscard]] static bool isRestorableFrequencyHz(double hz) noexcept;

    //DH1KLM The CW BFO offset (HERMES.md §5: "CW has no BFO unless you build one").
    //DH1KLM +pitchHz for CWU/CW, -pitchHz for CWL, 0 for every other mode -- zero
    //DH1KLM for non-CW is why every mode routes through this rather than only the
    //DH1KLM CW ones (entering/leaving CW must re-push the shift either way).
    [[nodiscard]] static double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept;

    //DH1KLM Snaps an operator-requested span to the nearest rate this radio
    //DH1KLM actually offers (capabilities().sampleRatesHz), by RATIO (log
    //DH1KLM distance), not linear -- see the definition comment for why linear
    //DH1KLM distance is provably wrong for these octave-spaced rates
    //DH1KLM (HERMES.md §15.1; mirrors Hl2Backend::nearestIqSampleRateHz()).
    //DH1KLM There is no continuous zoom here -- DDC0 runs at exactly one of six
    //DH1KLM fixed rates.
    [[nodiscard]] static int nearestDdc0RateKsps(int requestedKsps) noexcept;

    //DH1KLM The AGC-T knob (0..100) as WDSP gain in dB: the AGC's maximum gain, or
    //DH1KLM the fixed gain with AGC off. -20..120 dB is the range of deskHPSDR's AGC
    //DH1KLM gain slider and of Thetis's AGC-T, which sets both gains the same way.
    //DH1KLM With AGC on, the AGC reaches its maximum only on weak signals, so a high
    //DH1KLM setting cannot clip a strong one. With AGC off, the knob is the gain.
    static constexpr double kAgcKnobMinDb = -20.0;
    static constexpr double kAgcKnobDbPerStep = 1.4;
    [[nodiscard]] static constexpr double agcKnobDb(int level) noexcept
    {
        return kAgcKnobMinDb + kAgcKnobDbPerStep * std::clamp(level, 0, 100);
    }
    //DH1KLM 71 = 79.4 dB, the step nearest deskHPSDR's 80 dB AGC gain default.
    static constexpr int kDefaultAgcThreshold = 71;
    //DH1KLM 29 = 20.6 dB, the step nearest Thetis's 20 dB fixed gain default
    //DH1KLM (deskHPSDR has no AGC-off gain control).
    static constexpr int kDefaultAgcOffLevel = 29;

    //DH1KLM Live operator AGC state as setSliceAgc()/requestSliceAgc() last stored
    //DH1KLM it, which connectRadio() and beginRateChange() build the DSP config
    //DH1KLM from. Exposed read-only for testing without a live radio (matches
    //DH1KLM WdspChannel's own *ForTest accessor convention), not part of the
    //DH1KLM operator-facing seam.
    [[nodiscard]] int agcModeForTest() const noexcept { return m_agcMode; }
    [[nodiscard]] double agcMaxGainDbForTest() const noexcept { return agcKnobDb(m_agcThreshold); }
    [[nodiscard]] double agcFixedGainDbForTest() const noexcept { return agcKnobDb(m_agcOffLevel); }
    //DH1KLM The live receive state as applyLiveRxState() writes it into the DSP config.
    [[nodiscard]] AnanRxDsp::Config liveRxConfigForTest() const
    {
        AnanRxDsp::Config config;
        applyLiveRxState(config);
        return config;
    }
    [[nodiscard]] int attenuationDbForTest() const noexcept { return m_attenuationDb; }
    [[nodiscard]] bool noiseBlankerOnForTest() const noexcept
    {
        return m_nbKind != AetherSDR::NoiseBlankerKind::Off;
    }
    [[nodiscard]] AetherSDR::NoiseBlankerKind noiseBlankerKindForTest() const noexcept
    {
        return m_nbKind;
    }
    [[nodiscard]] int noiseBlankerLevelForTest() const noexcept { return m_nbLevel; }
    [[nodiscard]] AetherSDR::NoiseBlankerFill noiseBlankerFillForTest() const noexcept
    {
        return m_nbFill;
    }
    //DH1KLM The radio's own output level/mute as the lineout seam last set them. Same
    //DH1KLM *ForTest convention as the accessors above: read-only, not part of the seam.
    [[nodiscard]] int lineoutGainPercentForTest() const noexcept { return m_lineoutGainPercent; }
    [[nodiscard]] bool lineoutMutedForTest() const noexcept { return m_lineoutMuted; }
    //DH1KLM What applyRestoredState() accepted; connectRadio() consumes it behind a socket.
    [[nodiscard]] double restoredFrequencyHzForTest() const noexcept { return m_restoredFreqHz; }
    [[nodiscard]] QString restoredModeForTest() const { return m_restoredMode; }
    //DH1KLM Drives the S-meter path as AnanRxDsp::meterUpdate would, so the
    //DH1KLM smoothing and publish tick can be tested without a live radio.
    void feedMeterForTest(float dbfs) { onDspMeter(dbfs); }
    //DH1KLM The smoothed value itself, whether or not the publish tick has come
    //DH1KLM round. Without this the ballistics can only be observed through the
    //DH1KLM tick, which makes the assertion depend on wall-clock timing and lets a
    //DH1KLM test pass with the smoothing replaced by a plain assignment.
    [[nodiscard]] double sMeterDbmForTest() const noexcept { return m_sMeter.value(); }

private:
    friend class AnanNoiseBlankerTestAccess;
    friend class AnanLinkStatsTestAccess;
    friend class AnanSupplyRailTestAccess;
    void beginDspSetup();
    void finishDspSetup(quint64 generation, bool ok, const QString& error);
    //DH1KLM The "restart P2Client with m_pendingParams, then retune" half of what
    //DH1KLM finishDspSetup() used to do inline -- extracted so beginRateChange()'s
    //DH1KLM background-rebuild path (finishRateChange()) can reuse it too, instead
    //DH1KLM of a second copy. Reads m_rateChanging to decide which connect timeout
    //DH1KLM to pass (see P2Client::start()'s own comment).
    void startP2ClientSession(quint64 generation);
    //DH1KLM Set by connectRadio() just before beginDspSetup(); read back by
    //DH1KLM finishDspSetup() once AnanRxDsp::configure() completes, so the P2Client
    //DH1KLM session isn't started until the DSP chain that will consume its IQ
    //DH1KLM actually exists.
    P2Client::Params m_pendingParams;
    AnanRxDsp::Config m_pendingDspConfig;
    //DH1KLM Panadapter points from the panel width (setPanPixelWidth()). Kept here
    //DH1KLM as well as in AnanRxDsp so a connect that happens after the width
    //DH1KLM arrives still builds the analyzer at that count.
    int m_panPoints = static_cast<int>(kDroopCorrectionFftSize);
    void emitSliceState();
    void emitPanState();

    //DH1KLM Convert one demodulated block to the radio's speaker stream and hand it to
    //DH1KLM P2Client. Takes the SAME buffer the speakers get, after the receiver's
    //DH1KLM audio stage, so mute and the fader reach the radio as well.
    //DH1KLM
    //DH1KLM Does nothing unless the session was started with speaker audio enabled.
    void sendSpeakerAudioToRadio(const QByteArray& stereoFloat);
    //DH1KLM Build or discard the two resamplers for the current audio rate. Called
    //DH1KLM whenever the DSP is configured or rebuilt, because the rate is theirs to
    //DH1KLM follow and a rate change invalidates their filter state.
    void resetSpeakerResamplers();
    //DH1KLM Declares SLC:LEVEL and RAD:+13.8A to the meter seam; on every connect,
    //DH1KLM before the first reading can arrive. See its definition.
    void defineMeters();
    //DH1KLM One client snapshot -> one LinkStats push. The FIXED cadence is the
    //DH1KLM client's timer, not ours: this runs on whatever the client published, and
    //DH1KLM it must keep arriving after the radio goes quiet, because "nothing came
    //DH1KLM this second" is the observation the heartbeat's alarm path waits for
    //DH1KLM (IRadioBackend::LinkStats).
    void onLinkCounters(const P2Client::LinkCounters& counters);
    //DH1KLM The last snapshot the client published, cached on THIS thread so the
    //DH1KLM synchronous linkStats() getter never reads the I/O thread's counters --
    //DH1KLM see P2Client::LinkCounters for why that read would be a race.
    P2Client::LinkCounters m_linkCounters;
    bool m_linkCountersSeen = false;
    //DH1KLM rxBytes at the previous snapshot, for LinkStats::alive -- a link that is
    //DH1KLM bound and counting but has gone quiet must read as dead, which a
    //DH1KLM cumulative total alone cannot say.
    quint64 m_lastSnapshotRxBytes = 0;
    //DH1KLM One WDSP S-meter reading (dBFS) -> dBm, smoothed, published on a tick.
    void onDspMeter(float dbfs);
    //DH1KLM The same smoother Hl2Backend publishes through, so the two receivers'
    //DH1KLM needles move alike by construction -- see WdspSMeter.h.
    SMeterSmoother m_sMeter;
    //DH1KLM One status packet's supply-rail count -> RAD:+13.8A, in volts, as the MEAN
    //DH1KLM of every count since the last publication, once a second. A plain mean
    //DH1KLM and not the S-meter's attack/decay: the readout wants the rail's level,
    //DH1KLM and one packet alone wanders by about +/-11 counts (+/-0.3 V) on the bench.
    void onSupplyRailCounts(int counts);
    //DH1KLM The clocked half of the above, publication included, so the window and
    //DH1KLM the meter it feeds are testable against an injected time.
    void onSupplyRailCountsAt(int counts, qint64 nowMs);
    //DH1KLM Every 1000 ms: under MeterModel::kVitalsFreshMs (1500), so a live rail
    //DH1KLM never reads as stale between publications, and about five packets at
    //DH1KLM p2app's 200 ms receive cadence.
    static constexpr qint64 kSupplyRailWindowMs = 1000;
    //DH1KLM Empties the window. Called on disconnect and at linkUp, so a new session
    //DH1KLM never averages in the last one's counts.
    void resetSupplyRailWindow();
    QElapsedTimer m_supplyRailClock;
    //DH1KLM -1 = no window open.
    qint64 m_supplyRailWindowStartMs = -1;
    qint64 m_supplyRailCountSum = 0;
    int m_supplyRailSamples = 0;
    //DH1KLM Leading+trailing throttle around applyTuneToRadioAndPan() -- see
    //DH1KLM setSliceFrequency()'s comment for why an unthrottled click/drag-tune
    //DH1KLM gesture is a problem for this backend specifically.
    void scheduleTuneApply();
    void applyTuneToRadioAndPan();
    //DH1KLM Live rate change for setPanBandwidth() -- see its own definition
    //DH1KLM comment for why this is a full stop+reconfigure+restart of the
    //DH1KLM P2Client session rather than an in-place resend while streaming.
    //DH1KLM Kicks off the background DSP rebuild (AnanRxDsp::buildChannel(), on
    //DH1KLM m_dspBuildThread) and returns immediately -- the old channel and the
    //DH1KLM live P2Client session both keep running, undisturbed, for the whole
    //DH1KLM build. finishRateChange() is what actually stops/restarts P2Client,
    //DH1KLM once the new channel already exists.
    void beginRateChange(int newRateKsps);
    //DH1KLM Runs once the background build (started by beginRateChange()) has
    //DH1KLM finished and been installed. Same body as the old beginRateChange()'s
    //DH1KLM second half, just triggered after the rebuild instead of before it --
    //DH1KLM see its own definition comment for the retry-window fix folded in
    //DH1KLM here too.
    void finishRateChange(quint64 generation, bool ok, const QString& error);

    //DH1KLM Identity guard for the droop-calibration write, in front of
    //DH1KLM AnanDroopCalibrator::saveTables() (which owns the merge, the schema
    //DH1KLM guard and the codec). Returns an empty string on success, or the
    //DH1KLM operator-facing reason it did not persist -- never void: the caller
    //DH1KLM reports the outcome to the dialog and the bridge, which both used to
    //DH1KLM claim success regardless.
    [[nodiscard]] QString persistDroopTables(
        const QMap<int, anan_original::DroopCorrectionTable>& tables);
    //DH1KLM If a zoom request arrived while a previous one was still in flight
    //DH1KLM (m_pendingBandwidthKsps != 0), starts it now. Called from every path
    //DH1KLM that clears m_rateChanging -- linkUp success and both finishDspSetup()
    //DH1KLM failure branches -- so a fast zoom sweep converges on the operator's
    //DH1KLM LATEST request instead of stalling on whichever one happened to be
    //DH1KLM running when they stopped clicking.
    void retryPendingRateChange();
    [[nodiscard]] double cwBfoHz() const noexcept { return cwBfoOffsetHz(m_mode, m_cwPitchHz); }
    //DH1KLM The operator's mode, passband, AGC and noise blanker, written into a DSP
    //DH1KLM config. connectRadio() and beginRateChange() both build theirs with it.
    void applyLiveRxState(AnanRxDsp::Config& config) const;
    //DH1KLM Re-push mode + filter (with the CW BFO folded in) + shift, in that
    //DH1KLM order, to m_dsp. HERMES.md §16.7: mode changes must re-push the
    //DH1KLM passband, every time, not only when its value changed.
    void pushModeFilterShift();

    QThread* m_ioThread = nullptr;
    P2Client* m_client = nullptr;    // lives on m_ioThread; nullptr parent (moveToThread requires it)
    AnanRxDsp* m_dsp = nullptr;      // lives on m_ioThread; nullptr parent
    //DH1KLM Build-only thread for a rate change's background AnanRxDsp::buildChannel()
    //DH1KLM call -- never touches P2Client or the real-time IQ path, so it can never
    //DH1KLM starve the keepalive the way blocking m_ioThread with the same work
    //DH1KLM used to (see beginRateChange()'s comment). m_dspBuildContext is a bare
    //DH1KLM QObject living there, used purely as an invokeMethod thread-affinity
    //DH1KLM target -- it owns no state of its own.
    QThread* m_dspBuildThread = nullptr;
    QObject* m_dspBuildContext = nullptr;
    bool m_connected = false;
    //DH1KLM Bumped on every connectRadio()/disconnectRadio(); a finishDspSetup()
    //DH1KLM callback checks the generation it captured against the current one and
    //DH1KLM bails silently if they differ -- the guard against a slow first
    //DH1KLM AnanRxDsp::configure() (FFTW PATIENT planning, ~19s cold per
    //DH1KLM HERMES.md §22.3) completing after a newer connect or a disconnect.
    quint64 m_connectGeneration = 0;

    //DH1KLM Filled in from P2Client::discoveryInfoReceived() -- see capabilities()'s
    //DH1KLM own comment. Reset at the start of connectRadio() so a fresh connect
    //DH1KLM (possibly to a DIFFERENT host) never reports a prior radio's identity
    //DH1KLM before its own reply lands.
    bool m_discoveryInfoReceived = false;
    quint8 m_discoveredBoardId = 0;
    quint8 m_discoveredFirmwareVer = 0;
    quint8 m_discoveredNumDdc = 0;

    //DH1KLM scheduleTuneApply()'s leading+trailing throttle state. Lives on this
    //DH1KLM object's own thread (the GUI thread), not m_ioThread.
    static constexpr int kTuneThrottleMs = 33;   // ~30 Hz ceiling on real DDC0 retunes
    QTimer* m_tuneThrottleTimer = nullptr;
    //DH1KLM Active ADC attenuation, mirrored in m_pendingParams for session restarts.
    //DH1KLM Captured/restored only through RadioStateMemory's rfGain extension.
    int m_attenuationDb = 0;
    bool m_tunePendingApply = false;

    //DH1KLM setPanBandwidth() serialization: one rate change at a time, gated on
    //DH1KLM m_rateChanging clearing (linkUp, or a finishDspSetup() failure), not a fixed
    //DH1KLM cooldown -- cold FFTW planning can take seconds, and overlapping restarts trip
    //DH1KLM P2Client's 2 s connect watchdog. At most one request is remembered while busy;
    //DH1KLM a newer one supersedes it.
    int m_pendingBandwidthKsps = 0;   // 0 = none pending
    //DH1KLM Set from beginRateChange() through the next linkUp (or a failure path). Also
    //DH1KLM suppresses connected()/disconnected() churn: a zoom is not a disconnect.
    bool m_rateChanging = false;
    //DH1KLM Audio mute after a rate-change linkUp so a fresh WdspChannel settles before
    //DH1KLM live RF (see the linkUp handler). Display is unaffected by the mute.
    static constexpr int kRateChangeAudioSettleMs = 300;
    //DH1KLM Connect timeout for the rate-change restart path only: the just-stopped radio
    //DH1KLM needs settle time to re-arm its DDC pipeline, and the 2 s first-connect
    //DH1KLM default fires spurious connection errors on it.
    static constexpr int kRateChangeConnectTimeoutMs = 6000;
    //DH1KLM Settle window after a LIVE rate change: the new WdspChannel is installed
    //DH1KLM before the radio switches, so it briefly sees old-rate samples until p2app's
    //DH1KLM register write lands; audio stays muted across it (finishRateChange()). The
    //DH1KLM session never stops, so no restart settle is needed. 250 ms is a starting
    //DH1KLM value, bench-clean at 48 and 1536 ksps. It also replaces
    //DH1KLM kRateChangeAudioSettleMs here, but measured from the swap, so WDSP gets 250 ms
    //DH1KLM minus the handoff latency; if a rate change thumps on unmute, split the two
    //DH1KLM settles before raising this (#5547).
    static constexpr int kRateChangeLiveSettleMs = 250;

    //DH1KLM DDC-Specific resend offsets (ms) inside the settle window, on top of the
    //DH1KLM immediate send: a lost datagram costs nothing audible (see finishRateChange()).
    static constexpr int kRateChangeResendMs[] = {60, 140};

    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 100;
    int m_filterHighHz = 2900;
    int m_cwPitchHz = 600;
    double m_sliceFreqHz = 0.0;
    //DH1KLM What applyRestoredState() accepted, held for connectRadio(); kept apart
    //DH1KLM from the live m_sliceFreqHz/m_mode. Zero/empty: nothing restored.
    double m_restoredFreqHz = 0.0;
    QString m_restoredMode;
    //DH1KLM Live AGC state in AGC-T knob units (agcKnobDb() converts). Both
    //DH1KLM connectRadio() and beginRateChange() build the DSP config from it, so
    //DH1KLM it survives a reconnect. RadioModel rebuilds the backend only on a
    //DH1KLM family switch, so connecting another ANAN keeps it too, as it keeps the
    //DH1KLM noise blanker below. emitSliceState() publishes it so the knob shows
    //DH1KLM what the DSP runs.
    int m_agcMode = 3;
    int m_agcThreshold = kDefaultAgcThreshold;
    int m_agcOffLevel = kDefaultAgcOffLevel;
    //DH1KLM Noise blanker as setSliceNoiseBlanker() last stored it. Both
    //DH1KLM connectRadio() and beginRateChange() build the DSP config from it, as
    //DH1KLM they do the AGC above. emitSliceState() also publishes the pair when a
    //DH1KLM different radio gets a fresh slice, keeping its NB button in agreement
    //DH1KLM with the retained setting. Defaults match AnanRxDsp::Config's.
    AetherSDR::NoiseBlankerKind m_nbKind = AetherSDR::NoiseBlankerKind::Off;
    int m_nbLevel = 50;
    AetherSDR::NoiseBlankerFill m_nbFill = AetherSDR::kDefaultNoiseBlankerFill;

    //DH1KLM The receiver audio stage as last set; emitSliceState() publishes these.
    //DH1KLM Retained across rate changes and reconnects (like the blanker and AGC).
    //DH1KLM 100/50 = unity, centred.
    bool m_sliceAudioMuted = false;
    int m_sliceAudioGainPercent = 100;
    int m_sliceAudioPanPercent = 50;

    //DH1KLM ---- speaker stream (DDC Audio, PC -> radio) ----
    //DH1KLM
    //DH1KLM Live copy of the connect-time parameter, so the audio path tests one bool
    //DH1KLM rather than reaching into m_pendingParams on every block.
    bool m_speakerAudioEnabled = false;
    //DH1KLM The radio's own output level/mute. 50 matches RadioModel's default, which it
    //DH1KLM resets to on every radio change; the two halves must agree. Also the
    //DH1KLM level applyRestoredState() returns to for a radio with none stored.
    static constexpr int kDefaultLineoutGainPercent = 50;
    int m_lineoutGainPercent = kDefaultLineoutGainPercent;
    bool m_lineoutMuted = false;
    //DH1KLM One resampler PER CHANNEL, never processStereoToStereo(), which averages to
    //DH1KLM mono and would undo the balance (as the engine's own output resampler,
    //DH1KLM docs/architecture/audio-pipeline.md).
    std::unique_ptr<Resampler> m_speakerResampleL;
    std::unique_ptr<Resampler> m_speakerResampleR;
    //DH1KLM Deinterleave/convert scratch, retained so a steady stream does not
    //DH1KLM allocate once per block in the audio path.
    std::vector<float> m_speakerSrcL;
    std::vector<float> m_speakerSrcR;
    QByteArray m_speakerOutL;
    QByteArray m_speakerOutR;
    std::vector<qint16> m_speakerInterleaved;

    //DH1KLM Fixed identifiers -- Phase 1b is exactly one slice, one pan.
    static constexpr int kSliceId = 0;
    static const QString kPanId;

    //DH1KLM Per-radio settings identity ("anan" family): RadioConnectRequest::serial
    //DH1KLM (AnanDiscovery::macToSerial()), set at the top of connectRadio(); empty before
    //DH1KLM the first connect. isValid() needs only the family, so an empty serial would
    //DH1KLM silently target the family-wide row -- droopcal's `start` guards on an empty
    //DH1KLM radioId explicitly.
    QString m_radioSerial;
    AnanDroopCalibrator m_droopCalibrator;
    QString m_droopMessage;
    int m_droopPercent = 0;
    void publishDroopStatus();
    QString applyDroopTables(const QMap<int, DroopCorrectionTable>& tables);


    //DH1KLM The DDC0 rate actually running, captured at the top of
    //DH1KLM beginRateChange() before the pending fields are overwritten, so
    //DH1KLM finishRateChange()'s failure path can put them back. 0 until the first
    //DH1KLM rate change. See beginRateChange()'s own comment.
    int m_preRateChangeKsps = 0;
};

}  // namespace AetherSDR::anan_original
