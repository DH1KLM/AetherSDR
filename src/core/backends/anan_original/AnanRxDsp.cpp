#include "core/backends/anan_original/AnanRxDsp.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <cstdint>

Q_LOGGING_CATEGORY(lcOriginalAnanRxDsp, "aether.anan.rxdsp")

namespace AetherSDR::anan_original {

AnanRxDsp::AnanRxDsp(QObject* parent) : QObject(parent)
{
    //DH1KLM Registered so audioReady/spectrumReady can cross a thread boundary
    //DH1KLM once this object is moved onto its own DSP thread (queued connections).
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    //DH1KLM Control verbs arrive here as queued invokeMethod calls from the GUI
    //DH1KLM thread; without this the Mode argument has no metatype and Qt drops
    //DH1KLM the call with only a warning.
    qRegisterMetaType<WdspChannel::Mode>("WdspChannel::Mode");
}

AnanRxDsp::~AnanRxDsp()
{
    //DH1KLM Stop the channel first so WdspChannel::close() skips WDSP's 100 ms
    //DH1KLM stop-and-flush timeout (docs/HERMES.md §13 item 9b); nothing feeds it after
    //DH1KLM this, so no drain runs. setRunning() refuses while a processIq() callback is in
    //DH1KLM flight, which cannot happen on this (the feeding) thread -- a false means that
    //DH1KLM assumption broke, so log it.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcOriginalAnanRxDsp)
            << "could not stop the WDSP channel before destroying it: a "
               "processIq callback was in flight. Teardown will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
}

bool AnanRxDsp::configure(const Config& config, std::string* error)
{
    //DH1KLM Guard the rate/block inputs before the block-size division below.
    //DH1KLM Reject at the boundary rather than crash or build a nonsensical WDSP
    //DH1KLM channel (Principle VII). buildChannel() repeats this same guard so it
    //DH1KLM stays safe called on its own (a rate change never routes through
    //DH1KLM configure() -- see the header comment on both).
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "AnanRxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;
    RebuildResult result = buildChannel(config);
    if (!result.channel || !result.analyzer) {
        if (error)
            *error = result.error;
        return false;
    }
    installChannel(std::move(result));
    return true;
}

AnanRxDsp::RebuildResult AnanRxDsp::buildChannel(const Config& config)
{
    RebuildResult result;

    //DH1KLM Same guard as configure() -- see its comment. Repeated here because
    //DH1KLM this is the entry point a rate change actually calls, on a thread
    //DH1KLM that isn't this object's own.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        result.error = "AnanRxDsp: input/audio sample rate and DSP block size "
                       "must all be positive";
        return result;
    }

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    //DH1KLM dsp_size describes the same span of time as in_size, but at dsp_rate:
    //DH1KLM     dsp_insize = dsp_size * (in_rate / dsp_rate)   [WDSP channel.c]
    //DH1KLM so dsp_size = in_size * dsp_rate / in_rate makes WDSP consume exactly
    //DH1KLM one of our input blocks per DSP pass.
    wc.dspBlockSize = static_cast<std::size_t>(config.dspBlockSize) *
                      static_cast<std::size_t>(kWdspDspSampleRateHz) /
                      static_cast<std::size_t>(config.inputSampleRateHz);
    wc.inputSampleRate = config.inputSampleRateHz;
    //DH1KLM The WDSP DSP rate is 48 kHz and is NOT the audio rate -- see
    //DH1KLM Hl2RxDsp::configure()'s comment for the reference-client precedent
    //DH1KLM this matches (Thetis, pihpsdr both hold RXA's internal rate at
    //DH1KLM 48 kHz regardless of the radio's own IQ rate).
    wc.dspSampleRate = kWdspDspSampleRateHz;
    wc.outputSampleRate = config.audioSampleRateHz;
    wc.mode = config.mode;
    wc.filterLowHz = config.filterLowHz;
    wc.filterHighHz = config.filterHighHz;
    wc.agcMode = config.agcMode;
    wc.maximumAgcGainDb = config.maximumAgcGainDb;
    wc.agcFixedGainDb = config.agcFixedGainDb;
    wc.blockForOutput = config.blockForOutput;
    wc.noiseBlanker = config.noiseBlanker;
    wc.noiseBlankerLevel = config.noiseBlankerLevel;
    wc.noiseBlankerFill = config.noiseBlankerFill;
    //DH1KLM filterTaps left at WdspChannel::Config's own default (2048): this
    //DH1KLM phase has no manual notch filter, so there is no narrow-notch floor to
    //DH1KLM widen it for (contrast Hl2RxDsp::kRxFilterTaps, which exists solely
    //DH1KLM for that reason).

