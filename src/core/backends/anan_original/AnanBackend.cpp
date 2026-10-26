#include "core/backends/anan_original/AnanBackend.h"
#include "core/AppActivity.h"
#include "core/backends/anan_original/AnanDroopCalibrator.h"
#include "core/backends/anan_original/AnanDroopDefaults.h"
#include "core/backends/WdspNoiseBlanker.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

Q_LOGGING_CATEGORY(lcOriginalAnanDefaults, "aether.anan.droopdefaults", QtWarningMsg)

namespace AetherSDR::anan_original {

const QString AnanBackend::kPanId = QStringLiteral("anan-0");

namespace {

//DH1KLM Placeholder until a bench measurement exists -- see the class comment.
//DH1KLM Named and zero rather than silently absent, so the next reader finds a
//DH1KLM TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

//DH1KLM One FFT AVG slider step as analyzer averaging time. deskHPSDR sets its
//DH1KLM analyzer's averaging as a time in 10 ms steps (default 250 ms), so 0 = none,
//DH1KLM 25 = deskHPSDR's default, 100 = one second. Published in capabilities()
//DH1KLM as BackendPanAveraging::msPerAverageStep.
constexpr int kMsPerAverageStep = 10;

//DH1KLM S-meter: WDSP's RXA_S_AV is 10*log10 mean I^2+Q^2 at the P2 24-bit full scale
//DH1KLM (kFullScale24Bit), i.e. dBFS. deskHPSDR uses the same scaling (1/2^23) and a
//DH1KLM 0 dB ANAN offset, so dBFS reads as dBm; this follows it. onDspMeter() adds the
//DH1KLM ADC attenuation before smoothing. No per-radio rx_gain_calibration yet. Kept
//DH1KLM apart from kUncalibratedDbfsToDbmOffset, which labels the panadapter axis.
constexpr double kSMeterDbmOffset = 0.0;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
           static_cast<qsizetype>(v.size() * sizeof(float))};
}

//DH1KLM The slice model's name for a WDSP AGC mode, the inverse of setSliceAgc()'s
//DH1KLM parse. WDSP's "long" (1) is never set here.
QString agcModeName(int wdspMode)
{
    switch (wdspMode) {
    case 0: return QStringLiteral("off");
    case 2: return QStringLiteral("slow");
    case 4: return QStringLiteral("fast");
    default: return QStringLiteral("med");
    }
}

}  // namespace

WdspChannel::Mode AnanBackend::modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    //DH1KLM "CW" is the spelling TciProtocol::tciToSmartSDR produces and the one a
    //DH1KLM Flex reports -- HERMES.md §16.7's own regression was this falling
    //DH1KLM through to the USB fallback because only "CWU" was mapped.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("CWL")) return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    //DH1KLM WDSP has no RTTY demod: RTTY is FSK decoded externally from a wide passband,
    //DH1KLM so map it to DIGU rather than falling through to USB (HERMES.md 15.7/16.7).
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;   // unknown mode: same fallback as Hl2Backend's
}

bool AnanBackend::isKnownModeString(const QString& mode) noexcept
{
    //DH1KLM Every branch of modeFromString() above, aliases included.
    static const QStringList kKnown = {
        QStringLiteral("LSB"),  QStringLiteral("USB"),  QStringLiteral("DSB"),
        QStringLiteral("CWU"),  QStringLiteral("CW"),   QStringLiteral("CWL"),
        QStringLiteral("FM"),   QStringLiteral("NFM"),  QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("RTTY"),
        QStringLiteral("SAM"),  QStringLiteral("DRM"),  QStringLiteral("WBFM"),
        QStringLiteral("WFM"),
    };
    return kKnown.contains(mode.toUpper());
}

bool AnanBackend::isRestorableFrequencyHz(double hz) noexcept
{
    //DH1KLM Strictly inside (0, Nyquist): zero is the sentinel, and at or above
    //DH1KLM kDspClockHz/2 phaseWord() aliases rather than tuning.
    return std::isfinite(hz) && hz > 0.0
           && hz < static_cast<double>(kDspClockHz) / 2.0;
}

std::pair<int, int> AnanBackend::defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    //DH1KLM Same window as DIGU -- see modeFromString()'s own comment on why RTTY
    //DH1KLM maps to Digu -- made explicit rather than landing here only because it
    //DH1KLM happens to equal this function's own unknown-mode fallback below.
    if (u == QLatin1String("RTTY")) return {150, 3000};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {-250, 250};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

double AnanBackend::cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return static_cast<double>(pitchHz);
    if (u == QLatin1String("CWL"))
        return -static_cast<double>(pitchHz);
    return 0.0;
}

