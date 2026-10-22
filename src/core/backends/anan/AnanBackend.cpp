#include "core/backends/anan/AnanFilterOptions.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/AppActivity.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan/AnanDroopDefaults.h"
#include "core/backends/WdspNoiseBlanker.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "models/BandSettings.h"
#include "models/BandPlanManager.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <QMetaType>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

Q_LOGGING_CATEGORY(lcAnanDefaults, "aether.anan.droopdefaults", QtWarningMsg)

namespace AetherSDR::anan {

const QString AnanBackend::kPanId = QStringLiteral("anan-0");

namespace {

// Placeholder until a bench measurement exists -- see the class comment.
// Named and zero rather than silently absent, so the next reader finds a
// TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

// One FFT AVG slider step as analyzer averaging time. deskHPSDR sets its
// analyzer's averaging as a time in 10 ms steps (default 250 ms), so 0 = none,
// 25 = deskHPSDR's default, 100 = one second. Published in capabilities()
// as BackendPanAveraging::msPerAverageStep.
constexpr int kMsPerAverageStep = 10;

// S-meter: WDSP's RXA_S_AV is 10*log10 mean I^2+Q^2 at the P2 24-bit full scale
// (kFullScale24Bit), i.e. dBFS. deskHPSDR uses the same scaling (1/2^23) and a
// 0 dB ANAN offset, so dBFS reads as dBm; this follows it. onDspMeter() adds the
// ADC attenuation before smoothing. No per-radio rx_gain_calibration yet. Kept
// apart from kUncalibratedDbfsToDbmOffset, which labels the panadapter axis.
constexpr double kSMeterDbmOffset = 0.0;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
           static_cast<qsizetype>(v.size() * sizeof(float))};
}

// The slice model's name for a WDSP AGC mode, the inverse of setSliceAgc()'s
// parse. WDSP's "long" (1) is never set here.
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
    // "CW" is the spelling TciProtocol::tciToSmartSDR produces and the one a
    // Flex reports -- HERMES.md §16.7's own regression was this falling
    // through to the USB fallback because only "CWU" was mapped.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("CWL")) return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    // WDSP has no RTTY demod: RTTY is FSK decoded externally from a wide passband,
    // so map it to DIGU rather than falling through to USB (HERMES.md 15.7/16.7).
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;   // unknown mode: same fallback as Hl2Backend's
}

bool AnanBackend::isKnownModeString(const QString& mode) noexcept
{
    // Every branch of modeFromString() above, aliases included.
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
    // Strictly inside (0, Nyquist): zero is the sentinel, and at or above
    // kDspClockHz/2 phaseWord() aliases rather than tuning.
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
    // Same window as DIGU -- see modeFromString()'s own comment on why RTTY
    // maps to Digu -- made explicit rather than landing here only because it
    // happens to equal this function's own unknown-mode fallback below.
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
        [this](int rateKsps) { setPanBandwidth(primaryPanId(), rateKsps * 1000.0); },
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
                ? QStringLiteral("Applied — the measured correction is "
                                                  "now live and saved.")
                : (m_droopCalibrator.hasResult()
                    ? QStringLiteral("Sweep stopped — review the "
                                                         "result, then Apply or Discard.")
                    : QStringLiteral("Sweep stopped — no result to apply."));
        }
        publishDroopStatus();
    });
    m_receiveLayout.reset(1, kFirstConnectFrequencyHz, 48);
    m_client = new AnanClient(nullptr);   // nullptr parent: moveToThread requires it
    m_dsp = new AnanRxDsp(nullptr);

    // Leading+trailing throttle for setSliceFrequency()'s expensive side
    // effects -- see scheduleTuneApply()'s comment. Lives on this object
    // (the GUI thread), same as the backend itself; not moved to m_ioThread.
    m_tuneThrottleTimer = new QTimer(this);
    m_tuneThrottleTimer->setSingleShot(true);
    connect(m_tuneThrottleTimer, &QTimer::timeout, this, [this] {
        if (m_tunePendingApply) {
            m_tunePendingApply = false;
            applyTuneToRadioAndPan();
            m_tuneThrottleTimer->start(kTuneThrottleMs);   // cooldown for the trailing apply too
        }
        // else: nothing arrived during the cooldown -- let the timer sit
        // idle until the next scheduleTuneApply() restarts it.
    });

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("anan-io"));
    m_client->moveToThread(m_ioThread);
    m_dsp->moveToThread(m_ioThread);
    AetherSDR::startStreamThread(m_ioThread);   // High QoS; see AppActivity.h

    // Build-only thread for a rate change's background DSP rebuild -- see
    // the member declaration comment. m_dspBuildContext owns no state; it
    // exists purely so QMetaObject::invokeMethod has a thread-affinity
    // target to hop the (slow) AnanRxDsp::buildChannel() call onto.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    // Same High QoS as the io thread: this one holds the FFTW planner mutex the
    // io thread takes in WdspChannel::setShift, so lower would be a priority
    // inversion. Idle except during a rebuild.
    AetherSDR::startStreamThread(m_dspBuildThread);

    // Both live on the I/O thread -- a same-thread call either way, but
    // explicit to document intent and match Hl2Backend's own explicit
    // DirectConnection on the equivalent wire-to-DSP wiring: no queue, no
    // event-loop hop for the sample path.
    connect(m_client, &AnanClient::ddcIqReady, m_dsp,
        [dsp = m_dsp, route = m_primaryRoute](int index,
                                              const std::vector<std::complex<float>>& iq) {
            if (index == route->load())
                dsp->processIqBlock(iq);
        },
            Qt::DirectConnection);

    // Everything below crosses from the I/O thread to whichever thread this
    // object lives on -- plain AutoConnection resolves to Queued, which is
    // why AnanRxDsp's constructor registers std::vector<float> as a Qt
    // metatype (P2Client's ddc0IqReady never needs that: it is only ever
    // DirectConnection'd, above).
    connect(m_client, &AnanClient::linkUp, this, [this] {
        m_connected = true;
        // See m_rateChanging's declaration comment: a live rate change is a
        // real stop+restart of this session, but the operator only asked to
        // zoom, so connected() is suppressed for that one round trip.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        if (!wasRateChange) {
            m_receiveLayout.reset(receiveStreamCeiling(), m_ddcCenterHz,
                                  m_pendingParams.ddc0RateKsps, receiveSliceCeiling());
            emit connected();
            emit capabilitiesChanged();
            defineMeters();
            // A new session's needle starts from its first reading, not from
            // where the last session's left off.
            m_sMeter.reset();
            // Likewise the rail's window: counts from before this session
            // must not ride into its first published voltage.
            resetSupplyRailWindow();
        }
        if (!wasRateChange) {
            if (m_restoreBandViewOnConnect) { restoreLastBandView(); }
            m_bandViewRestoreReady = true;
        }
        applyTuneToRadioAndPan();
        emitSliceState();
        // A rate change suppresses connected(), but a page opened during
        // its brief link restart still needs the fresh calibration status.
        publishDroopStatus();
        // Real zoom limits (HERMES.md §15.1): otherwise the GUI falls back to a
        // FlexLib model table (5.4 MHz for an unknown "ANAN-G2") and zooms past
        // the 1.536 MHz ceiling into unsampled black bars. Constant per connection
        // (one DDC), so emit on every linkUp; bounds are
        // capabilities().sampleRatesHz's endpoints.
        emit panBandwidthLimitsChanged(primaryPanId(),
                                      kDdc0RatesKsps.front() * 1000.0 / 1.0e6,
                                      nearestSessionRateKsps(kDdc0RatesKsps.back()) * 1000.0 / 1.0e6);
        publishReceiveGain();
        if (wasRateChange) {
            // Audio was muted in beginRateChange() before this session started (see
            // there). Unmute after a settle window: the rebuilt WdspChannel's
            // AGC/filters/DC blocker start from zero against live RF. The display is
            // unaffected.
            const quint64 generation = m_connectGeneration;
            QTimer::singleShot(kRateChangeAudioSettleMs, this, [this, generation] {
                // Superseded by a newer connect/rate-change/disconnect --
                // that cycle's own beginRateChange()/disconnectRadio() owns
                // muting from here, not this timer.
                if (generation != m_connectGeneration || !m_dsp)
                    return;
                QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                          Q_ARG(bool, false));
            });
            retryPendingRateChange();
        }
    });
    connect(m_client, &AnanClient::linkDown, this, [this] {
        m_connected = false;
        if (!m_rateChanging) {
            clearAdditionalRx();
            m_pendingBandView.reset();
            m_droopCalibrator.stop(false);
            m_droopCalibrator.setLandedRate(0);
            emit disconnected();
        }
    });
    connect(m_client, &AnanClient::connectionError, this,
            [this](const QString& reason) { emit connectionError(reason); });
    // Drops reach the readout through the counters snapshot below, which
    // carries the same total on a fixed cadence. Not wired per-drop: the
    // LinkStats tick is deliberately periodic rather than event-driven, so a
    // link that has gone silent still reports.
    connect(m_client, &AnanClient::linkCountersUpdated, this,
            &AnanBackend::onLinkCounters);
    // Queued onto this thread, like the snapshot above: the meter seam's
    // signals are emitted from the backend's own thread.
    connect(m_client, &AnanClient::supplyRailSampled, this,
            &AnanBackend::onSupplyRailCounts);
    // Only DDC0 feeds this DSP. Like ddc0IqReady above, notification and
    // processing run directly on the I/O thread, with the DSP as context.
    // Invalidate its partial FFT before the discontinuous block arrives.
    connect(m_client, &AnanClient::ddcSequenceGap, m_dsp, [dsp = m_dsp, route = m_primaryRoute](int ddcIndex) {
        if (ddcIndex == route->load()) {
            dsp->onSequenceGap();
        }
    }, Qt::DirectConnection);
    connect(m_client, &AnanClient::discoveryInfoReceived, this,
            [this](quint8 boardId, quint8 firmwareVer, quint8 reportedReceivers) {
                // Session resource ceilings are computed from this validated identity,
                // the effective profile and the advertised P2 receiver count.
                m_discoveredBoardId = boardId;
        m_discoveredFirmwareVer = firmwareVer;
        m_discoveredReceiverCount = reportedReceivers;
        m_discoveryInfoReceived = true;
        loadHardwareFilters(boardId);
        if (!hasClassicAlexAttenuator(m_pendingParams.model, boardId)
            && m_pendingParams.alexAttenuationDb != 0) {
            m_pendingParams.adc0AttenuationDb = std::min(31,
                m_pendingParams.adc0AttenuationDb + m_pendingParams.alexAttenuationDb);
            m_pendingParams.alexAttenuationDb = 0;
            QMetaObject::invokeMethod(m_client, "setFrontEndAttenuation", Qt::QueuedConnection,
                Q_ARG(int, 0), Q_ARG(int, m_pendingParams.adc0AttenuationDb), Q_ARG(int, 0));
        }
        m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
            ? m_pendingParams.adc1AttenuationDb
            : m_pendingParams.adc0AttenuationDb + m_pendingParams.alexAttenuationDb;
                // DH1KLM: Also cover address-only connections after the board is known.
                const auto profile = receiveProfile(boardId, m_pendingParams.model, m_pendingParams.overrideDetection);
        seedDroopCorrectionTables(m_pendingParams.protocol == 2
                                 && profile && profile->saturnDroopDefaults);
                // DH1KLM: Shipped droop tables follow the effective Saturn profile,
                //  including an explicit override. Warn about other gateware versions.
                if (profile && profile->saturnDroopDefaults && firmwareVer != kDefaultsGatewareVersion) {
            qCWarning(lcAnanDefaults).nospace()
                << "ANAN: radio reports gateware " << firmwareVer
                << ", shipped droop defaults were derived against "
                << kDefaultsGatewareVersion
                << " -- applying them anyway; run the in-app droop sweep if "
                   "the panadapter edges look wrong";
        }
        emit capabilitiesChanged();
    });

    connect(m_dsp, &AnanRxDsp::pcmReady, m_client,
            [this](const PcmFrame& pcm) { submitReceiveAudio(0, pcm); },
            Qt::DirectConnection);
    // Ungated by design: m_dsp emits on m_ioThread, the thread that emits
    // linkDown, so every frame is delivered before disconnected() (contract rule
    // 6).
    connect(m_dsp, &AnanRxDsp::spectrumReady, this, [this](const std::vector<float>& binsDbfs) {
        if (!m_connected || !primaryVisible())
            return;
        std::vector<float> dbm(binsDbfs.size());
        // Attenuation added back, as deskHPSDR does for its panadapter: a
        // signal keeps its level when the operator attenuates, and only the
        // part of the floor that was the ADC overloading actually drops.
        const float attenuation = static_cast<float>(m_attenuationDb);
        for (std::size_t i = 0; i < binsDbfs.size(); ++i) {
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset + attenuation;
        }
        m_droopCalibrator.onSpectrumFrame(dbm);
        const double rateHz = receiveTuningRateHz();
        if (m_bandViewSpanHz > 0 && m_bandViewSpanHz < rateHz) {
            dbm = bandViewSpectrum(dbm, m_bandViewSpanHz / rateHz);
        }
        emit spectrumFrameReady(primaryPanIndex(), floatBytes(dbm));
    });
    connect(m_dsp, &AnanRxDsp::meterUpdate, this, &AnanBackend::onDspMeter);
}