    auto channel = WdspChannel::create(wc, &result.error);
    if (!channel)
        return result;
    //DH1KLM The analyzer reuses the channel's id as its analyzer slot: both are
    //DH1KLM unique for as long as the channel lives, and RebuildResult/this class
    //DH1KLM destroy the analyzer before the channel returns the id. Built here, on
    //DH1KLM the build thread, because its first SetAnalyzer() plans FFTW_PATIENT.
    AnanPanAnalyzer::Settings as;
    as.sampleRateHz = config.inputSampleRateHz;
    as.numPoints = config.panPoints;
    as.framesPerSecond = config.spectrumFps;
    as.averageTimeMs = config.spectrumAverageMs;
    as.logAverage = config.spectrumLogAverage;
    auto analyzer = AnanPanAnalyzer::create(channel->channelId(), as, &result.error);
    if (!analyzer)
        return result;   // channel is released here, analyzer never existed
    result.outputBlockSize = channel->outputBlockSize();
    result.inputSampleRateHz = config.inputSampleRateHz;
    result.channel = std::move(channel);
    result.analyzer = std::move(analyzer);
    return result;
}

void AnanRxDsp::beginInitialBuild(const Config& config)
{
    m_config = config;
    m_rebuildInFlight = true;
}

void AnanRxDsp::beginRebuild()
{
    m_rebuildInFlight = true;
}

bool AnanRxDsp::installRebuiltChannel(RebuildResult result)
{
    m_rebuildInFlight = false;
    //DH1KLM buildChannel() never returns one without the other; refuse a result
    //DH1KLM that does rather than install a channel with no spectrum stage.
    if (!result.channel || !result.analyzer)
        return false;
    installChannel(std::move(result));
    return true;
}