AnanBackend::AnanBackend(QObject* parent)
    : IRadioBackend(parent)
    , m_droopCalibrator(AnanDroopCalibrator::Hooks{
        [this] { return m_connected && !m_radioSerial.isEmpty()
                         && capabilities().hostDroopCalibration; },
        [this](int rateKsps) { setPanBandwidth(kPanId, rateKsps * 1000.0); },
        [this](bool bypass) {
            if (m_dsp) {
                QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionBypassed",
                    Qt::QueuedConnection, Q_ARG(bool, bypass));
            }
        },
        [this](const QMap<int, DroopCorrectionTable>& tables) {
            return applyDroopTables(tables);
        }})
{
    connect(&m_droopCalibrator, &AnanDroopCalibrator::started, this, [this] {
        m_droopMessage = QStringLiteral("Sweeping");
        m_droopPercent = 0;
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::progress, this,
            [this](int rateIndex, int totalRates, int percent) {
        m_droopPercent = percent;
        m_droopMessage = QStringLiteral("Sweeping — rate %1 of %2…")
                            .arg(rateIndex + 1).arg(totalRates);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [this](const QString& reason) {
        m_droopMessage = QStringLiteral("Error: %1").arg(reason);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::finished, this, [this](bool applied) {
        if (!m_droopMessage.startsWith(QLatin1String("Error:"))) {
            m_droopMessage = applied
                ? QStringLiteral("Applied — the measured correction is now live and saved.")
                : (m_droopCalibrator.hasResult()
                    ? QStringLiteral("Sweep stopped — review the result, then Apply or Discard.")
                    : QStringLiteral("Sweep stopped — no result to apply."));
        }
        publishDroopStatus();
    });
    m_client = new P2Client(nullptr);   // nullptr parent: moveToThread requires it
    m_dsp = new AnanRxDsp(nullptr);

    //DH1KLM Leading+trailing throttle for setSliceFrequency()'s expensive side
    //DH1KLM effects -- see scheduleTuneApply()'s comment. Lives on this object
    //DH1KLM (the GUI thread), same as the backend itself; not moved to m_ioThread.
    m_tuneThrottleTimer = new QTimer(this);
    m_tuneThrottleTimer->setSingleShot(true);
    connect(m_tuneThrottleTimer, &QTimer::timeout, this, [this] {
        if (m_tunePendingApply) {
            m_tunePendingApply = false;
            applyTuneToRadioAndPan();
            m_tuneThrottleTimer->start(kTuneThrottleMs);   // cooldown for the trailing apply too
        }
        //DH1KLM else: nothing arrived during the cooldown -- let the timer sit
        //DH1KLM idle until the next scheduleTuneApply() restarts it.
    });

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("anan-io"));
    m_client->moveToThread(m_ioThread);
    m_dsp->moveToThread(m_ioThread);
    AetherSDR::startStreamThread(m_ioThread);   // High QoS; see AppActivity.h

    //DH1KLM Build-only thread for a rate change's background DSP rebuild -- see
    //DH1KLM the member declaration comment. m_dspBuildContext owns no state; it
    //DH1KLM exists purely so QMetaObject::invokeMethod has a thread-affinity
    //DH1KLM target to hop the (slow) AnanRxDsp::buildChannel() call onto.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    //DH1KLM Same High QoS as the io thread: this one holds the FFTW planner mutex the
    //DH1KLM io thread takes in WdspChannel::setShift, so lower would be a priority
    //DH1KLM inversion. Idle except during a rebuild.
    AetherSDR::startStreamThread(m_dspBuildThread);

    //DH1KLM Both live on the I/O thread -- a same-thread call either way, but
    //DH1KLM explicit to document intent and match Hl2Backend's own explicit
    //DH1KLM DirectConnection on the equivalent wire-to-DSP wiring: no queue, no
    //DH1KLM event-loop hop for the sample path.
    connect(m_client, &P2Client::ddc0IqReady, m_dsp, &AnanRxDsp::processIqBlock,
            Qt::DirectConnection);

    //DH1KLM Everything below crosses from the I/O thread to whichever thread this
    //DH1KLM object lives on -- plain AutoConnection resolves to Queued, which is
    //DH1KLM why AnanRxDsp's constructor registers std::vector<float> as a Qt
    //DH1KLM metatype (P2Client's ddc0IqReady never needs that: it is only ever
    //DH1KLM DirectConnection'd, above).
    connect(m_client, &P2Client::linkUp, this, [this] {
        m_connected = true;
        //DH1KLM See m_rateChanging's declaration comment: a live rate change is a
        //DH1KLM real stop+restart of this session, but the operator only asked to
        //DH1KLM zoom, so connected() is suppressed for that one round trip.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        if (!wasRateChange) {
            emit connected();
            defineMeters();
            //DH1KLM A new session's needle starts from its first reading, not from
            //DH1KLM where the last session's left off.
            m_sMeter.reset();
            //DH1KLM Likewise the rail's window: counts from before this session
            //DH1KLM must not ride into its first published voltage.
            resetSupplyRailWindow();
        }
        emitSliceState();
        emitPanState();
        //DH1KLM A rate change suppresses connected(), but a page opened during
        //DH1KLM its brief link restart still needs the fresh calibration status.
        publishDroopStatus();
        //DH1KLM Real zoom limits (HERMES.md §15.1): otherwise the GUI falls back to a FlexLib
        //DH1KLM model table (5.4 MHz for an unknown "ANAN-G2") and zooms past the 1.536 MHz
        //DH1KLM ceiling into unsampled black bars. Constant per connection (one DDC), so emit
        //DH1KLM on every linkUp; bounds are capabilities().sampleRatesHz's endpoints.
        emit panBandwidthLimitsChanged(kPanId,
                                      kDdc0RatesKsps.front() * 1000.0 / 1.0e6,
                                      kDdc0RatesKsps.back() * 1000.0 / 1.0e6);
        //DH1KLM The RF Gain slider IS the step attenuator: -31..0 dB in 1 dB steps.
        //DH1KLM Same every linkUp, for the same reason as the limits above.
        emit panRfGainInfoChanged(kPanId, -kMaxStepAttenuationDb, 0, 1);
        emit panRfGainChanged(kPanId, -m_attenuationDb);
        if (wasRateChange) {
            //DH1KLM Audio was muted in beginRateChange() before this session started (see there).
            //DH1KLM Unmute after a settle window: the rebuilt WdspChannel's AGC/filters/DC
            //DH1KLM blocker start from zero against live RF. The display is unaffected.
            const quint64 generation = m_connectGeneration;
            QTimer::singleShot(kRateChangeAudioSettleMs, this, [this, generation] {
                //DH1KLM Superseded by a newer connect/rate-change/disconnect --
                //DH1KLM that cycle's own beginRateChange()/disconnectRadio() owns
                //DH1KLM muting from here, not this timer.
                if (generation != m_connectGeneration || !m_dsp)
                    return;
                QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                          Q_ARG(bool, false));
            });
            retryPendingRateChange();
        }
    });
    connect(m_client, &P2Client::linkDown, this, [this] {
        m_connected = false;
        if (!m_rateChanging) {
            m_droopCalibrator.stop(false);
            m_droopCalibrator.setLandedRate(0);
            emit disconnected();
        }
    });
    connect(m_client, &P2Client::connectionError, this,
            [this](const QString& reason) { emit connectionError(reason); });
    //DH1KLM Drops reach the readout through the counters snapshot below, which
    //DH1KLM carries the same total on a fixed cadence. Not wired per-drop: the
    //DH1KLM LinkStats tick is deliberately periodic rather than event-driven, so a
    //DH1KLM link that has gone silent still reports.
    connect(m_client, &P2Client::linkCountersUpdated, this,
            &AnanBackend::onLinkCounters);
    //DH1KLM Queued onto this thread, like the snapshot above: the meter seam's
    //DH1KLM signals are emitted from the backend's own thread.
    connect(m_client, &P2Client::supplyRailSampled, this,
            &AnanBackend::onSupplyRailCounts);
    //DH1KLM Only DDC0 feeds this DSP. Like ddc0IqReady above, notification and
    //DH1KLM processing run directly on the I/O thread, with the DSP as context.
    //DH1KLM Invalidate its partial FFT before the discontinuous block arrives.
    connect(m_client, &P2Client::ddcSequenceGap, m_dsp, [dsp = m_dsp](int ddcIndex) {
        if (ddcIndex == 0) {
            dsp->onSequenceGap();
        }
    }, Qt::DirectConnection);
    connect(m_client, &P2Client::discoveryInfoReceived, this,
            [this](quint8 boardId, quint8 firmwareVer, quint8 numDdc) {
        //DH1KLM See capabilities()'s own comment for where these surface. Reported
        //DH1KLM real, not hardcoded -- the working plan's Step 2 exit item this
        //DH1KLM closes -- but this phase still only ever DRIVES one DDC regardless
        //DH1KLM of what numDdc says, so nothing here may touch maxSlices/
        //DH1KLM maxPanadapters; that stays a Phase 1b scope decision, not a
        //DH1KLM capability the radio gets to raise on our behalf.
        m_discoveredBoardId = boardId;
        m_discoveredFirmwareVer = firmwareVer;
        m_discoveredNumDdc = numDdc;
        m_discoveryInfoReceived = true;
        //DH1KLM The shipped droop defaults are derived from one gateware's filter
        //DH1KLM (AnanDroopDefaults.h) and still applied on a mismatch, so say so. Logged here
        //DH1KLM because connectRadio() zeroes m_discoveredFirmwareVer and the reply fills it
        //DH1KLM later. Keyed on firmwareVer only (board type must not gate behaviour), so a
        //DH1KLM non-Saturn board on the same build number goes unflagged. Informational.
        if (firmwareVer != kDefaultsGatewareVersion) {
            qCWarning(lcOriginalAnanDefaults).nospace()
                << "ANAN: radio reports gateware " << firmwareVer
                << ", shipped droop defaults were derived against "
                << kDefaultsGatewareVersion
                << " -- applying them anyway; run the in-app droop sweep if "
                   "the panadapter edges look wrong";
        }
        emit capabilitiesChanged();
    });

    connect(m_dsp, &AnanRxDsp::pcmReady, this, [this](const PcmFrame& frame) {
        QByteArray bytes = frame.legacyStereo24();
        if (bytes.isEmpty()) {
            return;
        }
        //DH1KLM THIS SLICE's audio for per-slice consumers (a TCI receiver channel, a
        //DH1KLM decoder), published BEFORE the receiver's audio stage. The seam
        //DH1KLM contract on IRadioBackend::sliceAudioFrameReady is pre-mute, pre-gain
        //DH1KLM and pre-balance, so muting a slice does not stop WSJT-X decoding on it
        //DH1KLM -- the same split a Flex gets from DAX and HL2 makes in its mixer.
        publishLegacySliceAudio(kSliceId, bytes);

        //DH1KLM The receiver's own audio stage, applied HERE rather than in the DSP.
        //DH1KLM
        //DH1KLM legacyStereo24() already hands back a fresh buffer this backend owns,
        //DH1KLM and the non-const data() below detaches from anything the slice tap
        //DH1KLM above may still share, so the stage never reaches that tap. Doing it here also keeps the whole stage on one thread with
        //DH1KLM the state the three setters write, so a mute needs no queued hop to
        //DH1KLM take effect on the next block. Both listening paths -- this computer's
        //DH1KLM speaker and the radio's own speaker below -- see the same audio.
        applySliceAudioInPlace(reinterpret_cast<float*>(bytes.data()),
                               static_cast<std::size_t>(bytes.size())
                                   / (2 * sizeof(float)),
                               m_sliceAudioMuted, m_sliceAudioGainPercent,
                               m_sliceAudioPanPercent);
        //DH1KLM One DDC, so "mixing" the speaker feed is the identity -- no
        //DH1KLM separate mix stage needed for a single receiver.
        publishLegacyAudio(bytes);
        sendSpeakerAudioToRadio(bytes);
    });
    //DH1KLM Ungated by design: m_dsp emits on m_ioThread, the thread that emits linkDown,
    //DH1KLM so every frame is delivered before disconnected() (contract rule 6).
    connect(m_dsp, &AnanRxDsp::spectrumReady, this, [this](const std::vector<float>& binsDbfs) {
        std::vector<float> dbm(binsDbfs.size());
        //DH1KLM Attenuation added back, as deskHPSDR does for its panadapter: a
        //DH1KLM signal keeps its level when the operator attenuates, and only the
        //DH1KLM part of the floor that was the ADC overloading actually drops.
        const float attenuation = static_cast<float>(m_attenuationDb);
        for (std::size_t i = 0; i < binsDbfs.size(); ++i) {
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset + attenuation;
        }
        m_droopCalibrator.onSpectrumFrame(dbm);
        emit spectrumFrameReady(kSliceId, floatBytes(dbm));
    });
    connect(m_dsp, &AnanRxDsp::meterUpdate, this, &AnanBackend::onDspMeter);
}

AnanBackend::~AnanBackend()
{
    m_droopCalibrator.stop(false);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup/rebuild callback

    //DH1KLM Join the build thread BEFORE deleting m_dsp/m_client: the build lambda's tail
    //DH1KLM posts invokeMethod(m_dsp/this, ...) after buildChannel() returns, so deleting
    //DH1KLM first is a use-after-free if the app closes mid-build. The wait is bounded by
    //DH1KLM the remaining FFTW planning.
    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }
    delete m_dspBuildContext;

    if (m_ioThread) {
        if (m_client)
            QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    }
    delete m_dsp;
    delete m_client;
}