AnanBackend::~AnanBackend()
{
    m_droopCalibrator.stop(false);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup/rebuild callback

    // Join the build thread BEFORE deleting m_dsp/m_client: the build lambda's tail
    // posts invokeMethod(m_dsp/this, ...) after buildChannel() returns, so deleting
    // first is a use-after-free if the app closes mid-build. The wait is bounded by
    // the remaining FFTW planning.
    if(m_client&&m_ioThread&&m_ioThread->isRunning())
        QMetaObject::invokeMethod(m_client,"stop",Qt::BlockingQueuedConnection);
    clearAdditionalRx();
    // Drain the detach/move requests before the planning thread is joined.
    if(m_client&&m_ioThread&&m_ioThread->isRunning())
        QMetaObject::invokeMethod(m_client,[]{},Qt::BlockingQueuedConnection);
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
    // No setTune() implementation, so no tune generator to select a mode on.
    c.twoToneGenerator = std::nullopt;
    c.firmwareUpdateSource = std::nullopt;  // gateware number; no published list covers it.
    // No VOX, monitor or speech-processor setters. PROC would be the host
    // ClientComp, but canTransmit is false below, so nothing serves it and the
    // drop notice correctly stands.
    c.voxControl = std::nullopt;
    c.speechProcessorControl = std::nullopt;
    c.txMonitorControl = std::nullopt;
    // The radio's speaker output: setLineoutMute() sends it silence and
    // setLineoutGain() scales the samples this host feeds it.
    c.lineoutControl = RadioCapabilities::LineoutControl{};
    c.radioSetupControl = RadioSetupControl{{RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
    // No client owner is declared for "Show TX in Waterfall" on an ANAN.
    c.txWaterfallClientFlag = std::nullopt;
    // The panadapter dBm axis is dBFS with a dBm label: kUncalibratedDbfsToDbmOffset
    // is 0.0f and bin levels depend on window/normalisation, unverified against a
    // known input. Internally consistent, but not comparable: never publish as a
    // spot, compare with another station, or use as an absolute threshold. The
    // S-meter (kSMeterDbmOffset) is separate and follows deskHPSDR's scaling.
    PanAmplitudeModel amplitude;
    amplitude.calibratedDbm = false;
    // The bins are raw dBFS + 0, so they are ABSOLUTE: nothing in them depends on
    // the display reference level, and the auto-floor loop converges.
    amplitude.binsAbsolute = true;
    // No register holds a display range: the client is its only memory.
    amplitude.clientPersistsDbmRange = true;
    c.panAmplitude = amplitude;
    // No measured squelch map; the SQL line and Auto SQL keep Flex's scale.
    c.squelchLevelScale = legacyDbmSquelchScale();

    // Span follows the sample rate: it snaps by ratio to one of the six DDC0 rates
    // (no continuous zoom); panBandwidthLimitsChanged clamps to that list's ends.
    // radioWide is false: one receiver, so no span budget is shared (unlike the
    // HL2's one DDC in front of every receiver) (#5725).
    PanSpanModel span;
    span.followsSampleRate = true;
    span.radioWide = false;
    c.panSpanModel = span;
    c.hasAgcThreshold = true; // Host receiver DSP implements threshold/off gain.
    c.manufacturer = QStringLiteral("Apache Labs");
    //DH1KLM: Discovery identifies a family, not an individual Orion MkII SKU.
    const auto* selectedModel = ananModelInfo(m_pendingParams.model);
    c.model = selectedModel && m_pendingParams.model != AnanModel::Automatic
        ? QString::fromLatin1(selectedModel->name.data(), selectedModel->name.size())
        : m_discoveryInfoReceived && m_discoveredBoardId != 10
        ? QString::fromLatin1(DiscoveryReply{.boardId = m_discoveredBoardId}.boardDisplayName())
        : QStringLiteral("ANAN-G2");
    c.adcFrontEnd = RadioCapabilities::AdcFrontEnd{};
    // ---- PA telemetry ----
    // The DC supply rail, published as RAD:+13.8A from every status packet's
    // count (onSupplyRailCounts()). Claimed on a bench measurement, not on the
    // input's label: see kSupplyRailVoltsPerCount for the reading and for why
    // the input is not the one the spec calls "supply voltage".
    //DH1KLM: Do not generalize the measured 5 V slow-ADC scaling to
    // Hermes/Angelia's 3.3 V hardware before its telemetry is qualified.
    c.hasSupplyVoltageTelemetry = m_pendingParams.protocol == 2
        && (!m_discoveryInfoReceived || effectiveBoardId() == 5 || effectiveBoardId() == 10);
    //
    // There is NO PA temperature in this protocol. The radio's whole analog
    // payload is six fields and none of them is a temperature, so the readout
    // can never resolve and is withdrawn rather than left showing a placeholder.
    // hasPaTemperatureTelemetry stays false on its own (nothing is received);
    // this says the stronger thing -- that nothing CAN be.
    c.paTelemetryAudit = RadioCapabilities::PaTelemetryAudit{
        /*temperatureAbsent=*/true};
    c.canCreateSlices = m_connected;
    c.maxSlices = receiveSliceCeiling();
    c.maxPanadapters = receiveStreamCeiling();
    for (const int ksps : kDdc0RatesKsps) {
        if (m_pendingParams.protocol == 2 || ksps <= (m_pendingParams.redPitayaP1 ? 384 : 192))
            c.sampleRatesHz.append(ksps * 1000);
    }
    // Not reported -- no verified G2 tuning range (RFC: "I have not fetched
    // the Apache Labs G2 manual"). RadioCapabilities.h's own convention:
    // both zero means "not reported", not a guess.
    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    // State is engine-owned, but verified coverage is still unavailable.
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine, 0, 0};
    c.receiveModeControl = std::nullopt; // mode/passband transition contract not yet qualified
    c.receiveFilterControl = std::nullopt;
    // Gain and mute both act on this receiver's audio and are applied by this
    // backend, which is what the record promises. Engine authority: the state is
    // ours, the radio echoes nothing back, and there is no register to read.
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receiveAudioExport = std::nullopt; // native DAX export not qualified for this backend
    c.dspParameterReset = std::nullopt;
    c.receivePanCenterControl = std::nullopt; // center also retunes the slice
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
        48'000, m_pendingParams.protocol == 1
            ? (m_pendingParams.redPitayaP1 ? 384'000 : 192'000) : 1'536'000};
    c.canTransmit = false;         // P2Client has no PTT capability -- see class comment
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;        // client-side WDSP, like the HL2
    c.takesTxAudioOverSeam = true; // moot while canTransmit is false
    // Host-modulated like the HL2, so when this backend gains a drive path it
    // will own the register and report intent, not a readback (#5518). Declared
    // rather than left absent because the ownership answer is already known; it
    // populates no TransmitDelta::rfPower today, so nothing publishes drive yet.
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
    //DH1KLM The host owns FFT averaging and cadence; recall operator choices per radio and pan.
    c.backendPanAveraging = BackendPanAveraging{kMsPerAverageStep, true, {}, {}};
    c.panFrameRateShaping = PanFrameRateShaping{true};
    // No protocol field holds a waterfall rate: the client keeps it.
    c.panWaterfallRateShaping =
        PanWaterfallRateShaping{/*clientPersistsWaterfallRate*/ true};
    // No band/segment zoom: the protocol carries no per-pan zoom flag.
    c.panZoomModes = std::nullopt;
    c.persistsMemories = false;    // default; stated explicitly
    // The radio reports no frequency, mode or output level of its own, so the
    // client is their only memory, per radio.
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RfGain
                              | RadioCapabilities::ClientSettingsDomain::Tuning
                              | RadioCapabilities::ClientSettingsDomain::ReceiveOutputLevel;
    c.hostDroopCalibration = m_pendingParams.protocol == 2 && m_additionalRx.empty(); // P2 sweep rate ladder only.
    c.extensionNamespaces = {QStringLiteral("anan")};  // "droop.apply" -- see invokeExtension()
    // From this session's own Discovery reply (P2Client::discoveryInfoReceived()).
    // Absent until it lands ("not reported yet", like tuningMinHz/MaxHz);
    // capabilitiesChanged() fires when it does. reportedReceivers is informational:
    // Software slice and hardware stream ceilings are reported separately.
    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("reportedReceivers")] = m_discoveredReceiverCount;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        anan[QStringLiteral("protocol")] = m_pendingParams.protocol;
        c.extensions[QStringLiteral("anan")] = anan;
    }
    return c;
}

void AnanBackend::applyRestoredState(const RestoredRadioState& state)
{
    // A missing document resets both ADCs on a same-family radio swap.
    // RadioStateMemory owns persistence; validate its opaque extension here.
    m_bandViewMemory = state.extension.value(QStringLiteral("tuning")).toObject();
    m_bandViewMemorySupported = m_bandViewMemory.isEmpty()
        || (m_bandViewMemory.value(QStringLiteral("version")).toInt() == 1
            && m_bandViewMemory.value(QStringLiteral("radioSerial")).isString()
            && m_bandViewMemory.value(QStringLiteral("profiles")).isObject()
            && m_bandViewMemory.value(QStringLiteral("profiles")).toObject().size() <= 128);
    m_beforeBandView = {};
    m_currentViewBand.clear();
    m_bandViewRestoreReady = false;
    const QJsonObject gain = state.extension.value(QStringLiteral("rfGain")).toObject();
    const int savedAlex = gain.value(QStringLiteral("alexAttenuationDb")).toInt(0);
    m_pendingParams.alexAttenuationDb = savedAlex == 10 || savedAlex == 20 || savedAlex == 30 ? savedAlex : 0;
    m_pendingParams.adc0AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc0AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_pendingParams.adc1AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc1AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb : m_pendingParams.adc0AttenuationDb + m_pendingParams.alexAttenuationDb;

    // Unconditional, like the ADC values above, so a same-family backend reuse
    // cannot carry radio A's dial or level onto radio B. A rejected value leaves
    // the sentinel, and connectRadio() falls to the first-connect default.
    m_restoredFreqHz = isRestorableFrequencyHz(state.rfFrequencyHz)
        ? state.rfFrequencyHz : 0.0;
    m_restoredMode = isKnownModeString(state.mode) ? state.mode.toUpper() : QString();
    m_lineoutGainPercent = state.receiveOutputLevelPct >= 0
        ? state.receiveOutputLevelPct : kDefaultLineoutGainPercent;
}

RestoredRadioState AnanBackend::currentOperatingState() const
{
    RestoredRadioState state;
    // The live dial and level, not the restored ones. A level of 0 is reported:
    // a silenced speaker is a choice, and "nothing to remember" is -1.
    state.rfFrequencyHz = m_sliceFreqHz;
    state.mode = m_mode;
    state.receiveOutputLevelPct = m_lineoutGainPercent;
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
        {QStringLiteral("alexAttenuationDb"), m_pendingParams.alexAttenuationDb},
        {QStringLiteral("adc0AttenuationDb"), m_pendingParams.adc0AttenuationDb},
        {QStringLiteral("adc1AttenuationDb"), m_pendingParams.adc1AttenuationDb}}}};
    const auto views = capturedBandViewMemory();
    if (!views.isEmpty()) { state.extension.insert(QStringLiteral("tuning"), views); }
    return state;
}

QString AnanBackend::bandViewProfileKey() const
{
    return QStringLiteral("%1:%2:%3:%4:%5").arg(static_cast<int>(m_pendingParams.model))
        .arg(m_discoveredBoardId).arg(m_pendingParams.ddc0AdcIndex)
        .arg(m_pendingParams.overrideDetection ? 1 : 0).arg(m_pendingParams.protocol);
}

QJsonObject AnanBackend::bandViewSnapshot() const
{
    const auto tuning = receiveTuning(m_ctun || m_receiveLayout.occupants(primaryPanIndex()) > 1, m_ddcCenterHz, m_sliceFreqHz,
        receiveTuningRateHz(), m_filterLowHz, m_filterHighHz, cwBfoHz());
    return {{QStringLiteral("vfoHz"), m_sliceFreqHz},
        {QStringLiteral("centerHz"), tuning ? tuning->centerHz : m_ddcCenterHz},
        {QStringLiteral("spanHz"), m_bandViewSpanHz},
        {QStringLiteral("rateKsps"), static_cast<int>(receiveTuningRateHz() / 1000)},
        {QStringLiteral("ctun"), m_ctun}, {QStringLiteral("mode"), m_mode},
        {QStringLiteral("filterLow"), m_filterLowHz}, {QStringLiteral("filterHigh"), m_filterHighHz}};
}

bool AnanBackend::validBandViewSnapshot(const QJsonObject& view) const
{
    for (const auto* key : {"vfoHz", "centerHz", "spanHz", "rateKsps", "filterLow", "filterHigh"}) {
        if (!view.value(QLatin1String(key)).isDouble()
            || !std::isfinite(view.value(QLatin1String(key)).toDouble())) { return false; }
    }
    const double frequency = view.value(QStringLiteral("vfoHz")).toDouble();
    const double center = view.value(QStringLiteral("centerHz")).toDouble();
    const double span = view.value(QStringLiteral("spanHz")).toDouble();
    const double rate = view.value(QStringLiteral("rateKsps")).toDouble();
    const double low = view.value(QStringLiteral("filterLow")).toDouble();
    const double high = view.value(QStringLiteral("filterHigh")).toDouble();
    const QString mode = view.value(QStringLiteral("mode")).toString();
    if (!view.value(QStringLiteral("ctun")).isBool() || !isKnownModeString(mode)
        || !isRestorableFrequencyHz(frequency) || !isRestorableFrequencyHz(center)
        || rate < 48 || rate > nearestSessionRateKsps(kDdc0RatesKsps.back())
        || std::find(kDdc0RatesKsps.begin(), kDdc0RatesKsps.end(), rate) == kDdc0RatesKsps.end()
        || span < 0 || span > rate * 1000 || low < -100'000 || high > 100'000
        || low >= high || low != std::floor(low) || high != std::floor(high)) { return false; }
    const auto tuning = receiveTuning(view.value(QStringLiteral("ctun")).toBool(),
        center, frequency, rate * 1000, low, high, cwBfoOffsetHz(mode, m_cwPitchHz));
    return tuning && std::abs(tuning->centerHz - center) < 0.5;
}

QJsonObject AnanBackend::capturedBandViewMemory() const
{
    if (!m_connected || !m_discoveryInfoReceived || m_radioSerial.isEmpty()
        || m_rateChanging || m_pendingBandView || !m_bandViewMemorySupported || !m_bandViewRestoreReady
        || m_currentViewBand.isEmpty() || m_currentViewBand == QLatin1String("GEN")) { return m_bandViewMemory; }
    const auto view = bandViewSnapshot();
    if (!validBandViewSnapshot(view)) { return m_bandViewMemory; }
    const bool owned = m_bandViewMemory.value(QStringLiteral("radioSerial")).toString() == m_radioSerial;
    auto profiles = owned ? m_bandViewMemory.value(QStringLiteral("profiles")).toObject() : QJsonObject{};
    const QString key = bandViewProfileKey();
    if (!profiles.contains(key) && profiles.size() >= 128) { return m_bandViewMemory; }
    auto profile = profiles.value(key).toObject();
    auto bands = profile.value(QStringLiteral("bands")).toObject();
    if (bands.size() > kBandCount) { return m_bandViewMemory; }
    auto record = bands.value(m_currentViewBand).toObject();
    record.insert(QStringLiteral("view"), view);
    record.insert(QStringLiteral("beforeB"), m_beforeBandView);
    if (m_ctun) {
        record.insert(QStringLiteral("ctunView"), view);
        record.insert(QStringLiteral("ctunBeforeB"), m_beforeBandView);
    }
    bands.insert(m_currentViewBand, record);
    profile.insert(QStringLiteral("bands"), bands);
    profile.insert(QStringLiteral("lastBand"), m_currentViewBand);
    profiles.insert(key, profile);
    auto memory = owned ? m_bandViewMemory : QJsonObject{};
    memory.insert(QStringLiteral("radioSerial"), m_radioSerial);
    memory.insert(QStringLiteral("version"), 1);
    memory.insert(QStringLiteral("profiles"), profiles);
    return memory;
}

void AnanBackend::rememberBandView()
{
    m_bandViewMemory = capturedBandViewMemory();
}