void AnanRxDsp::installChannel(RebuildResult result)
{
    //DH1KLM The only update site for this field outside configure()'s own
    //DH1KLM synchronous m_config = config -- see RebuildResult::inputSampleRateHz's
    //DH1KLM comment. Must land before droopTableForRate() is ever consulted again,
    //DH1KLM which processIqBlock() does on every block once m_channel is swapped
    //DH1KLM below.
    m_config.inputSampleRateHz = result.inputSampleRateHz;

    m_iqBuffer.clear();
    m_i.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_left.assign(result.outputBlockSize, 0.0f);
    m_right.assign(result.outputBlockSize, 0.0f);
    m_stereo.assign(result.outputBlockSize * 2, 0.0f);
    //DH1KLM DC blocker pole for the AUDIO rate -- the blocker runs on
    //DH1KLM WdspChannel's output, not its 48 kHz internal rate. Recomputed here so
    //DH1KLM a rate change keeps the same corner frequency instead of moving it.
    const float pole = dcBlockerPole(kDcBlockerCornerHz,
                                     static_cast<double>(m_config.audioSampleRateHz));
    m_dcBlockL.r = pole;
    m_dcBlockR.r = pole;
    //DH1KLM PcmFormat accepts 24000/48000 only. AnanBackend hardcodes 24000 today, so
    //DH1KLM this cannot fail in production — but if that rate ever moves, a silent
    //DH1KLM refusal here stops ANAN audio dead while the spectrum keeps updating,
    //DH1KLM which reads as a dead radio rather than a configuration error.
    if (!m_pcmProducer.start(PcmPurpose::Speaker, -1,
                             {m_config.audioSampleRateHz, PcmLayout::Stereo})) {
        qWarning() << "AnanRxDsp: no PCM producer for audio rate"
                   << m_config.audioSampleRateHz
                   << "Hz - RX audio will be silent on this channel";
    }
    m_dcBlockL.reset();
    m_dcBlockR.reset();

    //DH1KLM Same idea for the S-meter, one stage further back: the new channel's
    //DH1KLM own average starts at zero (create_meter() -> flush_meter()), so its
    //DH1KLM first readings are low for the length of the tap's time constant
    //DH1KLM whether or not a mute is involved. A rate change arms this twice --
    //DH1KLM here, and again when beginRateChange()'s settle window unmutes -- which
    //DH1KLM costs nothing: the window is re-armed, not accumulated.
    armMeterSettle();

    //DH1KLM Re-apply m_config's CURRENT values -- for configure() this is exactly
    //DH1KLM what buildChannel() was just given (m_config == config already); for
    //DH1KLM a rate-change swap it may include mode/filter/AGC changes the operator
    //DH1KLM made WHILE the build was in flight, which only ever reached m_config
    //DH1KLM (setMode() et al.'s in-flight gating) and never reached the snapshot
    //DH1KLM buildChannel() actually built from.
    result.channel->setMode(m_config.mode);
    result.channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    result.channel->setAgcFixedGain(m_config.agcFixedGainDb);
    result.channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
    //DH1KLM A rebuild creates a fresh channel; restore the operator's current
    //DH1KLM slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        result.channel->setShift(m_shiftHz);
    //DH1KLM Sent even when it matches what the channel was built with: the operator
    //DH1KLM may have moved the NB button while the background build ran.
    if (!result.channel->setNoiseBlanker(m_config.noiseBlanker,
                                         m_config.noiseBlankerLevel,
                                         m_config.noiseBlankerFill)) {
        qCWarning(lcOriginalAnanRxDsp) << "noise blanker refused by the rebuilt channel";
    }
    //DH1KLM The hold flag belongs to the channel, so a rebuild loses it. A rate
    //DH1KLM change swaps the channel while audio is muted for its settle window.
    result.channel->setNoiseBlankerHold(m_audioMuted);

    //DH1KLM Stop the OUTGOING channel before it is destroyed so close() skips the 100 ms
    //DH1KLM stop-and-flush timeout; this is the processIq() thread, so no block reaches it
    //DH1KLM in between. Not in beginRebuild(): the old channel must keep producing audio
    //DH1KLM through the background build. Checked as in the destructor.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcOriginalAnanRxDsp)
            << "could not stop the outgoing WDSP channel before the swap: a "
               "processIq callback was in flight. The rebuild will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
    //DH1KLM The analyzer was built for m_config.spectrumFps as it stood when the
    //DH1KLM build began; bring it up to the operator's current rate.
    result.analyzer->setFramesPerSecond(m_config.spectrumFps);
    result.analyzer->setNumPoints(m_config.panPoints);
    result.analyzer->setAverageTimeMs(m_config.spectrumAverageMs);
    result.analyzer->setLogAverage(m_config.spectrumLogAverage);
    //DH1KLM Analyzer before channel: the outgoing analyzer uses the outgoing
    //DH1KLM channel's id as its slot and must be destroyed before that channel
    //DH1KLM releases the id -- see RebuildResult.
    m_analyzer = std::move(result.analyzer);
    m_channel = std::move(result.channel);
    publishNoiseBlankerState();
}

void AnanRxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    //DH1KLM See beginRebuild()'s own comment: pushing through mid-build would
    //DH1KLM block this thread on WDSP's process-wide setup mutex for however long
    //DH1KLM the background build has left. m_config still updates, so the value
    //DH1KLM is not lost -- installRebuiltChannel() re-applies it at the swap.
    if (m_channel && !m_rebuildInFlight)
        m_channel->setMode(mode);
}

void AnanRxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setFilter(lowHz, highHz);
}

void AnanRxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgc(agcMode, maximumGainDb);
}