RadioCapabilities AnanBackend::capabilities() const
{
    RadioCapabilities c;
    c.broadcastFmReceive = std::nullopt;
    c.family = QStringLiteral("anan");
    c.panDisplayControl = PanDisplayControl{{PanDisplayField::Geometry, PanDisplayField::RfGain,
        PanDisplayField::FrameRate, PanDisplayField::Average, PanDisplayField::WeightedAverage,
        PanDisplayField::SpectrumPoints}, {PanGeometryComponent::Center, PanGeometryComponent::Bandwidth}};
    c.panLifecycleControl = std::nullopt;  // fixed receiver topology: no pan create/remove
    //DH1KLM No setTune() implementation, so no tune generator to select a mode on.
    c.twoToneGenerator = std::nullopt;
    c.firmwareUpdateSource = std::nullopt;  // gateware number; no published list covers it.
    //DH1KLM No VOX, monitor or speech-processor setters. PROC would be the host
    //DH1KLM ClientComp, but canTransmit is false below, so nothing serves it and the
    //DH1KLM drop notice correctly stands.
    c.voxControl = std::nullopt;
    c.speechProcessorControl = std::nullopt;
    c.txMonitorControl = std::nullopt;
    //DH1KLM The radio's speaker output: setLineoutMute() sends it silence and
    //DH1KLM setLineoutGain() scales the samples this host feeds it.
    c.lineoutControl = RadioCapabilities::LineoutControl{};
    c.radioSetupControl = RadioSetupControl{{RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
    //DH1KLM No client owner is declared for "Show TX in Waterfall" on an ANAN.
    c.txWaterfallClientFlag = std::nullopt;
    //DH1KLM The panadapter dBm axis is dBFS with a dBm label: kUncalibratedDbfsToDbmOffset
    //DH1KLM is 0.0f and bin levels depend on window/normalisation, unverified against a
    //DH1KLM known input. Internally consistent, but not comparable: never publish as a
    //DH1KLM spot, compare with another station, or use as an absolute threshold. The
    //DH1KLM S-meter (kSMeterDbmOffset) is separate and follows deskHPSDR's scaling.
    PanAmplitudeModel amplitude;
    amplitude.calibratedDbm = false;
    //DH1KLM The bins are raw dBFS + 0, so they are ABSOLUTE: nothing in them depends on
    //DH1KLM the display reference level, and the auto-floor loop converges.
    amplitude.binsAbsolute = true;
    //DH1KLM No register holds a display range: the client is its only memory.
    amplitude.clientPersistsDbmRange = true;
    c.panAmplitude = amplitude;
    //DH1KLM No measured squelch map; the SQL line and Auto SQL keep Flex's scale.
    c.squelchLevelScale = legacyDbmSquelchScale();

    //DH1KLM Span follows the sample rate: it snaps by ratio to one of the six DDC0 rates
    //DH1KLM (no continuous zoom); panBandwidthLimitsChanged clamps to that list's ends.
    //DH1KLM radioWide is false: one receiver, so no span budget is shared (unlike the
    //DH1KLM HL2's one DDC in front of every receiver) (#5725).
    PanSpanModel span;
    span.followsSampleRate = true;
    span.radioWide = false;
    c.panSpanModel = span;
    c.hasAgcThreshold = true; // Host receiver DSP implements threshold/off gain.
    c.manufacturer = QStringLiteral("Apache Labs");
    c.model = QStringLiteral("ANAN-G2");
    //DH1KLM The session takes its receive front-end options from AnanSettings
    //DH1KLM (populateFamilyParams()), so the setup page that edits them applies.
    c.adcFrontEnd = RadioCapabilities::AdcFrontEnd{};

    //DH1KLM ---- PA telemetry ----
    //DH1KLM The DC supply rail, published as RAD:+13.8A from every status packet's
    //DH1KLM count (onSupplyRailCounts()). Claimed on a bench measurement, not on the
    //DH1KLM input's label: see kSupplyRailVoltsPerCount for the reading and for why
    //DH1KLM the input is not the one the spec calls "supply voltage".
    c.hasSupplyVoltageTelemetry = true;
    //DH1KLM
    //DH1KLM There is NO PA temperature in this protocol. The radio's whole analog
    //DH1KLM payload is six fields and none of them is a temperature, so the readout
    //DH1KLM can never resolve and is withdrawn rather than left showing a placeholder.
    //DH1KLM hasPaTemperatureTelemetry stays false on its own (nothing is received);
    //DH1KLM this says the stronger thing -- that nothing CAN be.
    c.paTelemetryAudit = RadioCapabilities::PaTelemetryAudit{
        /*temperatureAbsent=*/true};
    c.canCreateSlices = false;
    c.maxSlices = 1;
    c.maxPanadapters = 1;
    for (const int ksps : kDdc0RatesKsps)
        c.sampleRatesHz.append(ksps * 1000);
    //DH1KLM Not reported -- no verified G2 tuning range (RFC: "I have not fetched
    //DH1KLM the Apache Labs G2 manual"). RadioCapabilities.h's own convention:
    //DH1KLM both zero means "not reported", not a guess.
    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    //DH1KLM State is engine-owned, but verified coverage is still unavailable.
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine, 0, 0};
    c.receiveModeControl = std::nullopt; // mode/passband transition contract not yet qualified
    c.receiveFilterControl = std::nullopt;
    //DH1KLM Gain and mute both act on this receiver's audio and are applied by this
    //DH1KLM backend, which is what the record promises. Engine authority: the state is
    //DH1KLM ours, the radio echoes nothing back, and there is no register to read.
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receiveAudioExport = std::nullopt; // native DAX export not qualified for this backend
    c.dspParameterReset = std::nullopt;
    c.receivePanCenterControl = std::nullopt; // center also retunes the slice
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                         48'000, 1'536'000};
    c.canTransmit = false;         // P2Client has no PTT capability -- see class comment
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;        // client-side WDSP, like the HL2
    c.takesTxAudioOverSeam = true; // moot while canTransmit is false
    //DH1KLM Host-modulated like the HL2, so when this backend gains a drive path it
    //DH1KLM will own the register and report intent, not a readback (#5518). Declared
    //DH1KLM rather than left absent because the ownership answer is already known; it
    //DH1KLM populates no TransmitDelta::rfPower today, so nothing publishes drive yet.
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine, /*tunePowerAppliesLive=*/false};
    c.hasRadioPttReadback = false; // no PTT at all, so no readback either
    c.radioPttObservation = std::nullopt;
    c.hasTuner = false;            // G2 has no internal ATU (Apache Labs spec)
    c.hasTunerMemories = false;    // no internal ATU, so no tuner-memory surface
    c.hasAmplifier = false;
    c.hasRadioSideDsp = false;     // DSP is engine-side (AnanRxDsp), not firmware
    c.hasAudioPeakingFilter = false; // no firmware APF verb on this path
    c.hasHostNoiseBlanker = true;  // WDSP ANB on the raw IQ, in AnanRxDsp
    c.radioOwnsDbmScale = false;   // client computes it from raw IQ
    c.hasDdcPanEdgeRolloff = true; // see RadioCapabilities.h's own comment
    c.backendPanAveraging = BackendPanAveraging{kMsPerAverageStep, false, {}, {}}; // AnanPanAnalyzer
    c.panFrameRateShaping = std::nullopt;  // no client owner is declared for FFT FPS
    //DH1KLM No protocol field holds a waterfall rate: the client keeps it.
    c.panWaterfallRateShaping =
        PanWaterfallRateShaping{/*clientPersistsWaterfallRate*/ true};
    //DH1KLM No band/segment zoom: the protocol carries no per-pan zoom flag.
    c.panZoomModes = std::nullopt;
    c.persistsMemories = false;    // default; stated explicitly
    //DH1KLM The radio reports no frequency, mode or output level of its own, so the
    //DH1KLM client is their only memory, per radio.
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RfGain
                              | RadioCapabilities::ClientSettingsDomain::Tuning
                              | RadioCapabilities::ClientSettingsDomain::ReceiveOutputLevel;
    c.hostDroopCalibration = true; // AnanDroopCorrection.h -- real DDC0 roll-off,
                                    //DH1KLM corrected client-side via AnanDroopCalibrator
    c.extensionNamespaces = {QStringLiteral("anan")};  // "frontend.set", "droop.apply" -- see invokeExtension()
    //DH1KLM From this session's own Discovery reply (P2Client::discoveryInfoReceived()).
    //DH1KLM Absent until it lands ("not reported yet", like tuningMinHz/MaxHz);
    //DH1KLM capabilitiesChanged() fires when it does. numDdc is informational:
    //DH1KLM maxSlices/maxPanadapters stay 1 because only DDC0 is driven.
    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("numDdc")] = m_discoveredNumDdc;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        c.extensions[QStringLiteral("anan")] = anan;
    }
    return c;
}

void AnanBackend::applyRestoredState(const RestoredRadioState& state)
{
    //DH1KLM A missing document resets both ADCs on a same-family radio swap.
    //DH1KLM RadioStateMemory owns persistence; validate its opaque extension here.
    const QJsonObject gain = state.extension.value(QStringLiteral("rfGain")).toObject();
    m_pendingParams.adc0AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc0AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_pendingParams.adc1AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc1AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb : m_pendingParams.adc0AttenuationDb;

    //DH1KLM Unconditional, like the ADC values above, so a same-family backend reuse
    //DH1KLM cannot carry radio A's dial or level onto radio B. A rejected value leaves
    //DH1KLM the sentinel, and connectRadio() falls to the first-connect default.
    m_restoredFreqHz = isRestorableFrequencyHz(state.rfFrequencyHz)
        ? state.rfFrequencyHz : 0.0;
    m_restoredMode = isKnownModeString(state.mode) ? state.mode.toUpper() : QString();
    m_lineoutGainPercent = state.receiveOutputLevelPct >= 0
        ? state.receiveOutputLevelPct : kDefaultLineoutGainPercent;
}

RestoredRadioState AnanBackend::currentOperatingState() const
{
    RestoredRadioState state;
    //DH1KLM The live dial and level, not the restored ones. A level of 0 is reported:
    //DH1KLM a silenced speaker is a choice, and "nothing to remember" is -1.
    state.rfFrequencyHz = m_sliceFreqHz;
    state.mode = m_mode;
    state.receiveOutputLevelPct = m_lineoutGainPercent;
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
        {QStringLiteral("adc0AttenuationDb"), m_pendingParams.adc0AttenuationDb},
        {QStringLiteral("adc1AttenuationDb"), m_pendingParams.adc1AttenuationDb}}}};
    return state;
}

void AnanBackend::connectRadio(const RadioConnectRequest& request)
{
    //DH1KLM: The original path is exclusively the physically discovered Saturn/P2 board.
    if (request.params.value(QStringLiteral("anan.boardId"), -1).toInt() != 10
        || request.params.value(QStringLiteral("anan.protocol"), 0).toInt() != 2) {
        emit connectionError(QStringLiteral("Saturn (ANAN G2) — tropo1234 requires a Saturn board (ID 10) on Protocol 2."));
        return;
    }
    const QHostAddress hostAddr(request.host);
    if (hostAddr.isNull()) {
        emit connectionError(QStringLiteral("ANAN: invalid host '%1'").arg(request.host));
        return;
    }
    if (m_connected)
        disconnectRadio();

    //DH1KLM A fresh connect (possibly to a different host) must not keep reporting
    //DH1KLM a prior radio's discovered identity until its own reply lands -- see
    //DH1KLM capabilities()'s own comment.
    m_discoveryInfoReceived = false;
    m_discoveredBoardId = 0;
    m_discoveredFirmwareVer = 0;
    m_discoveredNumDdc = 0;

    //DH1KLM Per-radio identity for RadioSettingsScope (droop calibration -- see
    //DH1KLM invokeExtension()'s "droop.apply" handler). Set BEFORE the seed load
    //DH1KLM just below, and before anything else in this function needs it,
    //DH1KLM matching Hl2Backend's own m_radioSerial assignment ordering.
    m_radioSerial = request.serial;
    if (m_dsp) {
        //DH1KLM Forget the PREVIOUS radio's tables before seeding this one's. The
        //DH1KLM seed below only inserts, and m_dsp is constructed once for the
        //DH1KLM lifetime of this backend -- so without this, connecting a second,
        //DH1KLM uncalibrated G2 in the same session renders it through the first
        //DH1KLM one's per-bin corrections. See
        //DH1KLM AnanRxDsp::clearDroopCorrectionTables().
        QMetaObject::invokeMethod(m_dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);

        auto pushTable = [this](int rateKsps, const DroopCorrectionTable& t) {
            QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, rateKsps),
                Q_ARG(std::vector<float>, std::vector<float>(t.begin(), t.end())));
        };

        //DH1KLM Shipped defaults FIRST, so an uncalibrated radio is corrected on first
        //DH1KLM connect (one gateware-derived curve for all rates, AnanDroopDefaults.h). Both
        //DH1KLM lists come from kDdc0RatesKsps, so the null test is a structural guard only.
        for (const int rateKsps : defaultDroopRatesKsps()) {
            if (const DroopCorrectionTable* t = defaultDroopTableForRate(rateKsps))
                pushTable(rateKsps, *t);
        }

        //DH1KLM Then THIS radio's own bench calibration on top, per rate. An
        //DH1KLM operator who measured their own hardware always outranks a shipped
        //DH1KLM default, and a partial sweep only overrides the rates it actually
        //DH1KLM covered rather than wiping the rest back to the default.
        const auto tables = AnanDroopCalibrator::loadTables(
            RadioSettingsScope(QStringLiteral("anan-tropo1234"), m_radioSerial));
        for (auto it = tables.constBegin(); it != tables.constEnd(); ++it)
            pushTable(it.key(), it.value());
    }

    m_pendingParams.host = request.host;
    m_pendingParams.ddc0RateKsps =
        request.params.value(QStringLiteral("anan.ddc0RateKsps"), 48).toInt();
    //DH1KLM Connect-time-only ADC options -- see P2Client::Params' own comment
    //DH1KLM for why none of these have a live setter. Defaults match
    //DH1KLM AnanSettings' own defaults, so a caller that never populated these
    //DH1KLM params (a picker/auto-reconnect connect, same reasoning as
    //DH1KLM ddc0RateKsps above) still gets sane values, not zeroed-out ones.
    m_pendingParams.ditherEnabled =
        request.params.value(QStringLiteral("anan.ditherEnabled"), true).toBool();
    m_pendingParams.randomEnabled =
        request.params.value(QStringLiteral("anan.randomEnabled"), true).toBool();
    m_pendingParams.ddc0AdcIndex =
        request.params.value(QStringLiteral("anan.ddc0AdcIndex"), 0).toInt() == 1 ? 1 : 0;
    m_pendingParams.bypassAdc0Filters =
        request.params.value(QStringLiteral("anan.bypassAdc0Filters"), true).toBool();
    m_pendingParams.bypassAdc1Filters =
        request.params.value(QStringLiteral("anan.bypassAdc1Filters"), true).toBool();
    //DH1KLM FALSE by default, unlike the options above, and for the opposite reason:
    //DH1KLM their default is the hardware's shipped state, while this one's is "the
    //DH1KLM operator has not asked for an outbound stream". A connect that never
    //DH1KLM populated this param must not start one.
    m_pendingParams.speakerAudioEnabled =
        request.params.value(QStringLiteral("anan.speakerAudioEnabled"), false).toBool();
    //DH1KLM Live once connected ("frontend.set" below), and seeded here so the
    //DH1KLM session's first High Priority packet already carries them. FALSE by
    //DH1KLM default: both defaults are the bytes this client sent before either
    //DH1KLM option existed.
    m_pendingParams.bandFiltersEnabled =
        request.params.value(QStringLiteral("anan.bandFiltersEnabled"), false).toBool();
    m_pendingParams.speakerAmpMuted =
        request.params.value(QStringLiteral("anan.speakerAmpMuted"), false).toBool();
    //DH1KLM applyRestoredState() seeds both ADC values before this connect.
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb
        : m_pendingParams.adc0AttenuationDb;

    //DH1KLM An explicit connect frequency, then this radio's restored one, then the
    //DH1KLM first-connect default.
    if (request.params.contains(QStringLiteral("anan.rxFrequencyHz"))) {
        m_sliceFreqHz = request.params.value(QStringLiteral("anan.rxFrequencyHz")).toDouble();
    } else if (m_restoredFreqHz > 0.0) {
        m_sliceFreqHz = m_restoredFreqHz;
    } else {
        m_sliceFreqHz = kFirstConnectFrequencyHz;
    }
    //DH1KLM Before m_pendingDspConfig is built below, which reads m_mode. No restored
    //DH1KLM mode means the default, never the previous radio's m_mode. The passband
    //DH1KLM resets only on a mode change, as in setSliceMode().
    const QString mode = m_restoredMode.isEmpty()
        ? QString::fromLatin1(kFirstConnectMode) : m_restoredMode;
    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    m_mode = mode;

    m_speakerAudioEnabled = m_pendingParams.speakerAudioEnabled;
    m_pendingDspConfig = AnanRxDsp::Config{};
    m_pendingDspConfig.inputSampleRateHz = m_pendingParams.ddc0RateKsps * 1000;
    m_pendingDspConfig.audioSampleRateHz = 24000;
    m_pendingDspConfig.dspBlockSize = 1024;
    //DH1KLM The panel's width in points (setPanPixelWidth()), or one point per
    //DH1KLM droop-table entry until the GUI has reported one; the analyzer's FFT
    //DH1KLM behind them is larger (see AnanPanAnalyzer). spectrumFps keeps Config's
    //DH1KLM default until RadioModel pushes the operator's rate through
    //DH1KLM setPanFrameRate().
    m_pendingDspConfig.panPoints = m_panPoints;
    //DH1KLM This backend retains the AGC and NB requests across reconnects.
    //DH1KLM emitSliceState() also supplies them if a different radio requires a
    //DH1KLM fresh slice.
    applyLiveRxState(m_pendingDspConfig);

    ++m_connectGeneration;
    beginDspSetup();
}