void AnanBackend::restoreBandView(const QJsonObject& record, bool preserveVfo)
{
    const auto view = record.value(QStringLiteral("view")).toObject();
    if (!validBandViewSnapshot(view)) { return; }
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    if (!preserveVfo) {
        m_sliceFreqHz = view.value(QStringLiteral("vfoHz")).toDouble();
        m_mode = view.value(QStringLiteral("mode")).toString();
        m_filterLowHz = view.value(QStringLiteral("filterLow")).toInt();
        m_filterHighHz = view.value(QStringLiteral("filterHigh")).toInt();
    }
    m_currentViewBand = BandSettings::bandForFrequency(m_sliceFreqHz / 1e6);
    m_beforeBandView = record.value(QStringLiteral("beforeB")).toObject();
    if (!validBandViewSnapshot(m_beforeBandView)) { m_beforeBandView = {}; }
    m_ctun = view.value(QStringLiteral("ctun")).toBool();
    const double center = m_ctun ? view.value(QStringLiteral("centerHz")).toDouble() : m_sliceFreqHz;
    const double span = view.value(QStringLiteral("spanHz")).toDouble();
    const int rate = view.value(QStringLiteral("rateKsps")).toInt();
    setPanBandwidth(primaryPanId(), rate * 1000.0);
    pushModeFilterShift();
    if (m_rateChanging) {
        m_pendingBandView = ReceiveBandView{center, rate, false, span};
    } else {
        m_ddcCenterHz = center;
        m_bandViewSpanHz = span;
        applyTuneToRadioAndPan();
    }
    emitSliceState();
}

void AnanBackend::restoreLastBandView()
{
    if (!m_bandViewMemorySupported
        || m_bandViewMemory.value(QStringLiteral("radioSerial")).toString() != m_radioSerial) { return; }
    const auto profile = m_bandViewMemory.value(QStringLiteral("profiles")).toObject()
        .value(bandViewProfileKey()).toObject();
    const auto band = profile.value(QStringLiteral("lastBand")).toString();
    const auto record = profile.value(QStringLiteral("bands")).toObject().value(band).toObject();
    const auto view = record.value(QStringLiteral("view")).toObject();
    if (validBandViewSnapshot(view)
        && BandSettings::bandForFrequency(view.value(QStringLiteral("vfoHz")).toDouble() / 1e6) == band) {
        restoreBandView(record);
    } else if (!m_bandViewMemory.isEmpty()) {
        // A newly selected hardware mapping starts independently of another
        // profile's legacy radio-wide frequency and mode.
        m_sliceFreqHz = kFirstConnectFrequencyHz;
        m_ddcCenterHz = m_sliceFreqHz;
        m_mode = QString::fromLatin1(kFirstConnectMode);
        const auto [low, high] = defaultPassbandForMode(m_mode);
        m_filterLowHz = low; m_filterHighHz = high;
        m_ctun = false; m_bandViewSpanHz = 0; m_beforeBandView = {};
        m_currentViewBand = BandSettings::bandForFrequency(m_sliceFreqHz / 1e6);
        pushModeFilterShift();
        applyTuneToRadioAndPan();
        emitSliceState();
    }
}

void AnanBackend::seedDroopCorrectionTables(bool useSaturnDefaults)
{
    std::vector<AnanRxDsp*> targets;
    if (m_dsp)
        targets.push_back(m_dsp);
    for (const auto& [id, r] : m_additionalRx)
        if (r->ready && r->dsp)
            targets.push_back(r->dsp);
    if (m_dsp) {
        // Forget the PREVIOUS radio's tables before seeding this one's. The
        // seed below only inserts, and m_dsp is constructed once for the
        // lifetime of this backend -- so without this, connecting a second,
        // uncalibrated G2 in the same session renders it through the first
        // one's per-bin corrections. See
        // AnanRxDsp::clearDroopCorrectionTables().
        for (auto* target : targets)
            QMetaObject::invokeMethod(target, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);
        //DH1KLM: A P2 noise sweep is not a P1 decimator calibration. Clear
        // both defaults and saved tables when the same radio connects via P1.
        if (m_pendingParams.protocol == 1)
            return;

        auto pushTable = [targets](int rateKsps, const DroopCorrectionTable& t) {
            for (auto* target : targets)
                QMetaObject::invokeMethod(
                    target, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, rateKsps),
                Q_ARG(std::vector<float>, std::vector<float>(t.begin(), t.end())));
        };

        // Shipped defaults FIRST, so an uncalibrated radio is corrected on first
        // connect (one gateware-derived curve for all rates, AnanDroopDefaults.h). Both
        // lists come from kDdc0RatesKsps, so the null test is a structural guard only.
        if (useSaturnDefaults) {
            for (const int rateKsps : defaultDroopRatesKsps()) {
                if (const DroopCorrectionTable* t = defaultDroopTableForRate(rateKsps)) {
                    pushTable(rateKsps, *t);
                }
            }
        }

        // Then THIS radio's own bench calibration on top, per rate. An
        // operator who measured their own hardware always outranks a shipped
        // default, and a partial sweep only overrides the rates it actually
        // covered rather than wiping the rest back to the default.
        const auto tables = AnanDroopCalibrator::loadTables(
            RadioSettingsScope(QStringLiteral("anan"), m_radioSerial));
        for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
            pushTable(it.key(), it.value());
        }
    }
}

void AnanBackend::connectRadio(const RadioConnectRequest& request)
{
    clearAdditionalRx();
    m_rxAntenna = AnanSettings::rxAntenna();
    const QHostAddress hostAddr(request.host);
    if (hostAddr.isNull()) {
        emit connectionError(QStringLiteral("ANAN: invalid host '%1'").arg(request.host));
        return;
    }
    //DH1KLM: A discovered physical board ID is authoritative metadata. Until
    //a receive profile exists, reject an explicitly identified board. Orion
    //MkII and Saturn have separate physical DDC and ALEX settings.
    if (request.params.contains(QStringLiteral("anan.boardId"))) {
        bool boardIdOk = false;
        const int boardId = request.params.value(QStringLiteral("anan.boardId")).toInt(&boardIdOk);
        if (!boardIdOk || !receiveProfile(boardId)) {
            emit connectionError(QStringLiteral(
                "ANAN: board ID %1 has no verified connection profile; an explicit receive profile is required")
                .arg(boardIdOk ? QString::number(boardId) : QStringLiteral("invalid")));
            return;
        }
    }
    if (m_connected)
        disconnectRadio();

    // A fresh connect (possibly to a different host) must not keep reporting
    // a prior radio's discovered identity until its own reply lands -- see
    // capabilities()'s own comment.
    m_discoveryInfoReceived = false;
    m_discoveredBoardId = 0;
    m_discoveredFirmwareVer = 0;
    m_discoveredReceiverCount = 0;

    // Per-radio identity for RadioSettingsScope (droop calibration -- see
    // invokeExtension()'s "droop.apply" handler). Set BEFORE the seed load
    // just below, and before anything else in this function needs it,
    // matching Hl2Backend's own m_radioSerial assignment ordering.
    m_radioSerial = request.serial;
    if (!m_bandViewMemory.isEmpty() && m_bandViewMemorySupported
        && m_bandViewMemory.value(QStringLiteral("radioSerial")).toString() != m_radioSerial) {
        m_bandViewMemory = {};
        m_bandViewMemorySupported = true;
        m_restoredFreqHz = 0;
        m_restoredMode.clear();
    }
    m_restoreBandViewOnConnect = !request.params.contains(QStringLiteral("anan.rxFrequencyHz"));
    m_bandViewRestoreReady = false;
    m_beforeBandView = {};
    m_currentViewBand.clear();
    //DH1KLM: Select the wire protocol from discovery, never from the model name.
    m_pendingParams.protocol = request.params.value(QStringLiteral("anan.protocol"), 2).toInt();
    m_pendingParams.model = static_cast<AnanModel>(
        request.params.value(QStringLiteral("anan.model"), -1).toInt());
    m_pendingParams.overrideDetection =
        request.params.value(QStringLiteral("anan.overrideDetection"), false).toBool();
    if (!ananModelInfo(m_pendingParams.model)
        || (m_pendingParams.overrideDetection && m_pendingParams.model == AnanModel::Automatic)) {
        emit connectionError(QStringLiteral("Invalid or unsupported saved OpenHPSDR model profile."));
        return;
    }
    m_pendingParams.redPitayaP1 = m_pendingParams.protocol == 1
        && (m_pendingParams.model == AnanModel::RedPitaya
            || request.params.value(QStringLiteral("anan.redPitayaP1"), false).toBool());
    if (m_pendingParams.protocol != 1 && m_pendingParams.protocol != 2) {
        emit connectionError(QStringLiteral("Unsupported OpenHPSDR protocol selection."));
        return;
    }
    //DH1KLM: Saturn-derived defaults follow the selected effective profile.
    const auto profile = receiveProfile(
        request.params.value(QStringLiteral("anan.boardId"), -1).toInt(), m_pendingParams.model,
        m_pendingParams.overrideDetection);
    if (request.params.contains(QStringLiteral("anan.boardId")) && !profile) {
        emit connectionError(QStringLiteral("The selected model does not match this radio's physical board."));
        return;
    }
    seedDroopCorrectionTables(m_pendingParams.protocol == 2
                             && profile && profile->saturnDroopDefaults);

    //DH1KLM: ADC count comes from the selected effective board profile.
    // Unknown address-only targets resolve it in P2Client after Discovery.
    m_pendingParams.numAdcs = profile ? profile->numAdcs : 2;
    m_pendingParams.host = request.host;
    //DH1KLM: Keep discovery's numeric board identity through to the P2
    // client so it can reject a target that differs from the selected radio.
    m_pendingParams.expectedBoardId =
        request.params.value(QStringLiteral("anan.boardId"), -1).toInt();
    m_pendingParams.ddc0RateKsps =
        nearestSessionRateKsps(request.params.value(QStringLiteral("anan.ddc0RateKsps"), 48).toInt());
    // Connect-time-only ADC options -- see P2Client::Params' own comment
    // for why none of these have a live setter. Defaults match
    // AnanSettings' own defaults, so a caller that never populated these
    // params (a picker/auto-reconnect connect, same reasoning as
    // ddc0RateKsps above) still gets sane values, not zeroed-out ones.
    m_pendingParams.ditherEnabled =
        request.params.value(QStringLiteral("anan.ditherEnabled"), true).toBool();
    m_pendingParams.randomEnabled =
        request.params.value(QStringLiteral("anan.randomEnabled"), true).toBool();
    m_pendingParams.ddc0AdcIndex =
        request.params.value(QStringLiteral("anan.ddc0AdcIndex"), 0).toInt() == 1 ? 1 : 0;
    //DH1KLM: Thetis Saturn BPF1 is frequency-selected by default; explicit
    //configuration can still request bypass.
    m_pendingParams.bypassAdc0Filters =
        request.params.value(QStringLiteral("anan.bypassAdc0Filters"), false).toBool();
    m_pendingParams.bypassAdc1Filters =
        request.params.value(QStringLiteral("anan.bypassAdc1Filters"), true).toBool();
    m_pendingParams.filters.reset();
    if (profile) {
        loadHardwareFilters(m_pendingParams.expectedBoardId);
    }
    m_pendingParams.rxAntenna = m_rxAntenna;
    // FALSE by default, unlike the options above, and for the opposite reason:
    // their default is the hardware's shipped state, while this one's is "the
    // operator has not asked for an outbound stream". A connect that never
    // populated this param must not start one.
    m_pendingParams.speakerAudioEnabled =
        request.params.value(QStringLiteral("anan.speakerAudioEnabled"), false).toBool();
    // A logical profile selects the analogue ATT bank even under a board override.
    if ((m_pendingParams.model != AnanModel::Automatic || m_pendingParams.expectedBoardId >= 0)
        && !hasClassicAlexAttenuator(m_pendingParams.model, m_pendingParams.expectedBoardId)) {
        const int total = m_pendingParams.adc0AttenuationDb + m_pendingParams.alexAttenuationDb;
        m_pendingParams.adc0AttenuationDb = std::min(total, 31);
        m_pendingParams.alexAttenuationDb = 0;
    }
    // applyRestoredState() seeds both ADC values before this connect.
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb
        : m_pendingParams.adc0AttenuationDb + m_pendingParams.alexAttenuationDb;

    // An explicit connect frequency, then this radio's restored one, then the
    // first-connect default.
    bool frequencyOk = true;
    const double frequencyHz = request.params.contains(QStringLiteral("anan.rxFrequencyHz"))
        ? request.params.value(QStringLiteral("anan.rxFrequencyHz")).toDouble(&frequencyOk)
        : (m_restoredFreqHz > 0.0 ? m_restoredFreqHz : kFirstConnectFrequencyHz);
    if (!frequencyOk || !std::isfinite(frequencyHz) || frequencyHz < 0 || frequencyHz > 61'440'000) {
        emit connectionError(QStringLiteral("The receive frequency must be within 0–61.44 MHz."));
        return;
    }
    m_sliceFreqHz = frequencyHz;
    m_currentViewBand = BandSettings::bandForFrequency(frequencyHz / 1e6);
    m_ddcCenterHz = frequencyHz;
    m_lastSentCenterHz = -1.0;
    m_ctun = false;
    m_bandViewSpanHz = 0.0;
    m_pendingBandView.reset();
    // Before m_pendingDspConfig is built below, which reads m_mode. No restored
    // mode means the default, never the previous radio's m_mode. The passband
    // resets only on a mode change, as in setSliceMode().
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
    // The panel's width in points (setPanPixelWidth()), or one point per
    // droop-table entry until the GUI has reported one; the analyzer's FFT
    // behind them is larger (see AnanPanAnalyzer). spectrumFps keeps Config's
    // default until RadioModel pushes the operator's rate through
    // setPanFrameRate().
    m_pendingDspConfig.panPoints = m_panPoints;
    // This backend retains the AGC and NB requests across reconnects.
    // emitSliceState() also supplies them if a different radio requires a
    // fresh slice.
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
    // Build on m_dspBuildThread, not m_ioThread: buildChannel() can take ~19 s cold
    // (FFTW PATIENT), and a disconnect/close in that window must not block on a
    // BlockingQueuedConnection to m_client queued behind configure().
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    // Seed the requested startup state before the off-thread build. The
    // install path reapplies the object's current state so operator edits made
    // during a build win; without this first-connect seed it would reapply the
    // Config member defaults over cfg instead.
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
    // A newer connectRadio() or a disconnectRadio() happened while
    // configure() was running on the I/O thread -- this result is stale.
    if (generation != m_connectGeneration)
        return;

    if (!ok) {
        emit connectionError(QStringLiteral("ANAN: DSP configure failed: %1")
                             .arg(error.isEmpty() ? QStringLiteral("unknown error") : error));
        // Only ever reached from connectRadio()'s first-connect path now --
        // beginRateChange() builds the new channel off-thread and never
        // calls beginDspSetup()/finishDspSetup() -- so m_rateChanging is
        // always false here. Kept for symmetry with
        // startP2ClientSession()'s own failure handling rather than because
        // this branch can currently observe a rate change in flight.
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
    // Rebuilt here rather than at connect, because this is the one place BOTH a
    // first connect and a rate change pass through. A converter carries filter
    // state, and the audio either side of a DSP rebuild is not one continuous
    // stream, so carrying that state across would ring the tail of the old rate
    // into the head of the new one.
    resetSpeakerResamplers();
    AnanClient::Params params = m_pendingParams;
    const auto antenna = receiveAntennaRoute();
    params.rxAntenna = antenna.antenna;
    if (params.filters) { params.filters->rxOnlyInput = antenna.rxOnly; }
    m_appliedAntenna = antenna.antenna;
    m_appliedRxOnly = antenna.rxOnly;
    const bool isRateChange = m_rateChanging;
    const double centerHz = m_ddcCenterHz;
    QMetaObject::invokeMethod(m_client, [this, generation, params, isRateChange, centerHz]() {
        const bool started = isRateChange
            ? m_client->start(params, kRateChangeConnectTimeoutMs)
            : m_client->start(params);
        if (!started) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (generation != m_connectGeneration)
                    return;
                emit connectionError(QStringLiteral("ANAN: could not open the UDP socket"));
                if (m_pendingBandwidthKsps != 0) {
                    // Same retry-window fix as finishRateChange()'s failure
                    // branch (see its comment) -- a socket-open failure
                    // right after a background rebuild can hit the exact
                    // same hazard: clearing m_rateChanging here, before the
                    // queued zoom retries, would let this failed attempt's
                    // own teardown emit an unsuppressed connected()/
                    // disconnected() pair before the retry re-arms it.
                    retryPendingRateChange();
                } else {
                    m_rateChanging = false;
                    emitPanState();
                }
            }, Qt::QueuedConnection);
            return;
        }
        // Push the operator's current tune once the session is up.
        // linkUp() (P2Client's own signal, connected in the constructor)
        // is what actually fires connected()/emitSliceState()/emitPanState()
        // once the first genuine DDC0 frame arrives -- not this call.
        m_client->setDdc0FrequencyHz(centerHz);
    }, Qt::QueuedConnection);
}