void AnanRxDsp::setAgcFixedGain(double fixedGainDb)
{
    m_config.agcFixedGainDb = fixedGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgcFixedGain(fixedGainDb);
}

void AnanRxDsp::setAudioMuted(bool muted)
{
    //DH1KLM The mute LIFTING is the edge that matters to the S-meter: WDSP's
    //DH1KLM average has been integrating the zeros this class fed it for the whole
    //DH1KLM mute and nothing flushes it, so the first blocks after the unmute still
    //DH1KLM read that silence. Swallow them instead of publishing them -- see
    //DH1KLM WdspSMeter.h for why the guard in processIqBlock() is not enough on
    //DH1KLM its own.
    if (m_audioMuted && !muted)
        armMeterSettle();
    m_audioMuted = muted;
    //DH1KLM Muted, the channel is clocked with ZEROS. The noise blanker triggers on
    //DH1KLM magnitude against a running average magnitude, so silence would drag
    //DH1KLM that average toward zero and the first real block afterwards would look
    //DH1KLM like one long impulse and be blanked. Holding the stage freezes the
    //DH1KLM average at the pre-mute level; see WdspChannel::setNoiseBlankerHold().
    if (m_channel)
        m_channel->setNoiseBlankerHold(muted);
}

void AnanRxDsp::setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                WdspChannel::NoiseBlankerFill fill)
{
    m_config.noiseBlanker = kind;
    m_config.noiseBlankerLevel = std::clamp(level, 0, 100);
    m_config.noiseBlankerFill = fill;
    if (!m_channel || m_rebuildInFlight)
        return;
    //DH1KLM WdspChannel refuses, rather than blocks on, a control call that races
    //DH1KLM another one. Logged so a refused toggle is not silent; m_config keeps
    //DH1KLM the request and the next rebuild applies it.
    if (!m_channel->setNoiseBlanker(m_config.noiseBlanker,
                                    m_config.noiseBlankerLevel,
                                    m_config.noiseBlankerFill)) {
        qCWarning(lcOriginalAnanRxDsp) << "noise blanker kind" << static_cast<int>(kind)
                               << "level" << m_config.noiseBlankerLevel
                               << "fill" << static_cast<int>(fill)
                               << "refused by the channel";
    }
    publishNoiseBlankerState();
}

void AnanRxDsp::publishNoiseBlankerState()
{
    //DH1KLM Read the applied channel even after refusal, never the requested config.
    const int state = m_channel
        ? m_channel->config().noiseBlankerLevel
              + kNbLevelStride * static_cast<int>(m_channel->noiseBlankerKind())
              + kNbLevelStride * kNbKindStride
                    * static_cast<int>(m_channel->noiseBlankerFill())
        : -1;
    m_nbAppliedState.store(state, std::memory_order_relaxed);
}

void AnanRxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    if (fps > 0) {
        m_config.spectrumFps = fps;
        //DH1KLM Mid-rebuild the incoming analyzer picks the rate up at install
        //DH1KLM (installChannel()); the outgoing one is about to be discarded.
        if (m_analyzer && !m_rebuildInFlight)
            m_analyzer->setFramesPerSecond(fps);
    }
    //DH1KLM Do NOT reset the clock or the last-emit stamp -- a rate change
    //DH1KLM mid-stream should take effect on the next frame that comes due, not
    //DH1KLM grant an immediate extra one.
}

void AnanRxDsp::setSpectrumAverageMs(int ms)
{
    if (ms < 0)
        return;
    m_config.spectrumAverageMs = ms;
    //DH1KLM Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    //DH1KLM up at install.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setAverageTimeMs(ms);
}

void AnanRxDsp::setPanPoints(int points)
{
    if (points < 2)
        return;
    m_config.panPoints = std::min(points, AnanPanAnalyzer::kMaxPoints);
    //DH1KLM Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    //DH1KLM up at install.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setNumPoints(m_config.panPoints);
}

void AnanRxDsp::setSpectrumLogAverage(bool on)
{
    m_config.spectrumLogAverage = on;
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setLogAverage(on);
}