void AnanBackend::applyLiveRxState(AnanRxDsp::Config& config) const
{
    config.mode = modeFromString(m_mode);
    config.filterLowHz = static_cast<double>(m_filterLowHz) + cwBfoHz();
    config.filterHighHz = static_cast<double>(m_filterHighHz) + cwBfoHz();
    config.agcMode = m_agcMode;
    config.maximumAgcGainDb = agcKnobDb(m_agcThreshold);
    config.agcFixedGainDb = agcKnobDb(m_agcOffLevel);
    config.noiseBlanker = AetherSDR::toWdsp(m_nbKind);
    config.noiseBlankerLevel = m_nbLevel;
    config.noiseBlankerFill = AetherSDR::toWdsp(m_nbFill);
}

void AnanBackend::beginDspSetup()
{
    //DH1KLM Build on m_dspBuildThread, not m_ioThread: buildChannel() can take ~19 s cold
    //DH1KLM (FFTW PATIENT), and a disconnect/close in that window must not block on a
    //DH1KLM BlockingQueuedConnection to m_client queued behind configure().
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    //DH1KLM Seed the requested startup state before the off-thread build. The
    //DH1KLM install path reapplies the object's current state so operator edits made
    //DH1KLM during a build win; without this first-connect seed it would reapply the
    //DH1KLM Config member defaults over cfg instead.
    QMetaObject::invokeMethod(m_dsp, [this, generation, cfg]() {
        m_dsp->beginInitialBuild(cfg);

        QMetaObject::invokeMethod(m_dspBuildContext, [this, generation, cfg]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP configure failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(m_dsp, [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, generation, ok, errStr]() {
                finishDspSetup(generation, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishDspSetup(quint64 generation, bool ok, const QString& error)
{
    //DH1KLM A newer connectRadio() or a disconnectRadio() happened while
    //DH1KLM configure() was running on the I/O thread -- this result is stale.
    if (generation != m_connectGeneration)
        return;

    if (!ok) {
        emit connectionError(QStringLiteral("ANAN: DSP configure failed: %1")
                             .arg(error.isEmpty() ? QStringLiteral("unknown error") : error));
        //DH1KLM Only ever reached from connectRadio()'s first-connect path now --
        //DH1KLM beginRateChange() builds the new channel off-thread and never
        //DH1KLM calls beginDspSetup()/finishDspSetup() -- so m_rateChanging is
        //DH1KLM always false here. Kept for symmetry with
        //DH1KLM startP2ClientSession()'s own failure handling rather than because
        //DH1KLM this branch can currently observe a rate change in flight.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        emitPanState();
        if (wasRateChange)
            retryPendingRateChange();
        return;
    }

    startP2ClientSession(generation);
}

void AnanBackend::startP2ClientSession(quint64 generation)
{
    //DH1KLM Rebuilt here rather than at connect, because this is the one place BOTH a
    //DH1KLM first connect and a rate change pass through. A converter carries filter
    //DH1KLM state, and the audio either side of a DSP rebuild is not one continuous
    //DH1KLM stream, so carrying that state across would ring the tail of the old rate
    //DH1KLM into the head of the new one.
    resetSpeakerResamplers();
    const P2Client::Params params = m_pendingParams;
    const bool isRateChange = m_rateChanging;
    QMetaObject::invokeMethod(m_client, [this, generation, params, isRateChange]() {
        const bool started = isRateChange
            ? m_client->start(params, kRateChangeConnectTimeoutMs)
            : m_client->start(params);
        if (!started) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (generation != m_connectGeneration)
                    return;
                emit connectionError(QStringLiteral("ANAN: could not open the UDP socket"));
                if (m_pendingBandwidthKsps != 0) {
                    //DH1KLM Same retry-window fix as finishRateChange()'s failure
                    //DH1KLM branch (see its comment) -- a socket-open failure
                    //DH1KLM right after a background rebuild can hit the exact
                    //DH1KLM same hazard: clearing m_rateChanging here, before the
                    //DH1KLM queued zoom retries, would let this failed attempt's
                    //DH1KLM own teardown emit an unsuppressed connected()/
                    //DH1KLM disconnected() pair before the retry re-arms it.
                    retryPendingRateChange();
                } else {
                    m_rateChanging = false;
                    emitPanState();
                }
            }, Qt::QueuedConnection);
            return;
        }
        //DH1KLM Push the operator's current tune once the session is up.
        //DH1KLM linkUp() (P2Client's own signal, connected in the constructor)
        //DH1KLM is what actually fires connected()/emitSliceState()/emitPanState()
        //DH1KLM once the first genuine DDC0 frame arrives -- not this call.
        if (m_sliceFreqHz > 0.0)
            m_client->setDdc0FrequencyHz(m_sliceFreqHz);
    }, Qt::QueuedConnection);
}

void AnanBackend::disconnectRadio()
{
    retirePcmStreams();
    m_droopCalibrator.stop(false);
    m_droopCalibrator.setLandedRate(0);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup callback
    //DH1KLM Blocking, like ~AnanBackend() and closeEvent()'s other worker stops: the caller
    //DH1KLM may tear down GUI objects or this backend next (the window X button runs this
    //DH1KLM synchronously with the event loop pumping), so the I/O thread must have closed
    //DH1KLM the socket first. disconnected() still arrives via linkDown().
    if (m_client)
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    m_pendingBandwidthKsps = 0;
    //DH1KLM The next session's counters restart from zero in P2Client::start(), so a
    //DH1KLM cached snapshot from this one would make the first tick of the next read
    //DH1KLM as a huge backward jump in every total.
    m_linkCounters = {};
    m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = 0;
    //DH1KLM A status packet can arrive before the next session's linkUp, so the
    //DH1KLM rail's window is emptied here as well as there.
    resetSupplyRailWindow();
    //DH1KLM A genuine operator-initiated disconnect always fires disconnected(),
    //DH1KLM even one that lands mid-rate-change -- this is not the zoom case
    //DH1KLM m_rateChanging exists to hide.
    m_rateChanging = false;
    //DH1KLM m_audioMuted lives on AnanRxDsp and survives configure() (see that
    //DH1KLM function's own comment), so a disconnect landing mid-rate-change --
    //DH1KLM while audio was deliberately muted for the settle window -- would
    //DH1KLM otherwise leave the NEXT session starting muted with nothing left to
    //DH1KLM ever clear it.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, false));
    //DH1KLM The droop tables are per-RADIO and m_dsp outlives any one connection,
    //DH1KLM so they go with the radio they were measured on. Clearing here (as well
    //DH1KLM as before connectRadio()'s seed) means a disconnected session cannot
    //DH1KLM leave a stale correction armed for whatever connects next, by any path.
    //DH1KLM The bypass flag is cleared too: a disconnect mid-sweep stops the
    //DH1KLM calibrator (the backend's disconnect handler) but its
    //DH1KLM finishSweep() cannot reach a backend that is already gone.
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionBypassed",
                                  Qt::QueuedConnection, Q_ARG(bool, false));
    }
    //DH1KLM linkDown() (constructor-wired) sets m_connected = false and emits
    //DH1KLM disconnected() once P2Client::stop() actually runs.
}

void AnanBackend::pushModeFilterShift()
{
    if (!m_dsp)
        return;
    const double bfo = cwBfoHz();
    QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
        Q_ARG(WdspChannel::Mode, modeFromString(m_mode)));
    QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
        Q_ARG(double, static_cast<double>(m_filterLowHz) + bfo),
        Q_ARG(double, static_cast<double>(m_filterHighHz) + bfo));
    //DH1KLM No NCO-vs-slice offset in this backend (see the class comment) --
    //DH1KLM the shift is exactly the CW BFO, negated (HERMES §5: the shift names
    //DH1KLM the RF frequency the detector treats as zero, so pushing that zero
    //DH1KLM DOWN a pitch is what lifts the marker UP onto it).
    QMetaObject::invokeMethod(m_dsp, "setShift", Qt::QueuedConnection, Q_ARG(double, -bfo));
}

void AnanBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    m_sliceFreqHz = hz;
    //DH1KLM Digit readout / slice model: unthrottled. Cheap (no radio round trip,
    //DH1KLM no pan re-layout), and it is what makes a drag gesture feel like it is
    //DH1KLM tracking the mouse at all.
    emitSliceState();
    //DH1KLM Everything that touches the radio or the pan's display geometry goes
    //DH1KLM through the coalescing throttle instead of firing here directly -- see
    //DH1KLM applyTuneToRadioAndPan()'s comment for why: a click/drag-tune gesture
    //DH1KLM calls this once per mouse-move event (tens of times a second), and
    //DH1KLM Phase 1b has no NCO-vs-slice decoupling (the class comment is explicit:
    //DH1KLM "the shift is ALWAYS exactly -cwBfoHz... because the NCO IS the slice
    //DH1KLM frequency, always") -- so unthrottled, EVERY one of those events would
    //DH1KLM retune the actual DDC0 hardware and re-broadcast the pan's geometry.
    scheduleTuneApply();
    //DH1KLM The Tuning capture rides the same throttle: applyTuneToRadioAndPan().
}

void AnanBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);
    //DH1KLM Idempotence: only reset the passband to the mode's default when the
    //DH1KLM mode actually changes, so a repeated set does not clobber an
    //DH1KLM operator's manual filter edit.
    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    m_mode = mode;
    pushModeFilterShift();   // HERMES §16.7: mode changes re-push the passband, every time
    emitSliceState();
    emit operatingStateChanged();   // Tuning capture; a mode change is discrete
}

void AnanBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Q_UNUSED(sliceId);
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_dsp) {
        const double bfo = cwBfoHz();
        QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(lowHz) + bfo),
            Q_ARG(double, static_cast<double>(highHz) + bfo));
    }
    emitSliceState();
}

void AnanBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(sliceId);
    //DH1KLM thresholdDb is the AGC-T knob's 0..100, not dB: agcKnobDb() maps it.
    const QString m = mode.trimmed().toLower();
    int wdspMode = 3;   // medium, WDSP's own default
    if (m == QLatin1String("off"))  wdspMode = 0;
    else if (m == QLatin1String("slow")) wdspMode = 2;
    else if (m == QLatin1String("fast")) wdspMode = 4;
    //DH1KLM Live state for connectRadio() and beginRateChange() -- see the member
    //DH1KLM declaration comment.
    m_agcMode = wdspMode;
    m_agcThreshold = std::clamp(thresholdDb, 0, 100);
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspMode), Q_ARG(double, agcKnobDb(m_agcThreshold)));
    }
    emitSliceState();
}

void AnanBackend::requestSliceAgc(int sliceId, const SliceAgcRequest& request)
{
    if (request.field != SliceAgcRequest::Field::OffLevel) {
        IRadioBackend::requestSliceAgc(sliceId, request);
        return;
    }
    //DH1KLM Sent in every AGC mode: WDSP applies the fixed gain only with AGC off
    //DH1KLM (wcpAGC.c xwcpagc), so a level set first takes effect when AGC goes off.
    m_agcOffLevel = std::clamp(request.offLevel, 0, 100);
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setAgcFixedGain", Qt::QueuedConnection,
            Q_ARG(double, agcKnobDb(m_agcOffLevel)));
    }
    emitSliceState();
}

ReceiveDispatch AnanBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    //DH1KLM #5824 already owns the worker and NB readback. The typed desktop route
    //DH1KLM must consume that implementation, not replace it or invent other DSP.
    if (sliceId != kSliceId || !request.valid()
        || request.feature != SliceDspRequest::Feature::Nb) {
        return ReceiveDispatch::Unsupported;
    }
    //DH1KLM Level and Fill ride along so the one verb always gets the whole state.
    setSliceNoiseBlanker(sliceId, request.requestedBlanker(), request.level, request.fill);
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                                       int level, AetherSDR::NoiseBlankerFill fill)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    m_nbKind = kind;
    m_nbLevel = std::clamp(level, 0, 100);
    m_nbFill = fill;
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setNoiseBlanker", Qt::QueuedConnection,
            Q_ARG(WdspChannel::NoiseBlanker, AetherSDR::toWdsp(m_nbKind)),
            Q_ARG(int, m_nbLevel),
            Q_ARG(WdspChannel::NoiseBlankerFill, AetherSDR::toWdsp(m_nbFill)));
    }
}

ReceiveDispatch AnanBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (sliceId != kSliceId || !request.valid()
        || request.origin != SliceAudioRequest::Origin::Operator) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case SliceAudioRequest::Field::Gain: setSliceAudioGain(sliceId, request.value); break;
    case SliceAudioRequest::Field::Mute: setSliceAudioMute(sliceId, request.value != 0); break;
    case SliceAudioRequest::Field::Pan: setSliceAudioPan(sliceId, request.value); break;
    }
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceAudioMute(int sliceId, bool mute)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    if (m_sliceAudioMuted == mute) {
        return;
    }
    m_sliceAudioMuted = mute;
    //DH1KLM Published, not just stored. The gate that decides whether the control is
    //DH1KLM offered at all requires an OBSERVATION as well as the capability, so a
    //DH1KLM mute that is applied but never echoed leaves the control refused with
    //DH1KLM "capability.unavailable" -- working audio behind a dead control.
    emitSliceState();
}

void AnanBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    Q_UNUSED(sliceId);
    const int bounded = std::clamp(gainPercent, 0, 100);
    if (m_sliceAudioGainPercent == bounded) {
        return;
    }
    m_sliceAudioGainPercent = bounded;
    emitSliceState();
}

void AnanBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    Q_UNUSED(sliceId);
    //DH1KLM NOT published: SliceDelta carries no balance field, so there is no
    //DH1KLM observation to echo and nothing downstream reads one. Applied all the
    //DH1KLM same -- the audio stage is the same stage either way.
    m_sliceAudioPanPercent = std::clamp(panPercent, 0, 100);
}

void AnanBackend::setLineoutGain(int percent)
{
    //DH1KLM The percent runs through the SAME dB law as the per-slice fader
    //DH1KLM (sliceAudioAmplitude, at the send), so 50 here is -20 dB and not half
    //DH1KLM amplitude. Worth knowing when comparing families: a Flex's own lineout
    //DH1KLM route sends `mixer lineout gain 50` and whatever that means at the radio is
    //DH1KLM its own scale, so the same slider position is not promised to be the same
    //DH1KLM loudness across radios. One law for both of OUR level controls is the
    //DH1KLM trade taken -- the alternative is a fader whose feel changes depending on
    //DH1KLM which of the three levels the operator happens to be moving.
    const int wanted = std::clamp(percent, 0, 100);
    if (wanted == m_lineoutGainPercent) {
        return;
    }
    m_lineoutGainPercent = wanted;
    emit operatingStateChanged();   // ReceiveOutputLevel capture
}

void AnanBackend::setLineoutMute(bool mute)
{
    m_lineoutMuted = mute;
}

void AnanBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);   // one pan in this phase
    Q_UNUSED(intent);  // ignored, matching Hl2Backend::setPanCenter
    setSliceFrequency(kSliceId, hz);
}

int AnanBackend::nearestDdc0RateKsps(int requestedKsps) noexcept
{
    //DH1KLM DDC0 runs at exactly one of these rates (no continuous zoom). Nearest by RATIO
    //DH1KLM (log domain), like Hl2Backend::nearestIqSampleRateHz (HERMES.md §15.1): the
    //DH1KLM rates are octave-spaced and zoom is multiplicative, so 140 ksps belongs to 192
    //DH1KLM (geometric mean 135.8), not 96. The equidistant point (e.g. 96*sqrt(2)) is
    //DH1KLM never an integer request, so no tie-break is needed.
    if (requestedKsps <= 0)
        return kDdc0RatesKsps.front();
    int best = kDdc0RatesKsps.front();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int r : kDdc0RatesKsps) {
        const double distance = std::abs(std::log(static_cast<double>(requestedKsps) / r));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = r;
        }
    }
    return best;
}

void AnanBackend::setPanBandwidth(const QString& panId, double hz)
{
    Q_UNUSED(panId);   // one pan in this phase
    if (hz <= 0.0 || !m_connected)
        return;

    const int requestedKsps = static_cast<int>(hz / 1000.0 + 0.5);
    const int snappedKsps = nearestDdc0RateKsps(requestedKsps);

    if (snappedKsps == m_pendingParams.ddc0RateKsps) {
        //DH1KLM Already at the closest rate: republish so an optimistic GUI span snaps back
        //DH1KLM (as PanadapterModel::republishCenterBandwidth(), #4470), and clear any pending
        //DH1KLM request so a stale older zoom cannot fire after this one.
        m_pendingBandwidthKsps = 0;
        emitPanState();
        return;
    }

    if (!m_rateChanging) {
        beginRateChange(snappedKsps);
    } else {
        //DH1KLM A reconfigure is already running -- remember the latest request
        //DH1KLM rather than starting a second one on top of it. See m_rateChanging's
        //DH1KLM own comment for why this waits for that cycle to actually finish
        //DH1KLM rather than a fixed cooldown.
        m_pendingBandwidthKsps = snappedKsps;
    }
}

void AnanBackend::retryPendingRateChange()
{
    if (m_pendingBandwidthKsps == 0)
        return;
    const int ksps = m_pendingBandwidthKsps;
    m_pendingBandwidthKsps = 0;
    beginRateChange(ksps);
}