void AnanBackend::disconnectRadio()
{
    clearAdditionalRx();
    rememberBandView();
    m_bandViewSpanHz = 0.0;
    m_pendingBandView.reset();
    retirePcmStreams();
    m_droopCalibrator.stop(false);
    m_droopCalibrator.setLandedRate(0);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup callback
    // Blocking, like ~AnanBackend() and closeEvent()'s other worker stops: the
    // caller may tear down GUI objects or this backend next (the window X button
    // runs this synchronously with the event loop pumping), so the I/O thread
    // must have closed the socket first. disconnected() still arrives via
    // linkDown().
    if (m_client)
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    m_pendingBandwidthKsps = 0;
    // The next session's counters restart from zero in P2Client::start(), so a
    // cached snapshot from this one would make the first tick of the next read
    // as a huge backward jump in every total.
    m_linkCounters = {};
    m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = 0;
    // A status packet can arrive before the next session's linkUp, so the
    // rail's window is emptied here as well as there.
    resetSupplyRailWindow();
    // A genuine operator-initiated disconnect always fires disconnected(),
    // even one that lands mid-rate-change -- this is not the zoom case
    // m_rateChanging exists to hide.
    m_rateChanging = false;
    // m_audioMuted lives on AnanRxDsp and survives configure() (see that
    // function's own comment), so a disconnect landing mid-rate-change --
    // while audio was deliberately muted for the settle window -- would
    // otherwise leave the NEXT session starting muted with nothing left to
    // ever clear it.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, false));
    // The droop tables are per-RADIO and m_dsp outlives any one connection,
    // so they go with the radio they were measured on. Clearing here (as well
    // as before connectRadio()'s seed) means a disconnected session cannot
    // leave a stale correction armed for whatever connects next, by any path.
    // The bypass flag is cleared too: a disconnect mid-sweep stops the
    // calibrator (the backend's disconnect handler) but its
    // finishSweep() cannot reach a backend that is already gone.
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionBypassed",
                                  Qt::QueuedConnection, Q_ARG(bool, false));
    }
    // linkDown() (constructor-wired) sets m_connected = false and emits
    // disconnected() once P2Client::stop() actually runs.
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
    pushReceiveShift(m_sliceFreqHz - m_ddcCenterHz - bfo);
}

void AnanBackend::setSliceFrequency(int sliceId, double hz)
{
    if (m_receiveBuilding) {
        emitSliceState();
        return;
    }
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end() || !i->second->ready)
            return;
        const auto before = m_receiveLayout.streams();
        if (!m_receiveLayout.tune(sliceId, hz, false)) {
            publishAdditionalRx(sliceId);
            return;
        }
        retireEmptyReceiveWindows(before);
        updateReceiveStreams();
        pushAdditionalShifts();
        publishAdditionalRx(sliceId);
        publishReceiveWindows();
        return;
    }
    if (m_connected) {
        if (!primaryVisible())
            return;
        const auto before = m_receiveLayout.streams();
        if (!m_receiveLayout.tune(0, hz, m_ctun)) {
            emitSliceState();
            return;
        }
        m_ddcCenterHz = m_receiveLayout.streams()[primaryPanIndex()].centerHz;
        retireEmptyReceiveWindows(before);
    }
    if (!std::isfinite(hz) || hz < 0 || hz > 61'440'000) {
        emitSliceState();
        return;
    }
    const QString targetBand = BandSettings::bandForFrequency(hz / 1.0e6);
    if (targetBand != m_currentViewBand) {
        rememberBandView();
        m_currentViewBand = targetBand;
        m_beforeBandView = {};
        m_ctun = false;
        m_bandViewSpanHz = 0;
    }
    m_pendingBandView.reset(); // newer operator tuning supersedes a pending band view
    if (m_bandViewSpanHz > 0 && std::abs(hz - m_ddcCenterHz) > m_bandViewSpanHz * 0.46) {
        m_bandViewSpanHz = 0.0;
    }
    m_sliceFreqHz = hz;
    // Digit readout / slice model: unthrottled. Cheap (no radio round trip,
    // no pan re-layout), and it is what makes a drag gesture feel like it is
    // tracking the mouse at all.
    emitSliceState();
    // Coalesce hardware/geometry updates and the digital tuning shift together.
    scheduleTuneApply();
    // The Tuning capture rides the same throttle: applyTuneToRadioAndPan().
}

void AnanBackend::setSliceMode(int sliceId, const QString& mode)
{
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end() || !isKnownModeString(mode))
            return;
        auto& r = *i->second;
        if (r.mode.compare(mode, Qt::CaseInsensitive) != 0) {
            const auto f = defaultPassbandForMode(mode);
            r.low = f.first;
            r.high = f.second;
        }
        r.mode = mode;
        applyAdditionalRx(sliceId);
        return;
    }
    Q_UNUSED(sliceId);
    // Idempotence: only reset the passband to the mode's default when the
    // mode actually changes, so a repeated set does not clobber an
    // operator's manual filter edit.
    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    m_mode = mode;
    if (m_ctun) { scheduleTuneApply(); }
    else if (m_receiveLayout.occupants(primaryPanIndex()) <= 1) { m_ddcCenterHz = m_sliceFreqHz; }
    pushModeFilterShift();
    emitSliceState();
    emit operatingStateChanged();   // Tuning capture; a mode change is discrete
}

void AnanBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end() || lowHz >= highHz || lowHz < -40000 || highHz > 40000)
            return;
        i->second->low = lowHz;
        i->second->high = highHz;
        applyAdditionalRx(sliceId);
        return;
    }
    Q_UNUSED(sliceId);
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_ctun) { scheduleTuneApply(); }
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
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        const auto m = mode.toLower();
        i->second->agc = m == "off" ? 0 : m == "slow" ? 2 : m == "fast" ? 4 : 3;
        i->second->threshold = std::clamp(thresholdDb, 0, 100);
        applyAdditionalRx(sliceId);
        return;
    }
    Q_UNUSED(sliceId);
    // thresholdDb is the AGC-T knob's 0..100, not dB: agcKnobDb() maps it.
    const QString m = mode.trimmed().toLower();
    int wdspMode = 3;   // medium, WDSP's own default
    if (m == QLatin1String("off"))  wdspMode = 0;
    else if (m == QLatin1String("slow")) wdspMode = 2;
    else if (m == QLatin1String("fast")) wdspMode = 4;
    // Live state for connectRadio() and beginRateChange() -- see the member
    // declaration comment.
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
    if (m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0 && request.field == SliceAgcRequest::Field::OffLevel) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        i->second->off = std::clamp(request.offLevel, 0, 100);
        applyAdditionalRx(sliceId);
        return;
    }
    if (request.field != SliceAgcRequest::Field::OffLevel) {
        IRadioBackend::requestSliceAgc(sliceId, request);
        return;
    }
    // Sent in every AGC mode: WDSP applies the fixed gain only with AGC off
    // (wcpAGC.c xwcpagc), so a level set first takes effect when AGC goes off.
    m_agcOffLevel = std::clamp(request.offLevel, 0, 100);
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setAgcFixedGain", Qt::QueuedConnection,
            Q_ARG(double, agcKnobDb(m_agcOffLevel)));
    }
    emitSliceState();
}

ReceiveDispatch AnanBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    // #5824 already owns the worker and NB readback. The typed desktop route
    // must consume that implementation, not replace it or invent other DSP.
    if (!m_receiveLayout.slice(sliceId) || !request.valid()
        || request.feature != SliceDspRequest::Feature::Nb) {
        return ReceiveDispatch::Unsupported;
    }
    // Level and Fill ride along so the one verb always gets the whole state.
    setSliceNoiseBlanker(sliceId, request.requestedBlanker(), request.level, request.fill);
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                                       int level, AetherSDR::NoiseBlankerFill fill)
{
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        i->second->nb = kind;
        i->second->nbLevel = std::clamp(level, 0, 100);
        i->second->nbFill = fill;
        applyAdditionalRx(sliceId);
        return;
    }
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
    if (!m_receiveLayout.slice(sliceId) || !request.valid()
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
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        i->second->mute = mute;
        publishAdditionalRx(sliceId);
        return;
    }
    Q_UNUSED(sliceId);   // one slice in this phase
    if (m_sliceAudioMuted == mute) {
        return;
    }
    m_sliceAudioMuted = mute;
    // Published, not just stored. The gate that decides whether the control is
    // offered at all requires an OBSERVATION as well as the capability, so a
    // mute that is applied but never echoed leaves the control refused with
    // "capability.unavailable" -- working audio behind a dead control.
    emitSliceState();
}

void AnanBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        i->second->gain = std::clamp(gainPercent, 0, 100);
        publishAdditionalRx(sliceId);
        return;
    }
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
    if(m_connected&&!m_receiveLayout.slice(sliceId))return;
    if (sliceId != 0) {
        auto i = m_additionalRx.find(sliceId);
        if (i == m_additionalRx.end())
            return;
        i->second->pan = std::clamp(panPercent, 0, 100);
        syncReceiveAudioRoutes();
        return;
    }
    Q_UNUSED(sliceId);
    // NOT published: SliceDelta carries no balance field, so there is no
    // observation to echo and nothing downstream reads one. Applied all the
    // same -- the audio stage is the same stage either way.
    m_sliceAudioPanPercent = std::clamp(panPercent, 0, 100);
    syncReceiveAudioRoutes();
}

void AnanBackend::setLineoutGain(int percent)
{
    // The percent runs through the SAME dB law as the per-slice fader
    // (sliceAudioAmplitude, at the send), so 50 here is -20 dB and not half
    // amplitude. Worth knowing when comparing families: a Flex's own lineout
    // route sends `mixer lineout gain 50` and whatever that means at the radio is
    // its own scale, so the same slider position is not promised to be the same
    // loudness across radios. One law for both of OUR level controls is the
    // trade taken -- the alternative is a fader whose feel changes depending on
    // which of the three levels the operator happens to be moving.
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
    if (m_receiveBuilding) {
        publishReceiveWindows();
        return;
    }
    const int st = streamForPan(panId);
    if (st < 0)
        return;
    if (panId != primaryPanId() || !primaryVisible() || m_receiveLayout.occupants(st) > 1) {
        const auto w = m_receiveLayout.streams()[st];
        if (m_receiveLayout.setWindow(st, hz, w.rateKsps)) {
            if (primaryVisible() && st == primaryPanIndex()) {
                m_ddcCenterHz = hz;
                scheduleTuneApply();
            } else {
                updateReceiveStreams();
            }
            pushAdditionalShifts();
        }
        publishReceiveWindows();
        return;
    }
    if (!m_ctun) { setSliceFrequency(kSliceId, hz); return; }
    if (intent == PanCenterIntent::Drag && std::isfinite(hz) && hz >= 0 && hz <= 61'440'000) {
        m_pendingBandView.reset();
        m_ddcCenterHz = hz;
        scheduleTuneApply();
    } else { emitPanState(); }
}