void AnanRxDsp::setDroopCorrectionTable(int rateKsps, const std::vector<float>& table)
{
    if (table.size() != kDroopCorrectionFftSize)
        return;
    bool valid = false;
    for (const int r : kDdc0RatesKsps)
        valid |= (r == rateKsps);
    if (!valid)
        return;
    DroopCorrectionTable t;
    std::copy(table.begin(), table.end(), t.begin());
    m_droopTables[rateKsps] = t;
}

void AnanRxDsp::setDroopCorrectionBypassed(bool bypassed)
{
    m_droopBypassed = bypassed;
}

void AnanRxDsp::clearDroopCorrectionTables()
{
    m_droopTables.clear();
}

const DroopCorrectionTable& AnanRxDsp::droopTableForRate(int rateKsps) const noexcept
{
    //DH1KLM Returning the kDroopCorrectionZero OBJECT (not a zero-valued copy) is
    //DH1KLM what also suppresses the edge fade in processIqBlock(), which tests
    //DH1KLM identity against exactly this address -- see
    //DH1KLM setDroopCorrectionBypassed()'s comment.
    if (m_droopBypassed)
        return kDroopCorrectionZero;
    const auto it = m_droopTables.constFind(rateKsps);
    return it != m_droopTables.constEnd() ? it.value() : kDroopCorrectionZero;
}

void AnanRxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setShift(shiftHz);
}

bool AnanRxDsp::spectrumFrameDue()
{
    if (m_spectrumIntervalMs <= 0)
        return true;                       // uncapped
    if (!m_spectrumClock.isValid()) {
        m_spectrumClock.start();
        m_lastSpectrumMs = 0;
        return true;                       // paint the first frame immediately
    }
    return (m_spectrumClock.elapsed() - m_lastSpectrumMs) >= m_spectrumIntervalMs;
}

void AnanRxDsp::onSequenceGap()
{
    if (!m_analyzer) {
        return;   // between rebuilds; the new analyzer starts empty
    }
    //DH1KLM Counted only when something was actually in flight -- see
    //DH1KLM Hl2RxDsp::onSequenceGap() for why a boundary-aligned gap must not be
    //DH1KLM counted, and why neither the frame-rate clock nor the audio path is
    //DH1KLM touched here.
    if (m_analyzer->dropStagedPartial() > 0) {
        m_spectrumGapDiscards.fetch_add(1, std::memory_order_relaxed);
    }
}

void AnanRxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    //DH1KLM Handedness: read docs/HERMES.md §16 before touching this. WDSP's RXA as
    //DH1KLM configured here selects the opposite sign to its passband bounds, and the HPSDR
    //DH1KLM wire is the conjugate of the analytic convention (confirmed for P2 by
    //DH1KLM `radiocert rx` on WWV and by SDR++ on an RSP1B). So the demodulator takes the
    //DH1KLM raw wire and the spectrum is mirrored exactly once (§16.6 rule 2) -- inside
    //DH1KLM WDSP's analyzer, whose Spectrum0() swaps I and Q. Conjugating here too would
    //DH1KLM mirror twice. anan_rxdsp_handedness_test pins this.

    //DH1KLM Panadapter: every block goes to the analyzer, whatever the display rate
    //DH1KLM -- its FFTs overlap so that one completes per display frame over fresh
    //DH1KLM samples, and none are thrown away. Only TAKING a frame is paced here.
    m_analyzer->feed(iq);
    if (spectrumFrameDue() && m_analyzer->takeFrame(m_bins)) {
        //DH1KLM Real DDC0 roll-off -- the anti-alias FIR's transition band, not CIC
        //DH1KLM sin(x)/x (see AnanDroopCorrection.h). The analyzer's points are
        //DH1KLM already time-averaged, so the correction lands on the averaged
        //DH1KLM level. inputSampleRateHz is always an exact multiple of 1000 for
        //DH1KLM the six valid DDC0 rates.
        const DroopCorrectionTable& droopTable =
            droopTableForRate(m_config.inputSampleRateHz / 1000);
        applyDroopCorrectionDbResampled(m_bins, droopTable);
        //DH1KLM Cosmetic edge fade (see applyEdgeFade()). connectRadio() seeds defaults for
        //DH1KLM all six rates, so on a G2 this always runs; the identity test only suppresses
        //DH1KLM it during a calibration bypass, when droopTableForRate() returns the
        //DH1KLM kDroopCorrectionZero object so a sweep measures the radio, not our fade.
        if (&droopTable != &kDroopCorrectionZero)
            applyEdgeFade(m_bins);
        emit spectrumReady(m_bins);
        m_lastSpectrumMs = m_spectrumClock.elapsed();
    }

    //DH1KLM Audio: the RAW wire (see the handedness note above).
    m_iqBuffer.insert(m_iqBuffer.end(), iq.begin(), iq.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        if (m_audioMuted) {
            std::fill(m_i.begin(), m_i.end(), 0.0f);
            std::fill(m_q.begin(), m_q.end(), 0.0f);
        } else {
            for (std::size_t n = 0; n < block; ++n) {
                m_i[n] = m_iqBuffer[consumed + n].real();
                m_q[n] = m_iqBuffer[consumed + n].imag();
            }
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        //DH1KLM Count every outcome, Ok included -- the Ok count is the denominator
        //DH1KLM a fault total has to be read against. Same rule, same words and the
        //DH1KLM same bounded log schedule as Hl2RxDsp::processIqBlock: these two
        //DH1KLM stages are copies of each other and the counting rule is the part
        //DH1KLM that must not drift, which is why it lives in WdspProcessTally.h
        //DH1KLM rather than twice here.
        const std::uint64_t seen = m_processTally.record(res);
        if (res != WdspChannel::ProcessResult::Ok) {
            //DH1KLM Underrun excluded from the log and NOT from the count: it is
            //DH1KLM normal while the asynchronous output side fills, and logging it
            //DH1KLM would drown the four outcomes that are not normal.
            //DH1KLM
            //DH1KLM After processIq() returns, never inside it -- qCWarning
            //DH1KLM allocates, and allocating between WdspChannel's two reads of
            //DH1KLM wdspPortAllocationSequence() would manufacture the very
            //DH1KLM AllocationViolation being reported.
            if (res != WdspChannel::ProcessResult::Underrun
                && WdspProcessTally::shouldLog(seen)) {
                qCWarning(lcOriginalAnanRxDsp)
                    << "WDSP processIq failed:" << WdspProcessTally::name(res)
                    << "- occurrence" << seen
                    << "on WDSP channel" << m_channel->channelId()
                    << "- this block produces no audio";
            }
            continue;   // underrun while the pipeline fills, etc. -- no output yet
        }

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_dcBlockL.process(m_left[k]);
            m_stereo[2 * k + 1] = m_dcBlockR.process(m_right[k]);
        }
        QVector<float> samples(m_stereo.begin(), m_stereo.end());
        if (const auto frame = m_pcmProducer.produce(std::move(samples))) {
            emit pcmReady(*frame);
            emit audioReady(m_stereo);
        }
        //DH1KLM AVERAGE, NOT PEAK: xmeter's `avg` is an EMA of I*I + Q*Q, `peak` a decaying
        //DH1KLM peak-hold. They agree on a carrier but peak reads band noise ~11-14 dB high,
        //DH1KLM and S9 (-73 dBm) is an RMS quantity. The backend applies its own ballistics.
        //DH1KLM Gated off while muted and for the settle window after unmute or a channel
        //DH1KLM install (the channel is fed zeros, and the average would carry that silence
        //DH1KLM past the unmute; see WdspSMeter.h). The gate also fixes cadence at one reading
        //DH1KLM per 48k-equivalent block (~47/s at every DDC0 rate). Blocks that underrun
        //DH1KLM above do not tick it.
        if (!m_audioMuted && m_meterTap.tick()) {
            emit meterUpdate(static_cast<float>(
                m_channel->meter(WdspChannel::Meter::SignalAverage)));
        }
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::anan_original