void AnanBackend::beginRateChange(int newRateKsps)
{
    //DH1KLM A live rate change: build the new WdspChannel on m_dspBuildThread FIRST (up to
    //DH1KLM ~a minute cold) while the old channel and session keep running undisturbed;
    //DH1KLM only then does finishRateChange() mute, swap and send the new rate to the
    //DH1KLM RUNNING session (P2Client::setDdcRateLive()). Build-before-disturb is what
    //DH1KLM keeps zoom from freezing. m_preRateChangeKsps records the running rate so the
    //DH1KLM failure path can roll back: emitPanState() reports from m_pendingDspConfig.
    m_preRateChangeKsps = m_pendingParams.ddc0RateKsps;
    m_pendingParams.ddc0RateKsps = newRateKsps;
    m_pendingDspConfig.inputSampleRateHz = newRateKsps * 1000;
    //DH1KLM Refresh from CURRENT live operator state, not connectRadio()'s
    //DH1KLM connect-time snapshot: m_pendingDspConfig otherwise only ever had its
    //DH1KLM rate touched here, so a rate change silently reverted mode/filter/AGC
    //DH1KLM to whatever they were at connect. m_shiftHz is not part of Config --
    //DH1KLM AnanRxDsp::installChannel() re-applies it separately, after the swap.
    applyLiveRxState(m_pendingDspConfig);

    m_rateChanging = true;
    ++m_connectGeneration;   // orphans any in-flight prior connect/reconfigure/rebuild
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    //DH1KLM Mark "rebuild in flight" on m_dsp's own thread FIRST, so any
    //DH1KLM setMode/setFilter/setAgc/setShift call that lands while the build is
    //DH1KLM running defers pushing to the (still-live, still-playing) old channel
    //DH1KLM instead of blocking on WDSP's process-wide setup mutex -- see
    //DH1KLM AnanRxDsp::beginRebuild()'s own comment. THEN hand the slow build to
    //DH1KLM the dedicated build thread.
    QMetaObject::invokeMethod(m_dsp, [this, generation, cfg]() {
        m_dsp->beginRebuild();

        QMetaObject::invokeMethod(m_dspBuildContext, [this, generation, cfg]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP rebuild failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(m_dsp, [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, generation, ok, errStr]() {
                finishRateChange(generation, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishRateChange(quint64 generation, bool ok, const QString& error)
{
    if (generation != m_connectGeneration)
        return;   // superseded by a newer rate change/connect/disconnect

    if (!ok) {
        //DH1KLM Roll the reported rate back to the one still running BEFORE
        //DH1KLM emitting pan state. AnanDroopCalibrator infers "the rate landed"
        //DH1KLM from pan bandwidth reaching its target; told the failed rate had
        //DH1KLM landed, it would measure the OLD rate's spectrum into the NEW
        //DH1KLM rate's correction table and persist it -- one rate's droop curve
        //DH1KLM applied to another rate's bins, which is exactly the cross-rate
        //DH1KLM corruption anan_rxdsp_handedness_test's Group 6 exists to prevent.
        if (m_preRateChangeKsps > 0) {
            m_pendingParams.ddc0RateKsps = m_preRateChangeKsps;
            m_pendingDspConfig.inputSampleRateHz = m_preRateChangeKsps * 1000;
        }
        emit connectionError(error);
        //DH1KLM Safe to clear m_rateChanging immediately here, unlike
        //DH1KLM startP2ClientSession()'s own failure branch below: the OLD
        //DH1KLM P2Client session was never touched on this path -- beginRateChange()
        //DH1KLM no longer stops it before building (see that function's own
        //DH1KLM comment) -- so there is no live teardown in flight whose
        //DH1KLM linkDown()/linkUp() could leak through unsuppressed. The old
        //DH1KLM channel and old session simply keep running at the OLD rate.
        m_rateChanging = false;
        emitPanState();
        retryPendingRateChange();
        return;
    }

    //DH1KLM The new channel is already installed (queued just before this call). Mute
    //DH1KLM here, not on a later event: frames reach AnanRxDsp::processIqBlock() by
    //DH1KLM DirectConnection on the I/O thread, so anything triggered after the rate
    //DH1KLM lands would be at least one block late.
    QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, true));

    //DH1KLM LIVE rate change on the running session: p2app applies DDC-Specific rate
    //DH1KLM changes by a direct FPGA register write with no teardown (see
    //DH1KLM P2Client::setDdcRateLive()). Until it lands the new channel sees old-rate
    //DH1KLM samples; the mute and settle window cover that. Sent THREE times
    //DH1KLM (kRateChangeResendMs): it is fire-and-forget UDP, the keepalive never resends
    //DH1KLM DDC-Specific, and a lost packet would leave the radio at the old rate with no
    //DH1KLM error while everything here records the new one. Duplicates are no-ops
    //DH1KLM (WriteP2DDCRateRegister() fires on change; HandlerCheckDDCSettings() is
    //DH1KLM empty) (#5547).
    const int rateKsps = m_pendingDspConfig.inputSampleRateHz / 1000;
    auto sendRate = [this, rateKsps, generation]() {
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_client, "setDdcRateLive", Qt::QueuedConnection,
                                  Q_ARG(int, 0), Q_ARG(int, rateKsps));
    };
    sendRate();
    for (const int delayMs : kRateChangeResendMs)
        QTimer::singleShot(delayMs, this, sendRate);

    QTimer::singleShot(kRateChangeLiveSettleMs, this, [this, generation]() {
        //DH1KLM Same generation guard the restart path used: a newer rate
        //DH1KLM change/connect/disconnect during the settle window supersedes this.
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, false));
        m_rateChanging = false;
        emitPanState();
        //DH1KLM A zoom the operator kept turning while this ran: apply the latest
        //DH1KLM request now rather than stranding the display a step behind.
        retryPendingRateChange();
    });
}

void AnanBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);   // one pan in this phase
    //DH1KLM Default no-op inherited from IRadioBackend was written for Flex,
    //DH1KLM whose own display engine paces its frames -- silently wrong here,
    //DH1KLM since it left AnanRxDsp's spectrum production uncapped (measured on
    //DH1KLM the bench: the waterfall scrolled far faster than the operator's own
    //DH1KLM FPS setting, because nothing was ever telling it what that setting
    //DH1KLM was). Mirrors Hl2Backend::setPanFrameRate()'s shape exactly.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumRateFps", Qt::QueuedConnection,
                                  Q_ARG(int, fps));
}

void AnanBackend::setPanRfGain(const QString& panId, int gainDb)
{
    Q_UNUSED(panId);   // one pan in this phase
    //DH1KLM The G2 has no gain stage the host drives, only the step attenuator in
    //DH1KLM front of each ADC (Protocol v4.4 pp.34,36, HP bytes 1442/1443).
    //DH1KLM deskHPSDR offers it as a 0-31 dB attenuation control; here it rides the RF Gain
    //DH1KLM slider, negated, so "more gain" still means "slider right". Clamped
    //DH1KLM rather than refused, per the seam's contract.
    const int attenuation = -std::clamp(gainDb, -kMaxStepAttenuationDb, 0);
    m_attenuationDb = attenuation;
    //DH1KLM Kept in the params too: a rate change that restarts the session
    //DH1KLM resends everything from m_pendingParams.
    if (m_pendingParams.ddc0AdcIndex == 1) {
        m_pendingParams.adc1AttenuationDb = attenuation;
    } else {
        m_pendingParams.adc0AttenuationDb = attenuation;
    }
    if (m_client) {
        QMetaObject::invokeMethod(m_client, "setStepAttenuationDb", Qt::QueuedConnection,
                                  Q_ARG(int, m_pendingParams.ddc0AdcIndex),
                                  Q_ARG(int, attenuation));
    }
    emit panRfGainChanged(kPanId, -attenuation);
    emit operatingStateChanged();
}

void AnanBackend::setPanAverage(const QString& panId, int average)
{
    Q_UNUSED(panId);   // one pan in this phase
    //DH1KLM The operator's FFT AVG slider, which otherwise reaches nothing on this
    //DH1KLM radio: the panadapter is averaged inside WDSP's analyzer, as a TIME --
    //DH1KLM see kMsPerAverageStep.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumAverageMs", Qt::QueuedConnection,
                                  Q_ARG(int, std::max(0, average) * kMsPerAverageStep));
}

void AnanBackend::setPanWeightedAverage(const QString& panId, bool on)
{
    Q_UNUSED(panId);   // one pan in this phase
    //DH1KLM deskHPSDR offers the analyzer's averaging MODE alongside its time. The
    //DH1KLM toggle picks between the two recursive modes: on = log-recursive
    //DH1KLM (deskHPSDR's default, the smoother look), off = linear-recursive (an
    //DH1KLM average of power, which reads noise and weak signals at their true
    //DH1KLM level).
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumLogAverage", Qt::QueuedConnection,
                                  Q_ARG(bool, on));
}

void AnanBackend::setPanPixelWidth(const QString& panId, int pixels)
{
    Q_UNUSED(panId);   // one pan in this phase
    //DH1KLM One analyzer point per screen pixel, as deskHPSDR sizes its analyzer
    //DH1KLM from the panel width. A fixed 1024 points stretched across a wider
    //DH1KLM panel is what made the waterfall look soft.
    if (pixels < 2)
        return;
    m_panPoints = std::min(pixels, AnanPanAnalyzer::kMaxPoints);
    m_pendingDspConfig.panPoints = m_panPoints;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setPanPoints", Qt::QueuedConnection,
                                  Q_ARG(int, m_panPoints));
}

void AnanBackend::setCwPitch(int hz)
{
    m_cwPitchHz = hz;
    //DH1KLM Both the shift AND the filter depend on the pitch -- HERMES §5:
    //DH1KLM "entering or leaving CW has to re-push the shift, not just the
    //DH1KLM filter." Re-derive both from current state rather than adjusting in
    //DH1KLM place, so this is correct whether or not the mode is currently CW.
    pushModeFilterShift();
    emitSliceState();
}

void AnanBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    //DH1KLM canTransmit is false and P2Client has no PTT capability -- there is
    //DH1KLM nothing this method could do even if the engine's TX guard (which
    //DH1KLM gates every call site above this seam) let a key-on request through.
    //DH1KLM A key-on request reaching here at all means that guard was bypassed,
    //DH1KLM which is itself the bug to chase -- not something to silently absorb.
    if (key)
        qWarning("AnanBackend::setKeying(true) called on a receive-only backend "
                 "(canTransmit=false) -- the engine TX guard should have refused this");
}

QString AnanBackend::persistDroopTables(const QMap<int, anan_original::DroopCorrectionTable>& tables)
{
    if (tables.isEmpty())
        return QStringLiteral("the request carried no valid correction table");
    //DH1KLM Never write an empty radio_id row (AGENTS.md): RadioSettingsScope falls
    //DH1KLM back exact-radio -> family-wide on read, so a row written with no
    //DH1KLM identity would be silently adopted by every ANAN that has none of its
    //DH1KLM own -- the same guard Hl2Backend::applyFreqCalPpb() uses. This is the
    //DH1KLM one thing saveTables() cannot judge for itself: a family-wide scope is
    //DH1KLM perfectly VALID, just not what a per-radio calibration wants.
    if (m_radioSerial.isEmpty()) {
        return QStringLiteral("no radio identity yet -- the correction is live "
                              "for this session only");
    }
    return AnanDroopCalibrator::saveTables(
        RadioSettingsScope(QStringLiteral("anan-tropo1234"), m_radioSerial), tables);
}