int AnanBackend::nearestDdc0RateKsps(int requestedKsps) noexcept
{
    // DDC0 runs at exactly one of these rates (no continuous zoom). Nearest by RATIO
    // (log domain), like Hl2Backend::nearestIqSampleRateHz (HERMES.md §15.1): the
    // rates are octave-spaced and zoom is multiplicative, so 140 ksps belongs to 192
    // (geometric mean 135.8), not 96. The equidistant point (e.g. 96*sqrt(2)) is
    // never an integer request, so no tie-break is needed.
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

int AnanBackend::nearestSessionRateKsps(int requestedKsps) const noexcept
{
    const int rate = nearestDdc0RateKsps(requestedKsps);
    return m_pendingParams.protocol == 1
        ? std::min(rate, m_pendingParams.redPitayaP1 ? 384 : 192) : rate;
}

void AnanBackend::setPanBandwidth(const QString& panId, double hz)
{
    if (!m_additionalRx.empty()) {
        const int st = streamForPan(panId);
        if (st >= 0 && std::isfinite(hz) && hz > 0)
            changeReceiveStreamRate(st, nearestSessionRateKsps(int(std::min(hz / 1000.0, 1536.0))));
        return;
    }
    Q_UNUSED(panId);   // one pan in this phase
    if (!std::isfinite(hz) || hz <= 0.0 || !m_connected) {
        return;
    }
    if (m_bandViewSpanHz > 0 && std::abs(hz - m_bandViewSpanHz) < 0.5) {
        emitPanState(); // zoom-in at the band-view minimum is already satisfied
        return;
    }
    m_pendingBandView.reset(); // a manual span request supersedes band centering
    if (m_bandViewSpanHz > 0) {
        m_bandViewSpanHz = 0.0;
        emitPanState(); // restore full-span bin metadata before subsequent frames
    }
    //DH1KLM: Clamp in floating point before narrowing; oversized finite
    // spans still select the widest supported rate without integer overflow.
    const int requestedKsps = static_cast<int>(std::min(hz / 1000.0 + 0.5,
        static_cast<double>(kDdc0RatesKsps.back())));
    const int snappedKsps = nearestSessionRateKsps(requestedKsps);

    if (snappedKsps == m_pendingParams.ddc0RateKsps) {
        // Already at the closest rate: republish so an optimistic GUI span snaps back
        // (as PanadapterModel::republishCenterBandwidth(), #4470), and clear any pending
        // request so a stale older zoom cannot fire after this one.
        m_pendingBandwidthKsps = 0;
        emitPanState();
        return;
    }

    if (!m_rateChanging) {
        beginRateChange(snappedKsps);
    } else {
        // A reconfigure is already running -- remember the latest request
        // rather than starting a second one on top of it. See m_rateChanging's
        // own comment for why this waits for that cycle to actually finish
        // rather than a fixed cooldown.
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
    // A live rate change: build the new WdspChannel on m_dspBuildThread FIRST (up to
    // ~a minute cold) while the old channel and session keep running undisturbed;
    // only then does finishRateChange() mute, swap and send the new rate to the
    // RUNNING session (P2Client::setDdcRateLive()). Build-before-disturb is what
    // keeps zoom from freezing. m_preRateChangeKsps records the running rate so the
    // failure path can roll back: emitPanState() reports from m_pendingDspConfig.
    m_preRateChangeKsps = m_pendingParams.ddc0RateKsps;
    m_pendingParams.ddc0RateKsps = newRateKsps;
    m_pendingDspConfig.inputSampleRateHz = newRateKsps * 1000;
    // Refresh from CURRENT live operator state, not connectRadio()'s
    // connect-time snapshot: m_pendingDspConfig otherwise only ever had its
    // rate touched here, so a rate change silently reverted mode/filter/AGC
    // to whatever they were at connect. m_shiftHz is not part of Config --
    // AnanRxDsp::installChannel() re-applies it separately, after the swap.
    applyLiveRxState(m_pendingDspConfig);

    m_rateChanging = true;
    ++m_connectGeneration;   // orphans any in-flight prior connect/reconfigure/rebuild
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    // Mark "rebuild in flight" on m_dsp's own thread FIRST, so any
    // setMode/setFilter/setAgc/setShift call that lands while the build is
    // running defers pushing to the (still-live, still-playing) old channel
    // instead of blocking on WDSP's process-wide setup mutex -- see
    // AnanRxDsp::beginRebuild()'s own comment. THEN hand the slow build to
    // the dedicated build thread.
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
        m_pendingBandView.reset();
        // Roll the reported rate back to the one still running BEFORE
        // emitting pan state. AnanDroopCalibrator infers "the rate landed"
        // from pan bandwidth reaching its target; told the failed rate had
        // landed, it would measure the OLD rate's spectrum into the NEW
        // rate's correction table and persist it -- one rate's droop curve
        // applied to another rate's bins, which is exactly the cross-rate
        // corruption anan_rxdsp_handedness_test's Group 6 exists to prevent.
        if (m_preRateChangeKsps > 0) {
            m_pendingParams.ddc0RateKsps = m_preRateChangeKsps;
            m_pendingDspConfig.inputSampleRateHz = m_preRateChangeKsps * 1000;
        }
        emit connectionError(error);
        // Safe to clear m_rateChanging immediately here, unlike
        // startP2ClientSession()'s own failure branch below: the OLD
        // P2Client session was never touched on this path -- beginRateChange()
        // no longer stops it before building (see that function's own
        // comment) -- so there is no live teardown in flight whose
        // linkDown()/linkUp() could leak through unsuppressed. The old
        // channel and old session simply keep running at the OLD rate.
        m_rateChanging = false;
        emitPanState();
        retryPendingRateChange();
        return;
    }

    // The new channel is already installed (queued just before this call). Mute
    // here, not on a later event: frames reach AnanRxDsp::processIqBlock() by
    // DirectConnection on the I/O thread, so anything triggered after the rate
    // lands would be at least one block late.
    QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, true));

    // LIVE rate change on the running session: p2app applies DDC-Specific rate
    // changes by a direct FPGA register write with no teardown (see
    // P2Client::setDdcRateLive()). Until it lands the new channel sees old-rate
    // samples; the mute and settle window cover that. Sent THREE times
    // (kRateChangeResendMs): it is fire-and-forget UDP, the keepalive never resends
    // DDC-Specific, and a lost packet would leave the radio at the old rate with no
    // error while everything here records the new one. Duplicates are no-ops
    // (WriteP2DDCRateRegister() fires on change; HandlerCheckDDCSettings() is
    // empty) (#5547).
    const int rateKsps = m_pendingDspConfig.inputSampleRateHz / 1000;
    auto sendRate = [this, rateKsps, generation]() {
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_client, "setDdcRateLive", Qt::QueuedConnection,
                                  Q_ARG(int, primaryPanIndex()), Q_ARG(int, rateKsps));
    };
    sendRate();
    for (const int delayMs : kRateChangeResendMs)
        QTimer::singleShot(delayMs, this, sendRate);

    QTimer::singleShot(kRateChangeLiveSettleMs, this, [this, generation]() {
        // Same generation guard the restart path used: a newer rate
        // change/connect/disconnect during the settle window supersedes this.
        if (generation != m_connectGeneration)
            return;
        m_rateChanging = false;
        if (m_pendingBandView && m_pendingBandwidthKsps == 0) {
            m_ddcCenterHz = m_pendingBandView->centerHz;
            m_bandViewSpanHz = m_pendingBandView->spanHz;
            m_pendingBandView.reset();
        }
        applyTuneToRadioAndPan();
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, false));
        // A zoom the operator kept turning while this ran: apply the latest
        // request now rather than stranding the display a step behind.
        retryPendingRateChange();
    });
}

void AnanBackend::setPanFrameRate(const QString& panId, int fps)
{
    {
        const int st = streamForPan(panId);
        if (st < 0)
            return;
        for (const auto& [id, r] : m_additionalRx) {
            const auto* p = m_receiveLayout.slice(id);
            if (p && p->stream == st && r->ready) {
                const int value = fps;
                r->config.spectrumFps = value;
                QMetaObject::invokeMethod(
                    r->dsp, [dsp = r->dsp, value] { dsp->setSpectrumRateFps(value); },
                    Qt::QueuedConnection);
            }
        }
        if (panId != primaryPanId() || !primaryVisible())
            return;
    }

    if (fps > 0)
        m_pendingDspConfig.spectrumFps = fps;
    Q_UNUSED(panId);   // one pan in this phase
    // Default no-op inherited from IRadioBackend was written for Flex,
    // whose own display engine paces its frames -- silently wrong here,
    // since it left AnanRxDsp's spectrum production uncapped (measured on
    // the bench: the waterfall scrolled far faster than the operator's own
    // FPS setting, because nothing was ever telling it what that setting
    // was). Mirrors Hl2Backend::setPanFrameRate()'s shape exactly.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumRateFps", Qt::QueuedConnection,
                                  Q_ARG(int, fps));
}

int AnanBackend::maximumReceiveAttenuation() const
{
    return m_pendingParams.ddc0AdcIndex == 0
        && hasClassicAlexAttenuator(m_pendingParams.model,
            m_discoveryInfoReceived ? m_discoveredBoardId : m_pendingParams.expectedBoardId) ? 61 : 31;
}

void AnanBackend::publishReceiveGain()
{

    // Thetis' historical Preamp combo contains attenuation presets, not gain.
    QStringList labels;
    const int maximumPreset = maximumReceiveAttenuation() == 61 ? 50 : 30;
    for (int db = 0; db <= maximumPreset; db += 10) {
        labels.append(db == 0 ? QStringLiteral("0 dB") : QStringLiteral("-%1 dB").arg(db));
    }
    int step = m_attenuationDb / 10;
    if (m_attenuationDb % 10 != 0 || m_attenuationDb > maximumPreset) {
        step = labels.size();
        labels.append(QStringLiteral("-%1 dB (custom)").arg(m_attenuationDb));
    }
    for (int st = 0; st < m_receiveLayout.limit(); ++st)
        if (m_receiveLayout.streams()[st].active) {
            const auto pan = QStringLiteral("anan-%1").arg(st);
            emit panRfGainInfoChanged(pan, -maximumReceiveAttenuation(), 0, 1);
            emit panRfGainChanged(pan, -m_attenuationDb);
            emit panPreampInfoChanged(pan, labels);
    emit panPreampChanged(pan, step);
        }
}

void AnanBackend::setPanPreamp(const QString& panId, int step)
{
    const int maximumStep = maximumReceiveAttenuation() == 61 ? 5 : 3;
    if (step < 0 || step > maximumStep) { publishReceiveGain(); return; }
    if (streamForPan(panId) < 0) { return; }
    const auto preset = receiveAttenuationPreset(step, maximumReceiveAttenuation() == 61);
    applyReceiveGain(preset.adcDb, preset.alexDb);
}

void AnanBackend::setPanRfGain(const QString& panId, int gainDb)
{
    if (streamForPan(panId) < 0) { return; }
    const auto attenuation = receiveAttenuation(-std::clamp(gainDb, -maximumReceiveAttenuation(), 0),
        maximumReceiveAttenuation() == 61);
    applyReceiveGain(attenuation.adcDb, attenuation.alexDb);
}

void AnanBackend::applyReceiveGain(int adcDb, int alexDb)
{
    m_attenuationDb = adcDb + alexDb;
    if (m_pendingParams.ddc0AdcIndex == 1) {
        m_pendingParams.adc1AttenuationDb = adcDb;
    } else {
        m_pendingParams.adc0AttenuationDb = adcDb;
        m_pendingParams.alexAttenuationDb = alexDb;
    }
    if (m_client) {
        QMetaObject::invokeMethod(m_client, "setFrontEndAttenuation", Qt::QueuedConnection,
            Q_ARG(int, m_pendingParams.ddc0AdcIndex), Q_ARG(int, adcDb),
            Q_ARG(int, alexDb));
    }
    publishReceiveGain();
    emit operatingStateChanged();
}

void AnanBackend::setPanAverage(const QString& panId, int average)
{
    {
        const int st = streamForPan(panId);
        if (st < 0)
            return;
        for (const auto& [id, r] : m_additionalRx) {
            const auto* p = m_receiveLayout.slice(id);
            if (p && p->stream == st && r->ready) {
                const int value = std::max(0, average) * kMsPerAverageStep;
                r->config.spectrumAverageMs = value;
                QMetaObject::invokeMethod(
                    r->dsp, [dsp = r->dsp, value] { dsp->setSpectrumAverageMs(value); },
                    Qt::QueuedConnection);
            }
        }
        if (panId != primaryPanId() || !primaryVisible())
            return;
    }

    m_pendingDspConfig.spectrumAverageMs = std::max(0, average) * kMsPerAverageStep;
    Q_UNUSED(panId);   // one pan in this phase
    // The operator's FFT AVG slider, which otherwise reaches nothing on this
    // radio: the panadapter is averaged inside WDSP's analyzer, as a TIME --
    // see kMsPerAverageStep.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumAverageMs", Qt::QueuedConnection,
                                  Q_ARG(int, std::max(0, average) * kMsPerAverageStep));
}

void AnanBackend::setPanWeightedAverage(const QString& panId, bool on)
{
    {
        const int st = streamForPan(panId);
        if (st < 0)
            return;
        for (const auto& [id, r] : m_additionalRx) {
            const auto* p = m_receiveLayout.slice(id);
            if (p && p->stream == st && r->ready) {
                const bool value = on;
                r->config.spectrumLogAverage = value;
                QMetaObject::invokeMethod(
                    r->dsp, [dsp = r->dsp, value] { dsp->setSpectrumLogAverage(value); },
                    Qt::QueuedConnection);
            }
        }
        if (panId != primaryPanId() || !primaryVisible())
            return;
    }

    m_pendingDspConfig.spectrumLogAverage = on;
    Q_UNUSED(panId);   // one pan in this phase
    // deskHPSDR offers the analyzer's averaging MODE alongside its time. The
    // toggle picks between the two recursive modes: on = log-recursive
    // (deskHPSDR's default, the smoother look), off = linear-recursive (an
    // average of power, which reads noise and weak signals at their true
    // level).
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumLogAverage", Qt::QueuedConnection,
                                  Q_ARG(bool, on));
}

void AnanBackend::setPanPixelWidth(const QString& panId, int pixels)
{
    if (pixels < 2)
        return;
    {
        const int st = streamForPan(panId);
        if (st < 0)
            return;
        for (const auto& [id, r] : m_additionalRx) {
            const auto* p = m_receiveLayout.slice(id);
            if (p && p->stream == st && r->ready) {
                const int value = pixels;
                r->config.panPoints = value;
                QMetaObject::invokeMethod(
                    r->dsp, [dsp = r->dsp, value] { dsp->setPanPoints(value); },
                    Qt::QueuedConnection);
            }
        }
        if (panId != primaryPanId() || !primaryVisible())
            return;
    }

    Q_UNUSED(panId);   // one pan in this phase
    // One analyzer point per screen pixel, as deskHPSDR sizes its analyzer
    // from the panel width. A fixed 1024 points stretched across a wider
    // panel is what made the waterfall look soft.
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
    for (const auto& [id, r] : m_additionalRx)
        applyAdditionalRx(id);
    // Both the shift AND the filter depend on the pitch -- HERMES §5:
    // "entering or leaving CW has to re-push the shift, not just the
    // filter." Re-derive both from current state rather than adjusting in
    // place, so this is correct whether or not the mode is currently CW.
    if (m_ctun) { scheduleTuneApply(); }
    pushModeFilterShift();
    emitSliceState();
}

void AnanBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    // canTransmit is false and P2Client has no PTT capability -- there is
    // nothing this method could do even if the engine's TX guard (which
    // gates every call site above this seam) let a key-on request through.
    // A key-on request reaching here at all means that guard was bypassed,
    // which is itself the bug to chase -- not something to silently absorb.
    if (key)
        qWarning("AnanBackend::setKeying(true) called on a receive-only backend "
                 "(canTransmit=false) -- the engine TX guard should have refused this");
}

QString AnanBackend::persistDroopTables(const QMap<int, anan::DroopCorrectionTable>& tables)
{
    if (tables.isEmpty())
        return QStringLiteral("the request carried no valid correction table");
    // Never write an empty radio_id row (AGENTS.md): RadioSettingsScope falls
    // back exact-radio -> family-wide on read, so a row written with no
    // identity would be silently adopted by every ANAN that has none of its
    // own -- the same guard Hl2Backend::applyFreqCalPpb() uses. This is the
    // one thing saveTables() cannot judge for itself: a family-wide scope is
    // perfectly VALID, just not what a per-radio calibration wants.
    if (m_radioSerial.isEmpty()) {
        return QStringLiteral("no radio identity yet -- the correction is live "
                              "for this session only");
    }
    return AnanDroopCalibrator::saveTables(
        RadioSettingsScope(QStringLiteral("anan"), m_radioSerial), tables);
}

QVariantMap AnanBackend::droopStatus() const
{
    // EVERY DDC0 rate, tagged with where its correction came from -- not just
    // the swept ones. Reporting only measuredTables() told the operator
    // "nothing measured" while the defaults connectRadio() seeds were live on
    // the display, so a panadapter that looked wrong had no surface anywhere
    // connecting it to a correction that was in fact being applied. The tag
    // is what keeps "derived from the gateware" and "measured on this radio"
    // distinguishable rather than collapsing them into one list.
    QVariantList corrections;
    const auto& measured = m_droopCalibrator.measuredTables();
    for (const int rateKsps : defaultDroopRatesKsps()) {
        const auto it = measured.constFind(rateKsps);
        const bool isMeasured = it != measured.constEnd();
        // A default is only live once connectRadio() has actually seeded it.
        // Before that the rate genuinely carries no correction and must not
        // claim one -- measured results, by contrast, outlive the connection
        // because the calibrator holds them for the session.
        const DroopCorrectionTable* table =
            isMeasured ? &it.value()
                       : (m_connected && effectiveBoardId() == 10 ? defaultDroopTableForRate(rateKsps) : nullptr);
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
    // Preserve the existing apply/persist behavior. The sweep and its data
    // stay below the seam; clients can apply a result, never inject tables.
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        if (m_dsp) {
            QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, it.key()), Q_ARG(std::vector<float>,
                    std::vector<float>(it.value().begin(), it.value().end())));
        }
    }
    return persistDroopTables(tables);
}

int AnanBackend::receiveAntennaOwner() const
{
    if (!m_connected) { return -1; }
    if (primaryVisible()) { return 0; }
    for (const auto& [id, slice] : m_receiveLayout.slices()) {
        const auto it = m_additionalRx.find(id);
        if (it != m_additionalRx.end() && it->second->ready) { return id; }
    }
    return -1;
}

int AnanBackend::receiveAntennaBandIndex() const
{
    const auto* owner = m_receiveLayout.slice(receiveAntennaOwner());
    const double hz = owner ? owner->frequencyHz : m_sliceFreqHz;
    //DH1KLM GEN and WWV are explicit Aether band slots, rather than Thetis's nearest-band fallback.
    for (const double center : {2.5e6, 5e6, 10e6, 15e6, 20e6, 25e6}) {
        if (std::abs(hz - center) <= 5000) { return 13; }
    }
    const QString planName = AppSettings::instance().value(QStringLiteral("BandPlanName"), QStringLiteral("ARRL (US)")).toString();
    if (!m_antennaBandPlan || m_antennaPlanName != planName) {
        if (!m_antennaBandPlan) { m_antennaBandPlan = std::make_unique<BandPlanManager>(); }
        m_antennaPlanName = planName;
        m_antennaBandPlan->loadPlans();
    }
    const auto band = m_antennaBandPlan->amateurBandAt(hz / 1e6);
    if (!band) { return 12; }
    //DH1KLM These separators identify a regional envelope; its actual edges come from BandPlanManager.
    constexpr std::array<double, 10> separators{2.75, 4.66525, 6.20175, 8.7, 12.075,
                                               16.209, 19.584, 23.17, 26.465, 39.85};
    const double midpoint = (band->lowMhz + band->highMhz) / 2;
    if (midpoint < 1 || midpoint > 61.44) { return 12; }
    return static_cast<int>(std::upper_bound(separators.begin(), separators.end(), midpoint) - separators.begin());
}

ReceiveAntennaBand AnanBackend::receiveAntennaRoute() const
{
    const auto& config = m_pendingParams.filters;
    if (config && config->antennaBands && config->antennaBands->enabled) {
        auto route = config->antennaBands->rows[receiveAntennaBandIndex()];
        //DH1KLM Thetis Alex.cs at 852bf0ef: XVTR is conditional on transverter mode, unavailable here.
        if (route.rxOnly >= 3) { route.rxOnly -= 3; }
        return route;
    }
    return {m_rxAntenna, config ? config->rxOnlyInput : 0};
}

void AnanBackend::applyReceiveAntennaRouting(bool force)
{
    if (!m_client || !m_pendingParams.filters) { return; }
    const auto route = receiveAntennaRoute();
    const bool changed = m_appliedAntenna != route.antenna || m_appliedRxOnly != route.rxOnly;
    if (!force && !changed) { return; }
    auto config = *m_pendingParams.filters;
    config.rxOnlyInput = route.rxOnly;
    const auto options = filterOptions(config);
    //DH1KLM Apply bank and input together, without an intermediate relay command using the old bank.
    QMetaObject::invokeMethod(m_client, "setReceiveHardware", Qt::QueuedConnection,
                              Q_ARG(QVariantMap, options), Q_ARG(int, route.antenna));
    m_appliedAntenna = route.antenna;
    m_appliedRxOnly = route.rxOnly;
}

void AnanBackend::publishReceiveAntennas()
{
    const quint64 generation = m_connectGeneration;
    std::vector<int> receivers;
    for (const auto& [id, rx] : m_additionalRx) { if (rx->ready) { receivers.push_back(id); } }
    emitSliceState();
    for (int id : receivers) {
        if (!m_connected || generation != m_connectGeneration) { return; }
        publishAdditionalRx(id);
    }
}

void AnanBackend::loadHardwareFilters(int boardId)
{
    const auto profile = receiveProfile(boardId, m_pendingParams.model, m_pendingParams.overrideDetection);
    if (!profile) { return; }
    m_rxAntenna = AnanSettings::radioRxAntenna(m_radioSerial,
        static_cast<int>(m_pendingParams.model), boardId);
    m_pendingParams.rxAntenna = m_rxAntenna;
    const auto saved = AnanSettings::hardwareFilters(m_radioSerial,
        static_cast<int>(m_pendingParams.model), boardId);
    FilterConfig config = parseFilterOptions(saved).value_or(
        defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf));
    if (saved.isEmpty()) {
        config.bypassAdc0 = m_pendingParams.bypassAdc0Filters;
        config.bypassAdc1 = m_pendingParams.bypassAdc1Filters;
    }
    //DH1KLM Seed every new row from the saved manual path; upgrades never switch an antenna.
    if (!config.antennaBands) {
        config.antennaBands.emplace();
        for (auto& row : config.antennaBands->rows) { row = {m_rxAntenna, config.rxOnlyInput}; }
    }
    m_pendingParams.filters = config;
    m_pendingParams.bypassAdc0Filters = config.bypassAdc0;
    m_pendingParams.bypassAdc1Filters = config.bypassAdc1;
    if (m_client && m_discoveryInfoReceived) {
        applyReceiveAntennaRouting(true);
    }
}

QVariantMap AnanBackend::hardwareFilterStatus() const
{
    const auto profile = receiveProfile(m_discoveredBoardId, m_pendingParams.model,
                                        m_pendingParams.overrideDetection);
    if (!m_connected || !profile || m_radioSerial.isEmpty()) { return {}; }
    FilterConfig config = m_pendingParams.filters.value_or(
        defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf));
    config.bypassAdc0 = m_pendingParams.bypassAdc0Filters;
    config.bypassAdc1 = m_pendingParams.bypassAdc1Filters;
    QVariantMap state = filterOptions(config);
    const auto defaultEdges = filterOptions(defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf));
    state.insert(QStringLiteral("defaultFilterEdges"), QVariantMap{
        {QStringLiteral("receive"), defaultEdges.value(QStringLiteral("receive"))},
        {QStringLiteral("lowPass"), defaultEdges.value(QStringLiteral("lowPass"))},
    });
    const auto* descriptor = ananModelInfo(m_pendingParams.model);
    DiscoveryReply physical;
    physical.boardId = m_discoveredBoardId;
    state.insert(QStringLiteral("ctun"), m_ctun);
    state.insert(QStringLiteral("ctunAvailable"), !m_droopCalibrator.isRunning() && !m_receiveBuilding && primaryVisible());
    state.insert(QStringLiteral("ddcCenterHz"), m_ddcCenterHz);
    state.insert(QStringLiteral("sliceFrequencyHz"), m_sliceFreqHz);
    state.insert(QStringLiteral("identity"), m_radioSerial);
    state.insert(QStringLiteral("modelId"), static_cast<int>(m_pendingParams.model));
    state.insert(QStringLiteral("boardId"), m_discoveredBoardId);
    state.insert(QStringLiteral("model"), m_pendingParams.model == AnanModel::Automatic
        ? QString::fromLatin1(physical.boardDisplayName())
        : QString::fromLatin1(descriptor->name.data(), descriptor->name.size()));
    state.insert(QStringLiteral("physical"), QString::fromLatin1(physical.boardDisplayName()));
    state.insert(QStringLiteral("override"), m_pendingParams.overrideDetection);
    state.insert(QStringLiteral("protocol"), m_pendingParams.protocol);
    state.insert(QStringLiteral("mkII"), profile->filterKind == ReceiveFilterKind::MkIIBpf);
    state.insert(QStringLiteral("numAdcs"), profile->numAdcs);
    state.insert(QStringLiteral("hasAlex2Filters"), hasAlex2Filters(*profile));
    state.insert(QStringLiteral("selectLpf"), m_pendingParams.protocol == 1 || profile->selectReceiveLpf);
    const auto plan = receiveFilterPlan();
    const auto demands = receiveFilterDemands();
    QStringList reasons;
    constexpr std::array<int, 6> relayBits{6, 5, 4, 1, 2, 3};
    for (int adc = 0; adc < 2; ++adc) {
        const bool manualBypass = adc == 0 ? config.bypassAdc0 : config.bypassAdc1;
        const auto bits = manualBypass ? 0u : plan.receiveBits[adc].value_or(
            receiveFilterBits(config, frontEndReferenceHz(), adc == 1));
        QVariantList rows;
        for (int row = 0; row < 6; ++row) {
            if (bits & (1u << relayBits[row])) { rows.append(row); }
        }
        state.insert(adc == 0 ? QStringLiteral("receiveActive") : QStringLiteral("adc1Active"),
                     rows.isEmpty() ? -1 : rows.front().toInt());
        state.insert(adc == 0 ? QStringLiteral("receiveActiveRows") : QStringLiteral("adc1ActiveRows"), rows);
        if (!manualBypass && plan.selections[adc] > 1) {
            QStringList bands;
            for (const auto& demand : demands) {
                if (demand.adc == adc) {
                    const QString band = BandSettings::bandForFrequency(demand.frequencyHz / 1e6);
                    if (!bands.contains(band)) { bands.append(band); }
                }
            }
            const QString detail = bands.size() > 1 ? bands.join(QStringLiteral(" + "))
                : bands.join(QStringLiteral(" + ")) + QStringLiteral("; different filter ranges");
            reasons.append(QStringLiteral("ADC%1: %2 (multi-band: %3)").arg(adc)
                .arg(plan.combined[adc] ? QStringLiteral("Combined filters") : QStringLiteral("BYPASS"))
                .arg(detail));
            if (plan.combined[adc] && (bits & (1u << 3))) {
                reasons.append(QStringLiteral("6m BPF/LNA retained; 12/10m row excluded while sharing this input."));
            }
        }
    }
    const double lpfHz = plan.lowPassHz.value_or(frontEndReferenceHz());
    state.insert(QStringLiteral("lpfActive"), config.lpfBypassOnRx || selectedFilterBand(config.lowPass, lpfHz) < 0
        ? 6 : selectedFilterBand(config.lowPass, lpfHz));
    if ((m_pendingParams.protocol == 1 || profile->selectReceiveLpf) &&
        !config.lpfBypassOnRx && plan.lowPassSelections > 1) {
        reasons.append(QStringLiteral("LPF follows highest DDC centre: %1 MHz (%2)")
            .arg(lpfHz / 1e6, 0, 'f', 6).arg(BandSettings::bandForFrequency(lpfHz / 1e6)));
    }
    state.insert(QStringLiteral("filterPolicyText"), reasons.join(QStringLiteral("\n")));
    state.insert(QStringLiteral("frequencyHz"), frontEndReferenceHz());
    state.insert(QStringLiteral("ocMask"), receiveOcMask(config, frontEndReferenceHz()));
    QStringList inputNames;
    for (const auto name : receiveInputNames(m_pendingParams.model, m_discoveredBoardId)) {
        inputNames.append(QString::fromLatin1(name.data(), static_cast<int>(name.size())));
    }
    state.insert(QStringLiteral("rxInputNames"), inputNames);
    const auto antennaRoute = receiveAntennaRoute();
    state.insert(QStringLiteral("antenna"), antennaRoute.antenna);
    state.insert(QStringLiteral("activeRxOnlyInput"), antennaRoute.rxOnly);
    state.insert(QStringLiteral("antennaOwner"), receiveAntennaOwner());
    state.insert(QStringLiteral("antennaBandRow"), receiveAntennaBandIndex());
    state.insert(QStringLiteral("host"), m_pendingParams.host);
    state.insert(QStringLiteral("firmware"), m_discoveredFirmwareVer);
    state.insert(QStringLiteral("reportedReceivers"), m_discoveredReceiverCount);
    state.insert(QStringLiteral("rateKsps"), m_pendingParams.ddc0RateKsps);
    QVariantList sampleRates;
    for (const int hz : capabilities().sampleRatesHz) {
        sampleRates.append(hz);
    }
    state.insert(QStringLiteral("sampleRatesHz"), sampleRates);
    QVariantList rateWindows;
    for (int stream = 0; stream < m_receiveLayout.limit(); ++stream) {
        const auto& window = m_receiveLayout.streams()[stream];
        if (!window.active) { continue; }
        const bool single = m_additionalRx.empty();
        int anchor = -1;
        for (const auto& [id, slice] : m_receiveLayout.slices()) {
            if (slice.stream == stream) { anchor = id; break; }
        }
        rateWindows.append(QVariantMap{
            {QStringLiteral("panId"), QStringLiteral("anan-%1").arg(stream)},
            {QStringLiteral("label"), QStringLiteral("Window %1").arg(stream + 1)},
            {QStringLiteral("sliceId"), anchor},
            {QStringLiteral("rateHz"), single ? static_cast<int>(receiveTuningRateHz()) : window.rateKsps * 1000}});
    }
    state.insert(QStringLiteral("sampleRateWindows"), rateWindows);
    state.insert(QStringLiteral("sampleRateBusy"), m_rateChanging || m_receiveBuilding);
    state.insert(QStringLiteral("sampleRateAvailable"), m_discoveryInfoReceived &&
        !m_rateChanging && !m_receiveBuilding && !m_droopCalibrator.isRunning());
    state.insert(QStringLiteral("adcIndex"), m_pendingParams.ddc0AdcIndex);
    state.insert(QStringLiteral("dither"), m_pendingParams.ditherEnabled);
    state.insert(QStringLiteral("random"), m_pendingParams.randomEnabled);
    return state;
}

void AnanBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    int bandSliceId = m_activeReceiveSlice;
    if (ns == QLatin1String("anan") && verb == QLatin1String("band.select")) {
        const auto fields = arg.toMap();
        if (fields.contains(QStringLiteral("sliceId"))) {
            const QVariant target = fields.value(QStringLiteral("sliceId"));
            if (target.metaType().id() != QMetaType::Int ||
                !m_receiveLayout.slice(target.toInt())) {
                emit extensionError(requestId, QStringLiteral("Band selection target no longer exists"));
                return;
            }
            bandSliceId = target.toInt();
        }
    }
    if(ns==QLatin1String("anan")&&!m_additionalRx.empty()&&verb==QLatin1String("band.select")) {
        const auto fields=arg.toMap();const double hz=fields.value(QStringLiteral("frequencyHz")).toDouble();
        const auto mode=fields.value(QStringLiteral("mode")).toString();const auto band=fields.value(QStringLiteral("band")).toString();
        if(!m_connected||m_receiveBuilding||!isRestorableFrequencyHz(hz)||!isKnownModeString(mode)
            ||(band!=QLatin1String("GEN")&&band!=QLatin1String("WWV")&&BandSettings::bandForFrequency(hz/1e6)!=band)) {
            emit extensionError(requestId,QStringLiteral("Band selection requires a valid frequency, mode and ready receiver"));return;
        }
        const int id=bandSliceId;setSliceFrequency(id,hz);
        const auto* selected=m_receiveLayout.slice(id);
        if(!selected||std::abs(selected->frequencyHz-hz)>0.5) {
            emit extensionError(requestId,QStringLiteral("No receive stream is available for this band"));return;
        }
        setSliceMode(id,mode);
        emit extensionResult(requestId,QVariantMap{{QStringLiteral("restored"),false},{QStringLiteral("ctun"),id==0&&m_ctun},{QStringLiteral("frequencyHz"),hz}});
        return;
    }
    if(ns==QLatin1String("anan")&&!m_additionalRx.empty()&&verb==QLatin1String("band.center")) {
        emit extensionError(requestId,QStringLiteral("Band-view memory requires the original receiver to be the only slice"));return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("band.select")) {
        const auto fields = arg.toMap();
        const QString band = fields.value(QStringLiteral("band")).toString();
        const double hz = fields.value(QStringLiteral("frequencyHz")).toDouble();
        const QString mode = fields.value(QStringLiteral("mode")).toString();
        if (!m_connected || !m_discoveryInfoReceived || m_radioSerial.isEmpty()
            || m_droopCalibrator.isRunning() || !m_bandViewMemorySupported
            || !isRestorableFrequencyHz(hz)
            || (band != QLatin1String("GEN") && band != QLatin1String("WWV")
                && BandSettings::bandForFrequency(hz / 1e6) != band)
            || !isKnownModeString(mode)) {
            emit extensionError(requestId, QStringLiteral("ANAN band recall refused: invalid band, disconnected hardware, unsupported memory or active calibration."));
            return;
        }
        rememberBandView();
        const auto profile = m_bandViewMemory.value(QStringLiteral("profiles")).toObject()
            .value(bandViewProfileKey()).toObject();
        //DH1KLM Recall the latest state, including CTUN off, rather than an older active B view.
        const auto record = profile.value(QStringLiteral("bands")).toObject().value(band).toObject();
        const auto view = record.value(QStringLiteral("view")).toObject();
        const bool restored = band != QLatin1String("GEN") && band != QLatin1String("WWV")
            && m_bandViewMemory.value(QStringLiteral("radioSerial")).toString() == m_radioSerial
            && validBandViewSnapshot(view)
            && BandSettings::bandForFrequency(view.value(QStringLiteral("vfoHz")).toDouble() / 1e6) == band;
        if (restored) {
            restoreBandView(record);
        } else {
            m_beforeBandView = {};
            m_currentViewBand = BandSettings::bandForFrequency(hz / 1e6);
            m_ctun = false;
            m_bandViewSpanHz = 0;
            m_pendingBandView.reset();
            if (m_mode.compare(mode, Qt::CaseInsensitive) != 0) {
                const auto [low, high] = defaultPassbandForMode(mode);
                m_filterLowHz = low;
                m_filterHighHz = high;
            }
            m_mode = mode;
            m_sliceFreqHz = hz;
            m_tuneThrottleTimer->stop();
            m_tunePendingApply = false;
            pushModeFilterShift();
            applyTuneToRadioAndPan();
            emitSliceState();
        }
        emit extensionResult(requestId, QVariantMap{{QStringLiteral("restored"), restored},
            {QStringLiteral("ctun"), m_ctun}, {QStringLiteral("frequencyHz"), m_sliceFreqHz}});
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("band.center")) {
        if (!m_connected || !m_discoveryInfoReceived || m_radioSerial.isEmpty()
            || m_droopCalibrator.isRunning() || !m_bandViewMemorySupported) {
            emit extensionError(requestId, QStringLiteral("Band view requires validated hardware, supported memory and no active droop sweep."));
            return;
        }
        if (validBandViewSnapshot(m_beforeBandView)) {
            auto previous = m_beforeBandView;
            previous.insert(QStringLiteral("ctun"), false);
            previous.insert(QStringLiteral("centerHz"), m_sliceFreqHz);
            previous.insert(QStringLiteral("vfoHz"), m_sliceFreqHz);
            restoreBandView(QJsonObject{{QStringLiteral("view"), previous}}, true);
            emit extensionResult(requestId, QVariantMap{{QStringLiteral("ctun"), false},
                {QStringLiteral("rateKsps"), previous.value(QStringLiteral("rateKsps")).toInt()},
                {QStringLiteral("pending"), m_rateChanging}});
            return;
        }

        const auto fields = arg.toMap();
        bool lowOk = false, highOk = false;
        const double low = fields.value(QStringLiteral("lowHz")).toDouble(&lowOk);
        const double high = fields.value(QStringLiteral("highHz")).toDouble(&highOk);
        const auto view = lowOk && highOk ? receiveBandView(low, high, m_sliceFreqHz,
            nearestSessionRateKsps(kDdc0RatesKsps.back()), m_filterLowHz, m_filterHighHz, cwBfoHz())
            : std::nullopt;
        if (!m_connected || !m_discoveryInfoReceived || m_radioSerial.isEmpty()
            || m_droopCalibrator.isRunning() || !view) {
            emit extensionError(requestId, QStringLiteral("Band view requires a validated ANAN, a regional band containing RX, and no active droop sweep."));
            return;
        }
        rememberBandView();
        m_beforeBandView = bandViewSnapshot();
        m_currentViewBand = BandSettings::bandForFrequency(m_sliceFreqHz / 1e6);
        m_tuneThrottleTimer->stop();
        m_tunePendingApply = false;
        m_ctun = true;
        setPanBandwidth(primaryPanId(), view->rateKsps * 1000.0);
        if (m_rateChanging) {
            // Do not clamp the new centre against the old narrow sample rate.
            m_pendingBandView = *view;
        } else {
            m_ddcCenterHz = view->centerHz;
            m_bandViewSpanHz = view->spanHz;
            applyTuneToRadioAndPan();
        }
        emit extensionResult(requestId, QVariantMap{
            {QStringLiteral("rateKsps"), view->rateKsps},
            {QStringLiteral("wholeBand"), view->wholeBand},
            {QStringLiteral("ctun"), true},
            {QStringLiteral("pending"), m_rateChanging}});
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("sample-rate.set")) {
        const QVariantMap state = hardwareFilterStatus();
        const QVariantMap fields = arg.toMap();
        if (state.isEmpty() || !state.value(QStringLiteral("sampleRateAvailable")).toBool()) {
            emit extensionError(requestId, QStringLiteral("Sample rate requires a connected ANAN with no receive rebuild or droop sweep running."));
            return;
        }
        for (const QString& key : {QStringLiteral("identity"), QStringLiteral("modelId"), QStringLiteral("boardId")}) {
            if (fields.value(key) != state.value(key)) {
                emit extensionError(requestId, QStringLiteral("The connected hardware changed; reload this page."));
                return;
            }
        }
        const QVariant rate = fields.value(QStringLiteral("rateHz"));
        const int stream = streamForPan(fields.value(QStringLiteral("panId")).toString());
        const QVariant anchor = fields.value(QStringLiteral("sliceId"));
        const auto* slice = anchor.metaType().id() == QMetaType::Int ? m_receiveLayout.slice(anchor.toInt()) : nullptr;
        if (rate.metaType().id() != QMetaType::Int ||
            !capabilities().sampleRatesHz.contains(rate.toInt()) || stream < 0 || !slice || slice->stream != stream) {
            emit extensionError(requestId, QStringLiteral("Select an active receive window and a supported sample rate."));
            return;
        }
        //DH1KLM Thetis setup.cs InitAudioTab and RX1/RX2 rate handlers at 852bf0ef:
        //DH1KLM P1 has one wire rate; P2 permits independent receive captures.
        if (!m_additionalRx.empty()) {
            if (!changeReceiveStreamRate(stream, rate.toInt() / 1000)) {
                emit extensionError(requestId, QStringLiteral("This rate would exclude a slice sharing a receive window. Choose a wider rate."));
                return;
            }
        } else {
            m_pendingBandView.reset();
            m_bandViewSpanHz = 0.0;
            setPanBandwidth(primaryPanId(), rate.toInt());
        }
        emit extensionResult(requestId, hardwareFilterStatus());
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("ctun.set")) {
        if (!m_connected || !m_discoveryInfoReceived || m_radioSerial.isEmpty()
            || arg.metaType().id() != QMetaType::Bool
            || m_droopCalibrator.isRunning()) {
            emit extensionError(requestId, QStringLiteral("CTUN requires a connected ANAN and no active droop sweep"));
            return;
        }
        m_tuneThrottleTimer->stop();
        m_tunePendingApply = false;
        m_pendingBandView.reset();
        m_ctun = arg.toBool();
        if (!m_ctun) { m_bandViewSpanHz = 0.0; m_beforeBandView = {}; }
        applyTuneToRadioAndPan();
        emitSliceState();
        emit extensionResult(requestId, hardwareFilterStatus());
        return;
    }
    if (ns == QLatin1String("anan") && (verb == QLatin1String("hw.get")
        || verb == QLatin1String("hw.set") || verb == QLatin1String("hw.defaults"))) {
        const QVariantMap state = hardwareFilterStatus();
        if (state.isEmpty()) {
            emit extensionError(requestId, QStringLiteral("Connect an ANAN with a validated receive profile first."));
            return;
        }
        if (verb != QLatin1String("hw.get")) {
            const QVariantMap fields = arg.toMap();
            if (fields.value(QStringLiteral("identity")) != state.value(QStringLiteral("identity"))
                || fields.value(QStringLiteral("modelId")) != state.value(QStringLiteral("modelId"))
                || fields.value(QStringLiteral("boardId")) != state.value(QStringLiteral("boardId"))) {
                emit extensionError(requestId, QStringLiteral("The connected hardware changed; reload this page."));
                return;
            }
            const QVariantMap options = verb == QLatin1String("hw.defaults")
                ? filterOptions(defaultFilterConfig(state.value(QStringLiteral("mkII")).toBool())) : fields;
            auto config = parseFilterOptions(options);
            //DH1KLM Older callers and filter resets retain the antenna table they do not own.
            if (config && !config->antennaBands && m_pendingParams.filters) {
                config->antennaBands = m_pendingParams.filters->antennaBands;
            }
            //DH1KLM Legacy callers do not own the independent SWL assignments.
            if (config && verb == QLatin1String("hw.set") && !fields.contains(QStringLiteral("ocSwl"))
                && m_pendingParams.filters) { config->ocSwl = m_pendingParams.filters->ocSwl; }
            if (!config || !AnanSettings::setHardwareFilters(m_radioSerial,
                    static_cast<int>(m_pendingParams.model), m_discoveredBoardId, filterOptions(*config))) {
                emit extensionError(requestId, QStringLiteral("Receive settings were not changed. Filter/OC ranges require integer Hz within 0–61.44 MHz, distinct OC ranges and masks 0–127. Band antennas require ANT1–3 and RX-only values 0–5. Device settings must be writable."));
                return;
            }
            m_pendingParams.filters = *config;
            m_pendingParams.bypassAdc0Filters = config->bypassAdc0;
            m_pendingParams.bypassAdc1Filters = config->bypassAdc1;
            applyReceiveAntennaRouting(true);
            updateReceiveStreams();
            publishReceiveAntennas();
        }
        emit extensionResult(requestId, hardwareFilterStatus());
        return;
    }
    //DH1KLM: ALEX RX antenna control stays behind the ANAN extension. Only
    // the validated device's saved receive path is changed.
    if (ns == QLatin1String("anan") && verb == QLatin1String("rx-antenna.get")) {
        if (requestId != 0) {
            emit extensionResult(requestId, QVariantMap{
                {QStringLiteral("antenna"), receiveAntennaRoute().antenna},
            });
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("rx-antenna.set")) {
        bool ok = false;
        const int antenna = arg.toInt(&ok);
        if (!ok || antenna < 1 || antenna > 3) {
            emit extensionError(requestId, QStringLiteral("RX antenna must be 1 (ANT1), 2 (ANT2), or 3 (ANT3)"));
            return;
        }
        const QString error = applyReceiveAntenna(antenna);
        if (!error.isEmpty()) { emit extensionError(requestId, error); return; }
        if (requestId != 0) {
            emit extensionResult(requestId, QVariantMap{
                {QStringLiteral("antenna"), receiveAntennaRoute().antenna},
            });
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("nb.get")) {
        if (requestId != 0) {
            const AnanRxDsp::NoiseBlankerState applied = m_dsp
                ? m_dsp->noiseBlankerState() : AnanRxDsp::NoiseBlankerState{};
            const QVariantMap receiver{
                {QStringLiteral("ddc"), 0},
                {QStringLiteral("panId"), primaryPanId()},
                // `on` stays, and stays first: it is what every existing script
                // reads. `kind` is the same fact with the second blanker in it.
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
            if (m_ctun) {
                QObject::disconnect(errorConnection);
                emit extensionError(requestId, QStringLiteral("Disable CTUN before starting a droop sweep"));
                return;
            }
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
    // Index is ours to choose -- nothing on a P2 radio assigns meter ids. Same
    // name, unit and range as Hl2Backend::defineMeters()'s receive meter, the
    // bare "SLC:LEVEL" MeterModel binds to the S-meter. Receive-only: no TX
    // meters until this backend can transmit.
    MeterDef d;
    d.index = 1;
    d.source = QStringLiteral("SLC");
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level");
    emit meterDefined(d);

    // The supply rail. "+13.8A" is the name MeterModel binds to the status bar
    // and Meter applet voltage readouts, whatever the radio; unit and range as
    // IcomMeters declares the same meter.
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
    // Nothing measured yet: the consumer keeps whatever source it already had
    // rather than being handed a snapshot of zeros.
    if (!m_connected || !m_linkCountersSeen)
        return s;
    s.reported = true;
    s.alive = m_linkCounters.rxBytes > m_lastSnapshotRxBytes;
    s.rxBytes = static_cast<qint64>(m_linkCounters.rxBytes);
    s.txBytes = static_cast<qint64>(m_linkCounters.txBytes);
    s.rxPackets = m_linkCounters.rxPackets;
    s.rxPacketsLost = m_linkCounters.drops;
    s.localEndpoint = m_linkCounters.localEndpoint;
    // LEFT AT -1, WHICH THE STRUCT DEFINES AS "NOT MEASURED" AND THE MODEL
    // HONOURS BY IGNORING. This transport is stream-only: the radio is told
    // where to send and then sends, with no request/response exchange to time,
    // so there is no round trip here to measure. Reporting 0 instead would
    // render as a perfect "< 1 ms" link -- a measurement never taken.
    // rttMs, jitterMs, gapMs and gapMaxMs all keep their -1 defaults.
    return s;
}

void AnanBackend::onLinkCounters(const P2Client::LinkCounters& counters)
{
    // A session restart shows as a BACKWARD jump, since P2Client::start() zeroes
    // its counters; the cache is then a dead session's and is discarded here,
    // whichever path restarted it. The first snapshot of a session compares
    // against ZERO: against itself, it would read dead on the first tick.
    if (counters.rxBytes < m_linkCounters.rxBytes)
        m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = m_linkCountersSeen ? m_linkCounters.rxBytes : 0;
    m_linkCounters = counters;
    m_linkCountersSeen = true;
    emit linkStatsUpdated(linkStats());
}

void AnanBackend::onDspMeter(float dbfs)
{
    if (!primaryVisible())
        return;
    const double dbm = static_cast<double>(dbfs) + kSMeterDbmOffset + m_attenuationDb;
    // Smooth EVERY reading, publish only on the tick -- the same smoother,
    // and the same reasons, as Hl2Backend's receive meter (SMeterSmoother).
    // AnanRxDsp reads the tap at one DSP-rate block's cadence whatever the
    // DDC0 rate (WdspSMeter::emitEveryBlocks), so this sees ~47 readings a
    // second at 48 and at 1536 ksps alike.
    if (const auto out = m_sMeter.feed(dbm))
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), *out);
}

//DH1KLM: Retain discovery identity separately; an explicit override owns
// board-dependent telemetry/default selection as well as RX packet encoding.
int AnanBackend::effectiveBoardId() const
{
    const auto* model = ananModelInfo(m_pendingParams.model);
    return m_pendingParams.overrideDetection && model
        ? model->nativeBoardId : m_discoveredBoardId;
}

void AnanBackend::onSupplyRailCounts(int counts)
{
    if (!m_supplyRailClock.isValid())
        m_supplyRailClock.start();
    onSupplyRailCountsAt(counts, m_supplyRailClock.elapsed());
}

void AnanBackend::onSupplyRailCountsAt(int counts, qint64 nowMs)
{
    //DH1KLM: Publish only for the hardware families with the qualified scale.
    if (m_discoveryInfoReceived && effectiveBoardId() != 5 && effectiveBoardId() != 10) {
        return;
    }
    // A 12-bit ADC reads 0..4095. Anything else means the field is not this
    // input, and no reading is better than a plausible wrong voltage.
    if (counts < 0 || counts > 4095)
        return;
    m_supplyRailCountSum += counts;
    ++m_supplyRailSamples;
    // The first window opens on its first count; each later one opens where the
    // last closed, so publications stay a fixed window apart rather than
    // drifting by a packet interval each time.
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
    syncReceiveAudioRoutes();
    for (const auto& [id, r] : m_additionalRx)
        publishAdditionalRx(id);
    if (!primaryVisible())
        return;
    SliceDelta d;
    d.frequency = m_sliceFreqHz / 1.0e6;   // SliceDelta::frequency is MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.active = m_activeReceiveSlice == 0;
    // A different radio replaces the slice model but retains this backend.
    // Publish the retained NB request so that fresh model agrees with the DSP.
    d.nb = m_nbKind != AetherSDR::NoiseBlankerKind::Off;
    d.nbKind = m_nbKind;
    d.nbLevel = m_nbLevel;
    d.nbFill = m_nbFill;
    // The AGC as the DSP runs it, so the slice's knob starts from this
    // backend's values, not SliceModel's defaults.
    d.agcMode = agcModeName(m_agcMode);
    d.agcThreshold = m_agcThreshold;
    d.agcOffLevel = m_agcOffLevel;
    // The audio stage as APPLIED. Both are required, not cosmetic: the receive
    // control gate refuses an operation whose observation is absent, so without
    // these two the mute and the fader are offered and then rejected.
    //
    // Safe to echo unconditionally -- SliceModel::applyChanges() assigns these
    // into its observation without re-emitting a command, so a published value
    // cannot loop back as a fresh intent (Principle II).
    d.audioGain = m_sliceAudioGainPercent;
    d.audioMute = m_sliceAudioMuted;
    // Without this, RadioModel::sliceChanged's handler never assigns the
    // slice a panId (SliceDelta::panId is std::optional and SliceModel::
    // applyChanges() only touches it when set) -- the slice materialised by
    // "Gap B" stays permanently unassociated with the pan emitPanState()
    // creates, and every click/drag-tune on the panadapter silently no-ops
    // because MainWindow_Wiring.cpp's tune-target resolver matches on
    // panId equality. Hl2Backend::emitSliceState() sets this every time for
    // the same reason.
    d.panId = primaryPanId();
    const auto antenna = activeReceiveAntenna();
    QStringList ports;
    for (std::size_t n = 0; n < antenna.count; ++n) {
        ports.append(QString::fromLatin1(antenna.ports[n].data(), static_cast<int>(antenna.ports[n].size())));
    }
    const QString current = QString::fromLatin1(antenna.current.data(), static_cast<int>(antenna.current.size()));
    if (!ports.isEmpty()) {
        d.rxAntennaList = ports;
        d.rxAntenna = current;
    }
    const quint64 generation = m_connectGeneration;
    emit sliceChanged(kSliceId, d);
    if (!ports.isEmpty() && m_connected && generation == m_connectGeneration) {
        emit panAntennaListChanged(primaryPanId(), ports);
        if (m_connected && generation == m_connectGeneration) { emit panRxAntennaChanged(primaryPanId(), current); }
    }
}

ReceiveAntennaSelection AnanBackend::activeReceiveAntenna() const
{
    if (!m_connected || !m_discoveryInfoReceived) { return {}; }
    const auto profile = receiveProfile(m_discoveredBoardId, m_pendingParams.model, m_pendingParams.overrideDetection);
    if (!profile) { return {}; }
    const auto route = receiveAntennaRoute();
    return receiveAntennaSelection(profile->numAdcs, m_pendingParams.ddc0AdcIndex,
        route.rxOnly, route.antenna,
        receiveInputNames(m_pendingParams.model, m_discoveredBoardId));
}

QString AnanBackend::applyReceiveAntenna(int antenna)
{
    if (!m_connected || !m_client || m_radioSerial.isEmpty() || !m_discoveryInfoReceived) {
        return QStringLiteral("Connect a validated ANAN before changing its RX antenna");
    }
    if (antenna < 1 || antenna > 3) { return QStringLiteral("RX antenna must be ANT1, ANT2 or ANT3"); }
    if (m_pendingParams.filters && m_pendingParams.filters->antennaBands
        && m_pendingParams.filters->antennaBands->enabled) {
        auto config = *m_pendingParams.filters;
        auto& row = config.antennaBands->rows[receiveAntennaBandIndex()];
        row = {antenna, 0};
        if (!AnanSettings::setHardwareFilters(m_radioSerial, static_cast<int>(m_pendingParams.model),
                                              m_discoveredBoardId, filterOptions(config))) {
            return QStringLiteral("Band antenna was not changed: device settings could not be saved");
        }
        m_pendingParams.filters = config;
        applyReceiveAntennaRouting();
        publishReceiveAntennas();
        return {};
    }
    if (!AnanSettings::setRadioRxAntenna(m_radioSerial,
            static_cast<int>(m_pendingParams.model), m_discoveredBoardId, antenna)) {
        return QStringLiteral("RX antenna was not changed: device settings could not be saved");
    }
    m_rxAntenna = antenna;
    m_pendingParams.rxAntenna = antenna;
    applyReceiveAntennaRouting();
    publishReceiveAntennas();
    return {};
}

ReceiveDispatch AnanBackend::requestSliceRxAntenna(int sliceId, const QString& antenna)
{
    const auto route = activeReceiveAntenna();
    const bool automatic = m_pendingParams.filters && m_pendingParams.filters->antennaBands
        && m_pendingParams.filters->antennaBands->enabled;
    if (automatic && sliceId != receiveAntennaOwner()) {
        if (sliceId == 0) { emitSliceState(); } else { publishAdditionalRx(sliceId); }
        return ReceiveDispatch::Unsupported;
    }
    if (m_receiveLayout.slice(sliceId) && route.count == 3) {
        for (int n = 0; n < 3; ++n) {
            if (antenna == QString::fromLatin1(route.ports[n].data(), static_cast<int>(route.ports[n].size()))) {
                if (applyReceiveAntenna(n + 1).isEmpty()) { return ReceiveDispatch::Dispatched; }
                break;
            }
        }
    } else if (m_receiveLayout.slice(sliceId) && route.count == 1
               && antenna == QString::fromLatin1(route.current.data(), static_cast<int>(route.current.size()))) {
        return ReceiveDispatch::LocalOnly;
    }
    // Restore an optimistic widget selection after a refused or unsaved request.
    if (m_connected) { emitSliceState(); }
    return ReceiveDispatch::Unsupported;
}

void AnanBackend::emitPanState()
{
    if (!primaryVisible())
        return;
    const double sampleRateHz = receiveTuningRateHz();
    // Calibration always observes the hardware capture, before display cropping.
    m_droopCalibrator.setLandedRate(static_cast<int>(sampleRateHz / 1000.0));
    const double spanHz = m_bandViewSpanHz > 0 ? std::min(m_bandViewSpanHz, sampleRateHz) : sampleRateHz;
    emit panBandwidthLimitsChanged(primaryPanId(),
        std::min(48'000.0, spanHz) / 1.0e6,
        nearestSessionRateKsps(kDdc0RatesKsps.back()) * 1000.0 / 1.0e6);
    emit panCenterBandwidthChanged(primaryPanId(), m_ddcCenterHz / 1.0e6, spanHz / 1.0e6);
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
        // Already at the stream's rate: no converter, and none is built rather
        // than a 1:1 one being built and trusted to be transparent. A ratio-1
        // resampler still costs a filter and still adds its group delay.
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

    // Split before converting. The two channels have to travel through separate
    // converters to keep their difference (see the member declaration), and a
    // deinterleaved copy is what a mono converter takes.
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
        // Identical configuration and identical input length, so these agree;
        // taking the shorter is not a correction but a refusal to walk off the
        // end of one buffer if they ever stop agreeing. A converter's first
        // calls legitimately return 0 while its filter fills.
        const int n = std::min(nL, nR);
        if (n <= 0) {
            return;
        }
        outFrames = static_cast<std::size_t>(n);
        outL = reinterpret_cast<const float*>(m_speakerOutL.constData());
        outR = reinterpret_cast<const float*>(m_speakerOutR.constData());
    }

    // The RADIO's own level, on top of the per-slice stage; applied here so turning
    // the radio down never quiets the computer's speakers. A mute sends SILENCE, it
    // does not stop sending: stopping would underflow the codec FIFO (polluting the
    // underflow signal and clicking on unmute) and invalidate the pacer's estimate.
    const float lineout = m_lineoutMuted ? 0.0f
                                         : sliceAudioAmplitude(m_lineoutGainPercent);

    // Interleave and quantise. sliceAudioToInt16() carries the
    // clamp-before-scale law and the non-finite guard, with the tests for both.
    m_speakerInterleaved.resize(outFrames * 2);
    for (std::size_t i = 0; i < outFrames; ++i) {
        m_speakerInterleaved[i * 2] = sliceAudioToInt16(outL[i] * lineout);
        m_speakerInterleaved[i * 2 + 1] = sliceAudioToInt16(outR[i] * lineout);
    }

    // One queued hand-off per block. P2Client owns the socket, the packetising
    // and the pacing on its own thread; this side owns the DSP and the audio
    // stage. Copying the block is the price of that boundary and it is a few
    // hundred bytes.
    QMetaObject::invokeMethod(m_client, "enqueueSpeakerAudio", Qt::QueuedConnection,
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(m_speakerInterleaved.data()),
                                     static_cast<qsizetype>(outFrames * 2 * sizeof(qint16)))));
}