QVariantMap AnanBackend::droopStatus() const
{
    //DH1KLM EVERY DDC0 rate, tagged with where its correction came from -- not just
    //DH1KLM the swept ones. Reporting only measuredTables() told the operator
    //DH1KLM "nothing measured" while the defaults connectRadio() seeds were live on
    //DH1KLM the display, so a panadapter that looked wrong had no surface anywhere
    //DH1KLM connecting it to a correction that was in fact being applied. The tag
    //DH1KLM is what keeps "derived from the gateware" and "measured on this radio"
    //DH1KLM distinguishable rather than collapsing them into one list.
    QVariantList corrections;
    const auto& measured = m_droopCalibrator.measuredTables();
    for (const int rateKsps : defaultDroopRatesKsps()) {
        const auto it = measured.constFind(rateKsps);
        const bool isMeasured = it != measured.constEnd();
        //DH1KLM A default is only live once connectRadio() has actually seeded it.
        //DH1KLM Before that the rate genuinely carries no correction and must not
        //DH1KLM claim one -- measured results, by contrast, outlive the connection
        //DH1KLM because the calibrator holds them for the session.
        const DroopCorrectionTable* table =
            isMeasured ? &it.value()
                       : (m_connected ? defaultDroopTableForRate(rateKsps) : nullptr);
        if (!table)
            continue;
        const auto [lo, hi] = std::minmax_element(table->begin(), table->end());
        corrections.append(QVariantMap{
            {QStringLiteral("rateKsps"), rateKsps},
            {QStringLiteral("minDb"), *lo},
            {QStringLiteral("maxDb"), *hi},
            {QStringLiteral("source"), isMeasured ? QStringLiteral("measured")
                                                  : QStringLiteral("default")}});
    }
    return {{QStringLiteral("running"), m_droopCalibrator.isRunning()},
            {QStringLiteral("rateIndex"), m_droopCalibrator.rateIndex()},
            {QStringLiteral("totalRates"), m_droopCalibrator.totalRates()},
            {QStringLiteral("hasResult"), m_droopCalibrator.hasResult()},
            {QStringLiteral("percent"), m_droopPercent},
            {QStringLiteral("message"), m_droopMessage},
            {QStringLiteral("corrections"), corrections}};
}

void AnanBackend::publishDroopStatus()
{
    emit extensionStatus(QStringLiteral("anan"), QStringLiteral("droop"), droopStatus());
}

QString AnanBackend::applyDroopTables(const QMap<int, DroopCorrectionTable>& tables)
{
    //DH1KLM Preserve the existing apply/persist behavior. The sweep and its data
    //DH1KLM stay below the seam; clients can apply a result, never inject tables.
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        if (m_dsp) {
            QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, it.key()), Q_ARG(std::vector<float>,
                    std::vector<float>(it.value().begin(), it.value().end())));
        }
    }
    return persistDroopTables(tables);
}

void AnanBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    if (ns == QLatin1String("anan") && verb == QLatin1String("frontend.set")) {
        //DH1KLM The live front-end options. Partial: only the named fields move,
        //DH1KLM so one control cannot overwrite another. A change reaches the radio
        //DH1KLM on the next High Priority packet, within 100 ms.
        //DH1KLM
        //DH1KLM This persists nothing. The ANAN Front End page writes AnanSettings
        //DH1KLM and then invokes this; a caller that only invokes it gets a change
        //DH1KLM that reverts at the next connect, which reads AnanSettings.
        const QVariantMap fields = arg.toMap();
        if (fields.contains(QStringLiteral("bandFiltersEnabled"))) {
            m_pendingParams.bandFiltersEnabled =
                fields.value(QStringLiteral("bandFiltersEnabled")).toBool();
            if (m_client) {
                QMetaObject::invokeMethod(m_client, "setBandFiltersEnabled",
                                          Qt::QueuedConnection,
                                          Q_ARG(bool, m_pendingParams.bandFiltersEnabled));
            }
        }
        if (fields.contains(QStringLiteral("speakerAmpMuted"))) {
            m_pendingParams.speakerAmpMuted =
                fields.value(QStringLiteral("speakerAmpMuted")).toBool();
            if (m_client) {
                QMetaObject::invokeMethod(m_client, "setSpeakerAmpMuted",
                                          Qt::QueuedConnection,
                                          Q_ARG(bool, m_pendingParams.speakerAmpMuted));
            }
        }
        if (requestId != 0) {
            emit extensionResult(requestId, QVariantMap{{QStringLiteral("ok"), true}});
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("nb.get")) {
        if (requestId != 0) {
            const AnanRxDsp::NoiseBlankerState applied = m_dsp
                ? m_dsp->noiseBlankerState() : AnanRxDsp::NoiseBlankerState{};
            const QVariantMap receiver{
                {QStringLiteral("ddc"), 0},
                {QStringLiteral("panId"), kPanId},
                //DH1KLM `on` stays, and stays first: it is what every existing script
                //DH1KLM reads. `kind` is the same fact with the second blanker in it.
                {QStringLiteral("on"), applied.on()},
                {QStringLiteral("kind"), static_cast<int>(applied.kind)},
                {QStringLiteral("level"), applied.level},
                {QStringLiteral("fill"), static_cast<int>(applied.fill)},
                {QStringLiteral("requestedOn"),
                 m_nbKind != AetherSDR::NoiseBlankerKind::Off},
                {QStringLiteral("requestedKind"), static_cast<int>(m_nbKind)},
                {QStringLiteral("requestedLevel"), m_nbLevel},
                {QStringLiteral("requestedFill"), static_cast<int>(m_nbFill)},
                {QStringLiteral("hasChain"), applied.hasChain},
                {QStringLiteral("threshold"),
                 WdspChannel::noiseBlankerThresholdForLevel(applied.level)},
            };
            emit extensionResult(requestId, QVariantMap{
                {QStringLiteral("receivers"), QVariantList{receiver}},
            });
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb.startsWith(QLatin1String("droop."))) {
        if (!capabilities().hostDroopCalibration || !m_connected || m_radioSerial.isEmpty()) {
            emit extensionError(requestId, QStringLiteral("connect an ANAN before calibrating"));
            return;
        }
        const QString action = verb.mid(6);
        if (action == QLatin1String("status")) {
            emit extensionResult(requestId, droopStatus());
            return;
        }
        if (action != QLatin1String("start") && action != QLatin1String("stop")
            && action != QLatin1String("apply") && action != QLatin1String("discard")) {
            emit extensionError(requestId, QStringLiteral("unknown droop calibration action"));
            return;
        }
        if (m_droopCalibrator.isRunning() && action != QLatin1String("stop")) {
            emit extensionError(requestId, QStringLiteral("a sweep is already running -- stop it first"));
            return;
        }
        QString failure;
        const QMetaObject::Connection errorConnection = connect(
            &m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [&failure](const QString& reason) { failure = reason; });
        m_droopMessage.clear();
        if (action == QLatin1String("start")) {
            m_droopCalibrator.start();
        } else if (action == QLatin1String("stop")) {
            m_droopCalibrator.stop();
        } else if (action == QLatin1String("apply")) {
            m_droopCalibrator.applyResult();
        } else {
            m_droopCalibrator.clear();
            m_droopPercent = 0;
            m_droopMessage = QStringLiteral("Discarded — no sweep result staged.");
        }
        QObject::disconnect(errorConnection);
        publishDroopStatus();
        if (!failure.isEmpty()) {
            emit extensionError(requestId, failure);
        } else {
            emit extensionResult(requestId, droopStatus());
        }
        return;
    }
    if (requestId != 0) {
        emit extensionError(requestId, QStringLiteral("ANAN: unknown extension verb '%1.%2'")
                                           .arg(ns, verb));
    }
}

void AnanBackend::defineMeters()
{
    //DH1KLM Index is ours to choose -- nothing on a P2 radio assigns meter ids. Same
    //DH1KLM name, unit and range as Hl2Backend::defineMeters()'s receive meter, the
    //DH1KLM bare "SLC:LEVEL" MeterModel binds to the S-meter. Receive-only: no TX
    //DH1KLM meters until this backend can transmit.
    MeterDef d;
    d.index = 1;
    d.source = QStringLiteral("SLC");
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level");
    emit meterDefined(d);

    //DH1KLM The supply rail. "+13.8A" is the name MeterModel binds to the status bar
    //DH1KLM and Meter applet voltage readouts, whatever the radio; unit and range as
    //DH1KLM IcomMeters declares the same meter.
    MeterDef rail;
    rail.index = 2;
    rail.source = QStringLiteral("RAD");
    rail.name = QStringLiteral("+13.8A");
    rail.unit = QStringLiteral("Volts");
    rail.low = 0.0;
    rail.high = 16.0;
    rail.description = QStringLiteral("DC supply voltage");
    emit meterDefined(rail);
}

IRadioBackend::LinkStats AnanBackend::linkStats() const
{
    LinkStats s;
    //DH1KLM Nothing measured yet: the consumer keeps whatever source it already had
    //DH1KLM rather than being handed a snapshot of zeros.
    if (!m_connected || !m_linkCountersSeen)
        return s;
    s.reported = true;
    s.alive = m_linkCounters.rxBytes > m_lastSnapshotRxBytes;
    s.rxBytes = static_cast<qint64>(m_linkCounters.rxBytes);
    s.txBytes = static_cast<qint64>(m_linkCounters.txBytes);
    s.rxPackets = m_linkCounters.rxPackets;
    s.rxPacketsLost = m_linkCounters.drops;
    s.localEndpoint = m_linkCounters.localEndpoint;
    //DH1KLM LEFT AT -1, WHICH THE STRUCT DEFINES AS "NOT MEASURED" AND THE MODEL
    //DH1KLM HONOURS BY IGNORING. This transport is stream-only: the radio is told
    //DH1KLM where to send and then sends, with no request/response exchange to time,
    //DH1KLM so there is no round trip here to measure. Reporting 0 instead would
    //DH1KLM render as a perfect "< 1 ms" link -- a measurement never taken.
    //DH1KLM rttMs, jitterMs, gapMs and gapMaxMs all keep their -1 defaults.
    return s;
}

void AnanBackend::onLinkCounters(const P2Client::LinkCounters& counters)
{
    //DH1KLM A session restart shows as a BACKWARD jump, since P2Client::start() zeroes
    //DH1KLM its counters; the cache is then a dead session's and is discarded here,
    //DH1KLM whichever path restarted it. The first snapshot of a session compares
    //DH1KLM against ZERO: against itself, it would read dead on the first tick.
    if (counters.rxBytes < m_linkCounters.rxBytes)
        m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = m_linkCountersSeen ? m_linkCounters.rxBytes : 0;
    m_linkCounters = counters;
    m_linkCountersSeen = true;
    emit linkStatsUpdated(linkStats());
}

void AnanBackend::onDspMeter(float dbfs)
{
    const double dbm = static_cast<double>(dbfs) + kSMeterDbmOffset + m_attenuationDb;
    //DH1KLM Smooth EVERY reading, publish only on the tick -- the same smoother,
    //DH1KLM and the same reasons, as Hl2Backend's receive meter (SMeterSmoother).
    //DH1KLM AnanRxDsp reads the tap at one DSP-rate block's cadence whatever the
    //DH1KLM DDC0 rate (WdspSMeter::emitEveryBlocks), so this sees ~47 readings a
    //DH1KLM second at 48 and at 1536 ksps alike.
    if (const auto out = m_sMeter.feed(dbm))
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), *out);
}

void AnanBackend::onSupplyRailCounts(int counts)
{
    if (!m_supplyRailClock.isValid())
        m_supplyRailClock.start();
    onSupplyRailCountsAt(counts, m_supplyRailClock.elapsed());
}

void AnanBackend::onSupplyRailCountsAt(int counts, qint64 nowMs)
{
    //DH1KLM A 12-bit ADC reads 0..4095. Anything else means the field is not this
    //DH1KLM input, and no reading is better than a plausible wrong voltage.
    if (counts < 0 || counts > 4095)
        return;
    m_supplyRailCountSum += counts;
    ++m_supplyRailSamples;
    //DH1KLM The first window opens on its first count; each later one opens where the
    //DH1KLM last closed, so publications stay a fixed window apart rather than
    //DH1KLM drifting by a packet interval each time.
    if (m_supplyRailWindowStartMs < 0)
        m_supplyRailWindowStartMs = nowMs;
    if (nowMs - m_supplyRailWindowStartMs < kSupplyRailWindowMs)
        return;
    const double meanCounts =
        static_cast<double>(m_supplyRailCountSum) / m_supplyRailSamples;
    m_supplyRailCountSum = 0;
    m_supplyRailSamples = 0;
    m_supplyRailWindowStartMs = nowMs;
    emit meterUpdate(QStringLiteral("RAD:+13.8A"), meanCounts * kSupplyRailVoltsPerCount);
}

void AnanBackend::resetSupplyRailWindow()
{
    m_supplyRailWindowStartMs = -1;
    m_supplyRailCountSum = 0;
    m_supplyRailSamples = 0;
}

void AnanBackend::emitSliceState()
{
    SliceDelta d;
    d.frequency = m_sliceFreqHz / 1.0e6;   // SliceDelta::frequency is MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.active = true;
    //DH1KLM A different radio replaces the slice model but retains this backend.
    //DH1KLM Publish the retained NB request so that fresh model agrees with the DSP.
    d.nb = m_nbKind != AetherSDR::NoiseBlankerKind::Off;
    d.nbKind = m_nbKind;
    d.nbLevel = m_nbLevel;
    d.nbFill = m_nbFill;
    //DH1KLM The AGC as the DSP runs it, so the slice's knob starts from this
    //DH1KLM backend's values, not SliceModel's defaults.
    d.agcMode = agcModeName(m_agcMode);
    d.agcThreshold = m_agcThreshold;
    d.agcOffLevel = m_agcOffLevel;
    //DH1KLM The audio stage as APPLIED. Both are required, not cosmetic: the receive
    //DH1KLM control gate refuses an operation whose observation is absent, so without
    //DH1KLM these two the mute and the fader are offered and then rejected.
    //DH1KLM
    //DH1KLM Safe to echo unconditionally -- SliceModel::applyChanges() assigns these
    //DH1KLM into its observation without re-emitting a command, so a published value
    //DH1KLM cannot loop back as a fresh intent (Principle II).
    d.audioGain = m_sliceAudioGainPercent;
    d.audioMute = m_sliceAudioMuted;
    //DH1KLM Without this, RadioModel::sliceChanged's handler never assigns the
    //DH1KLM slice a panId (SliceDelta::panId is std::optional and SliceModel::
    //DH1KLM applyChanges() only touches it when set) -- the slice materialised by
    //DH1KLM "Gap B" stays permanently unassociated with the pan emitPanState()
    //DH1KLM creates, and every click/drag-tune on the panadapter silently no-ops
    //DH1KLM because MainWindow_Wiring.cpp's tune-target resolver matches on
    //DH1KLM panId equality. Hl2Backend::emitSliceState() sets this every time for
    //DH1KLM the same reason.
    d.panId = kPanId;
    emit sliceChanged(kSliceId, d);
}

void AnanBackend::emitPanState()
{
    const double sampleRateHz = static_cast<double>(m_pendingDspConfig.inputSampleRateHz > 0
        ? m_pendingDspConfig.inputSampleRateHz : 48000);
    //DH1KLM Reports the TRUE rate, not a cropped fraction: the pan widget's zoom math uses
    //DH1KLM this as its baseline, and a reduced value stops zoom-out from ever crossing to
    //DH1KLM the next rate. The decimation roll-off stays visible (and is corrected by the
    //DH1KLM droop tables).
    m_droopCalibrator.setLandedRate(static_cast<int>(sampleRateHz / 1000.0));
    emit panCenterBandwidthChanged(kPanId, m_sliceFreqHz / 1.0e6, sampleRateHz / 1.0e6);
}

void AnanBackend::resetSpeakerResamplers()
{
    m_speakerResampleL.reset();
    m_speakerResampleR.reset();
    m_speakerOutL.clear();
    m_speakerOutR.clear();
    if (!m_speakerAudioEnabled) {
        return;
    }
    const int src = m_pendingDspConfig.audioSampleRateHz;
    if (src <= 0 || src == kSpeakerSampleRateHz) {
        //DH1KLM Already at the stream's rate: no converter, and none is built rather
        //DH1KLM than a 1:1 one being built and trusted to be transparent. A ratio-1
        //DH1KLM resampler still costs a filter and still adds its group delay.
        return;
    }
    m_speakerResampleL = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
    m_speakerResampleR = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
}

void AnanBackend::sendSpeakerAudioToRadio(const QByteArray& stereoFloat)
{
    if (!m_speakerAudioEnabled || m_client == nullptr) {
        return;
    }
    const std::size_t frames =
        static_cast<std::size_t>(stereoFloat.size()) / (2 * sizeof(float));
    if (frames == 0) {
        return;
    }
    const auto* in = reinterpret_cast<const float*>(stereoFloat.constData());

    //DH1KLM Split before converting. The two channels have to travel through separate
    //DH1KLM converters to keep their difference (see the member declaration), and a
    //DH1KLM deinterleaved copy is what a mono converter takes.
    m_speakerSrcL.resize(frames);
    m_speakerSrcR.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        m_speakerSrcL[i] = in[i * 2];
        m_speakerSrcR[i] = in[i * 2 + 1];
    }

    const float* outL = m_speakerSrcL.data();
    const float* outR = m_speakerSrcR.data();
    std::size_t outFrames = frames;
    if (m_speakerResampleL && m_speakerResampleR) {
        const int nL = m_speakerResampleL->process(m_speakerSrcL.data(),
                                                   static_cast<int>(frames), m_speakerOutL);
        const int nR = m_speakerResampleR->process(m_speakerSrcR.data(),
                                                   static_cast<int>(frames), m_speakerOutR);
        //DH1KLM Identical configuration and identical input length, so these agree;
        //DH1KLM taking the shorter is not a correction but a refusal to walk off the
        //DH1KLM end of one buffer if they ever stop agreeing. A converter's first
        //DH1KLM calls legitimately return 0 while its filter fills.
        const int n = std::min(nL, nR);
        if (n <= 0) {
            return;
        }
        outFrames = static_cast<std::size_t>(n);
        outL = reinterpret_cast<const float*>(m_speakerOutL.constData());
        outR = reinterpret_cast<const float*>(m_speakerOutR.constData());
    }

    //DH1KLM The RADIO's own level, on top of the per-slice stage; applied here so turning
    //DH1KLM the radio down never quiets the computer's speakers. A mute sends SILENCE, it
    //DH1KLM does not stop sending: stopping would underflow the codec FIFO (polluting the
    //DH1KLM underflow signal and clicking on unmute) and invalidate the pacer's estimate.
    const float lineout = m_lineoutMuted ? 0.0f
                                         : sliceAudioAmplitude(m_lineoutGainPercent);

    //DH1KLM Interleave and quantise. sliceAudioToInt16() carries the
    //DH1KLM clamp-before-scale law and the non-finite guard, with the tests for both.
    m_speakerInterleaved.resize(outFrames * 2);
    for (std::size_t i = 0; i < outFrames; ++i) {
        m_speakerInterleaved[i * 2] = sliceAudioToInt16(outL[i] * lineout);
        m_speakerInterleaved[i * 2 + 1] = sliceAudioToInt16(outR[i] * lineout);
    }

    //DH1KLM One queued hand-off per block. P2Client owns the socket, the packetising
    //DH1KLM and the pacing on its own thread; this side owns the DSP and the audio
    //DH1KLM stage. Copying the block is the price of that boundary and it is a few
    //DH1KLM hundred bytes.
    QMetaObject::invokeMethod(m_client, "enqueueSpeakerAudio", Qt::QueuedConnection,
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(m_speakerInterleaved.data()),
                                     static_cast<qsizetype>(outFrames * 2 * sizeof(qint16)))));
}

void AnanBackend::scheduleTuneApply()
{
    if (!m_tuneThrottleTimer->isActive()) {
        //DH1KLM Leading edge: nothing in flight, apply immediately so the first
        //DH1KLM move of a gesture has no added latency.
        applyTuneToRadioAndPan();
        m_tuneThrottleTimer->start(kTuneThrottleMs);
    } else {
        //DH1KLM Already inside a cooldown window from a very recent apply --
        //DH1KLM remember that a trailing catch-up is owed once it elapses, rather
        //DH1KLM than doing the expensive work again right now. m_sliceFreqHz
        //DH1KLM already holds the latest value (set by the caller before this
        //DH1KLM runs), so the timeout handler always applies the newest position,
        //DH1KLM not a stale intermediate one.
        m_tunePendingApply = true;
    }
}

void AnanBackend::applyTuneToRadioAndPan()
{
    //DH1KLM The two genuinely expensive things a retune does: an actual UDP
    //DH1KLM High Priority packet to the radio (P2Client::setDdc0FrequencyHz --
    //DH1KLM real hardware DDC0/NCO retuning, which is not glitch-free at the rate
    //DH1KLM a raw mouse-move stream would otherwise drive it), and the pan's
    //DH1KLM center re-broadcast (PanadapterModel::setCenterBandwidth(), which
    //DH1KLM re-lays-out the spectrum/waterfall geometry on every call). Both are
    //DH1KLM throttled together by scheduleTuneApply() -- see its comment.
    if (m_client && m_connected) {
        const double hz = m_sliceFreqHz;
        QMetaObject::invokeMethod(m_client, [this, hz]() { m_client->setDdc0FrequencyHz(hz); },
                                  Qt::QueuedConnection);
    }
    emitPanState();
    //DH1KLM Tuning capture, once per settled gesture. Outside the connected guard:
    //DH1KLM where the dial ended up is worth keeping even during a teardown.
    emit operatingStateChanged();
}

}  // namespace AetherSDR::anan_original