void AnanBackend::pushReceiveShift(double shiftHz)
{
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setShift", Qt::QueuedConnection, Q_ARG(double, shiftHz));
    }
}

double AnanBackend::receiveTuningRateHz() const
{
    if (m_rateChanging && m_preRateChangeKsps > 0) { return m_preRateChangeKsps * 1000.0; }
    return m_pendingDspConfig.inputSampleRateHz > 0 ? m_pendingDspConfig.inputSampleRateHz : 48000.0;
}

void AnanBackend::scheduleTuneApply()
{
    if (!m_tuneThrottleTimer->isActive()) {
        // Leading edge: nothing in flight, apply immediately so the first
        // move of a gesture has no added latency.
        applyTuneToRadioAndPan();
        m_tuneThrottleTimer->start(kTuneThrottleMs);
    } else {
        // Already inside a cooldown window from a very recent apply --
        // remember that a trailing catch-up is owed once it elapses, rather
        // than doing the expensive work again right now. m_sliceFreqHz
        // already holds the latest value (set by the caller before this
        // runs), so the timeout handler always applies the newest position,
        // not a stale intermediate one.
        m_tunePendingApply = true;
    }
}

void AnanBackend::applyTuneToRadioAndPan()
{
    const auto tuning = receiveTuning(
        m_ctun || m_receiveLayout.occupants(primaryPanIndex()) > 1,
        m_ddcCenterHz, m_sliceFreqHz,
        receiveTuningRateHz(), m_filterLowHz, m_filterHighHz, cwBfoHz());
    if (!tuning) { emitSliceState(); return; }
    m_ddcCenterHz = tuning->centerHz;
    if (m_connected && primaryVisible()) {
        if (m_receiveLayout.slices().size() == 1)
            m_receiveLayout.tune(0, m_sliceFreqHz, m_ctun);
        m_receiveLayout.setWindow(primaryPanIndex(), m_ddcCenterHz,
                                  int(receiveTuningRateHz() / 1000));
        m_lastSentCenterHz = m_ddcCenterHz;
        updateReceiveStreams();
        pushAdditionalShifts();
    }
    pushReceiveShift(tuning->shiftHz);
    emitPanState();
    rememberBandView();
    // Tuning capture, once per settled gesture. Outside the connected guard:
    // where the dial ended up is worth keeping even during a teardown.
    emit operatingStateChanged();
}

}  // namespace AetherSDR::anan
