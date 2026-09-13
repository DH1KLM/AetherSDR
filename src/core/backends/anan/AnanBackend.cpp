#include "core/backends/anan/AnanBackend.h"

#include <QHostAddress>
#include <QMetaObject>
#include <QTimer>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace AetherSDR::anan {

const QString AnanBackend::kPanId = QStringLiteral("anan-0");

namespace {

// Placeholder until a bench measurement exists -- see the class comment.
// Named and zero rather than silently absent, so the next reader finds a
// TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
           static_cast<qsizetype>(v.size() * sizeof(float))};
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
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("CWL")) return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM"))
        return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    // WDSP has no dedicated RTTY demod (WdspChannel::Mode has no Rtty member --
    // RTTY is FSK decoded by an external app from a wide IQ passband, the same
    // arrangement DIGU already gives every other digital mode here). Found by
    // `radiocert tune`'s mode-map stage: RTTY silently fell through to the
    // USB fallback below -- readback still said "RTTY" (the slice keeps
    // whatever string it's given), so the passband LOOKED digital-shaped by
    // coincidence (this function's own USB fallback and DIGU's passband
    // aren't the same numbers) while the demod underneath was actually USB.
    // Exactly the HERMES.md 15.7/16.7 shape this file already cites for
    // CW/NFM -- same bug, a mode neither of those fixes happened to cover.
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM"))
        return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;
}

std::pair<int, int> AnanBackend::defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    if (u == QLatin1String("RTTY")) return {150, 3000};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL"))
        return {-250, 250};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM"))
        return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM"))
        return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM"))
        return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};
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

AnanBackend::AnanBackend(QObject* parent) : IRadioBackend(parent)
{
    m_client = new P2Client(nullptr);
    //DH1KLM: Create the Protocol 1 transport client alongside the existing
    //DH1KLM: Protocol 2 client. Both clients are owned by this backend and
    //DH1KLM: are moved to the common ANAN I/O thread below.
    m_p1Client = new P1Client(nullptr);
    m_dsp = new AnanRxDsp(nullptr);

    m_tuneThrottleTimer = new QTimer(this);
    m_tuneThrottleTimer->setSingleShot(true);
    connect(m_tuneThrottleTimer, &QTimer::timeout, this, [this] {
        if (m_tunePendingApply) {
            m_tunePendingApply = false;
            applyTuneToRadioAndPan();
            m_tuneThrottleTimer->start(kTuneThrottleMs);
        }
    });

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("anan-io"));
    m_client->moveToThread(m_ioThread);
    //DH1KLM: P1 and P2 network processing share the same ANAN I/O thread.
    m_p1Client->moveToThread(m_ioThread);
    m_dsp->moveToThread(m_ioThread);
    m_ioThread->start();

    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    m_dspBuildThread->start();

    connect(m_client, &P2Client::ddc0IqReady, m_dsp,
            &AnanRxDsp::processIqBlock,
            Qt::DirectConnection);

    //DH1KLM: Protocol 1 delivers one IQ vector per receiver in a vector-of-
    //DH1KLM: vectors. Stage 1 drives one receiver, so feed receiver 0 directly
    //DH1KLM: into the same DSP entry point used by Protocol 2.
    connect(m_p1Client, &P1Client::iqBlocksReady, m_dsp,
            [this](const std::vector<std::vector<std::complex<float>>>& blocks) {
        if (!blocks.empty())
            m_dsp->processIqBlock(blocks.front());
    }, Qt::DirectConnection);

    connect(m_client, &P2Client::linkUp, this, [this] {
        m_connected = true;

        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;

        if (!wasRateChange)
            emit connected();

        emitSliceState();
        emitPanState();

        emit panBandwidthLimitsChanged(
            kPanId,
            48'000.0 / 1.0e6,
            1'536'000.0 / 1.0e6);

        if (wasRateChange) {
            const quint64 generation = m_connectGeneration;
            QTimer::singleShot(
                kRateChangeAudioSettleMs,
                this,
                [this, generation] {
                    if (generation != m_connectGeneration || !m_dsp)
                        return;

                    QMetaObject::invokeMethod(
                        m_dsp,
                        "setAudioMuted",
                        Qt::QueuedConnection,
                        Q_ARG(bool, false));
                });

            retryPendingRateChange();
        }
    });

    connect(m_client, &P2Client::linkDown, this, [this] {
        m_connected = false;
        if (!m_rateChanging)
            emit disconnected();
    });

    connect(m_client, &P2Client::connectionError, this,
            [this](const QString& reason) {
        emit connectionError(reason);
    });

    connect(m_client, &P2Client::dropsUpdated, this, [](quint64) {
        // No LinkStats wiring in this phase.
    });

    connect(m_client, &P2Client::discoveryInfoReceived, this,
            [this](quint8 boardId, quint8 firmwareVer, quint8 numDdc) {
        m_discoveredBoardId = boardId;
        m_discoveredFirmwareVer = firmwareVer;
        m_discoveredNumDdc = numDdc;
        m_discoveryInfoReceived = true;
        emit capabilitiesChanged();
    });

    //DH1KLM: Protocol 1 link state.
    connect(m_p1Client, &P1Client::linkUp,
            this,
            [this] {
        if (m_protocol != Protocol::P1)
            return;

        m_connected = true;

        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;

        if (!wasRateChange)
            emit connected();

        emitSliceState();
        emitPanState();

        // P1 supports only 48/96/192/384 kS/s. The limits emitted here are
        // therefore deliberately different from the P2 limits above.
        emit panBandwidthLimitsChanged(
            kPanId,
            48'000.0 / 1.0e6,
            384'000.0 / 1.0e6);

        if (wasRateChange) {
            const quint64 generation = m_connectGeneration;

            QTimer::singleShot(
                kRateChangeAudioSettleMs,
                this,
                [this, generation] {
                    if (generation != m_connectGeneration || !m_dsp)
                        return;

                    QMetaObject::invokeMethod(
                        m_dsp,
                        "setAudioMuted",
                        Qt::QueuedConnection,
                        Q_ARG(bool, false));
                });

            retryPendingRateChange();
        }
    });

    connect(m_p1Client, &P1Client::linkDown,
            this,
            [this] {
        if (m_protocol != Protocol::P1)
            return;

        m_connected = false;

        if (!m_rateChanging)
            emit disconnected();
    });

    connect(m_p1Client, &P1Client::connectFailed,
            this,
            [this](const QString& reason) {
        if (m_protocol != Protocol::P1)
            return;

        emit connectionError(reason);
    });

    connect(m_p1Client, &P1Client::dropsUpdated,
            this,
            [this](quint64 drops) {
        if (m_protocol != Protocol::P1)
            return;

        Q_UNUSED(drops);
        // No LinkStats wiring in this phase.
    });

    connect(m_dsp, &AnanRxDsp::audioReady,
            this,
            [this](const std::vector<float>& pcm) {
        const QByteArray bytes = floatBytes(pcm);
        emit sliceAudioFrameReady(kSliceId, bytes);
        emit audioFrameReady(bytes);
    });

    connect(m_dsp, &AnanRxDsp::spectrumReady,
            this,
            [this](const std::vector<float>& binsDbfs) {
        std::vector<float> dbm(binsDbfs.size());

        for (std::size_t i = 0; i < binsDbfs.size(); ++i)
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset;

        emit spectrumFrameReady(kSliceId, floatBytes(dbm));
    });
}

AnanBackend::~AnanBackend()
{
    ++m_connectGeneration;

    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }

    delete m_dspBuildContext;

    if (m_ioThread) {
        if (m_protocol == Protocol::P1) {
            if (m_p1Client)
                QMetaObject::invokeMethod(
                    m_p1Client,
                    "stop",
                    Qt::BlockingQueuedConnection);
        } else {
            if (m_client)
                QMetaObject::invokeMethod(
                    m_client,
                    "stop",
                    Qt::BlockingQueuedConnection);
        }

        m_ioThread->quit();
        m_ioThread->wait();
    }

    delete m_dsp;
    delete m_p1Client;
    delete m_client;
}

RadioCapabilities AnanBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("anan");
    c.hasAgcThreshold = true;
    c.manufacturer = QStringLiteral("Apache Labs");
    c.model = QStringLiteral("ANAN-G2");
    c.maxSlices = 1;
    c.maxPanadapters = 1;
    c.sampleRatesHz = {
        48000,
        96000,
        192000,
        384000,
        768000,
        1536000
    };

    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;
    c.takesTxAudioOverSeam = true;
    c.hasRadioPttReadback = false;
    c.hasTuner = false;
    c.hasTunerMemories = false;
    c.hasAmplifier = false;
    c.hasRadioSideDsp = false;
    c.hasAudioPeakingFilter = false;
    c.radioOwnsDbmScale = false;
    c.hasDdcPanEdgeRolloff = true;
    c.persistsMemories = false;
    c.clientSettingsDomains = {};
    c.extensionNamespaces = {};

    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("numDdc")] = m_discoveredNumDdc;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        c.extensions[QStringLiteral("anan")] = anan;
    }

    return c;
}

void AnanBackend::connectRadio(const RadioConnectRequest& request)
{
    const QHostAddress hostAddr(request.host);

    if (hostAddr.isNull()) {
        emit connectionError(
            QStringLiteral("ANAN: invalid host '%1'").arg(request.host));
        return;
    }

    if (m_connected)
        disconnectRadio();

    //DH1KLM: Select the transport explicitly from the connection request.
    // Stage 1 uses:
    //   1 = OpenHPSDR Protocol 1
    //   2 = OpenHPSDR Protocol 2
    // Missing or unknown values retain the existing P2 default.
    const int requestedProtocol =
        request.params.value(
            QStringLiteral("anan.protocol")).toInt();

    m_protocol = (requestedProtocol == 1)
        ? Protocol::P1
        : Protocol::P2;

    m_discoveryInfoReceived = false;
    m_discoveredBoardId = 0;
    m_discoveredFirmwareVer = 0;
    m_discoveredNumDdc = 0;

    m_pendingParams.host = request.host;

    int requestedRateKsps =
        request.params.value(
            QStringLiteral("anan.ddc0RateKsps")).toInt();

    if (requestedRateKsps <= 0)
        requestedRateKsps = 192;

    //DH1KLM: Protocol 1 has only 48/96/192/384 kS/s.
    //DH1KLM: Protocol 2 retains the existing six-rate range.
    if (m_protocol == Protocol::P1)
        requestedRateKsps = nearestP1RateKsps(requestedRateKsps);
    else
        requestedRateKsps = nearestDdc0RateKsps(requestedRateKsps);

    m_pendingParams.ddc0RateKsps = requestedRateKsps;

    m_pendingParams.ditherEnabled =
        request.params.value(
            QStringLiteral("anan.ditherEnabled"), true).toBool();

    m_pendingParams.randomEnabled =
        request.params.value(
            QStringLiteral("anan.randomEnabled"), true).toBool();

    m_pendingParams.ddc0AdcIndex =
        request.params.value(
            QStringLiteral("anan.ddc0AdcIndex"), 0).toInt() == 1
            ? 1
            : 0;

    m_pendingParams.bypassAdc0Filters =
        request.params.value(
            QStringLiteral("anan.bypassAdc0Filters"), true).toBool();

    m_pendingParams.bypassAdc1Filters =
        request.params.value(
            QStringLiteral("anan.bypassAdc1Filters"), true).toBool();

    m_sliceFreqHz =
        request.params.contains(QStringLiteral("anan.rxFrequencyHz"))
        ? request.params.value(
              QStringLiteral("anan.rxFrequencyHz")).toDouble()
        : 10'000'000.0;

    //DH1KLM: Prepare the Protocol 1 parameter set from the same common
    //DH1KLM: connection state. P1Client expects samples/sec rather than kS/s.
    m_pendingP1Params.host = hostAddr;
    m_pendingP1Params.sampleRate =
        m_pendingParams.ddc0RateKsps * 1000;
    m_pendingP1Params.receiverCount = 1;
    m_pendingP1Params.boardMaxRx = 7;
    m_pendingP1Params.rxFrequencyHz =
        static_cast<std::uint32_t>(
            std::llround(m_sliceFreqHz));

    m_pendingDspConfig = AnanRxDsp::Config{};
    m_pendingDspConfig.inputSampleRateHz =
        m_pendingParams.ddc0RateKsps * 1000;
    m_pendingDspConfig.audioSampleRateHz = 24000;
    m_pendingDspConfig.dspBlockSize = 1024;
    m_pendingDspConfig.fftSize = 1024;
    m_pendingDspConfig.mode = modeFromString(m_mode);
    m_pendingDspConfig.filterLowHz =
        static_cast<double>(m_filterLowHz) + cwBfoHz();
    m_pendingDspConfig.filterHighHz =
        static_cast<double>(m_filterHighHz) + cwBfoHz();
    m_pendingDspConfig.agcMode = 3;
    m_pendingDspConfig.maximumAgcGainDb = 60.0;

    ++m_connectGeneration;
    beginDspSetup();
}

void AnanBackend::beginDspSetup()
{
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    QMetaObject::invokeMethod(
        m_dsp,
        [this, generation, cfg] {
        m_dsp->beginInitialBuild(cfg);

        QMetaObject::invokeMethod(
            m_dspBuildContext,
            [this, generation, cfg] {
            AnanRxDsp::RebuildResult result =
                AnanRxDsp::buildChannel(cfg);

            const bool ok = result.channel != nullptr;

            const QString errStr =
                ok
                ? QString()
                : QStringLiteral(
                      "ANAN: DSP configure failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(
                m_dsp,
                [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(
                this,
                [this, generation, ok, errStr] {
                finishDspSetup(generation, ok, errStr);
            }, Qt::QueuedConnection);

        }, Qt::QueuedConnection);

    }, Qt::QueuedConnection);
}

void AnanBackend::finishDspSetup(
    quint64 generation,
    bool ok,
    const QString& error)
{
    if (generation != m_connectGeneration)
        return;

    if (!ok) {
        emit connectionError(
            QStringLiteral("ANAN: DSP configure failed: %1")
                .arg(error.isEmpty()
                         ? QStringLiteral("unknown error")
                         : error));

        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        emitPanState();

        if (wasRateChange)
            retryPendingRateChange();

        return;
    }

    //DH1KLM: Start the transport selected by discovery/connection params.
    if (m_protocol == Protocol::P1)
        startP1ClientSession(generation);
    else
        startP2ClientSession(generation);
}

void AnanBackend::startP1ClientSession(quint64 generation)
{
    if (generation != m_connectGeneration)
        return;

    m_pendingP1Params.sampleRate =
        m_pendingParams.ddc0RateKsps * 1000;

    m_pendingP1Params.rxFrequencyHz =
        static_cast<std::uint32_t>(
            std::llround(m_sliceFreqHz));

    const P1Client::Params params = m_pendingP1Params;

    bool started = false;

    QMetaObject::invokeMethod(
        m_p1Client,
        [this, params, &started] {
        started = m_p1Client->start(params);
    }, Qt::BlockingQueuedConnection);

    if (!started && generation == m_connectGeneration) {
        emit connectionError(
            QStringLiteral(
                "ANAN: failed to start OpenHPSDR Protocol 1 client"));
    }
}

void AnanBackend::startP2ClientSession(quint64 generation)
{
    const P2Client::Params params = m_pendingParams;
    const bool isRateChange = m_rateChanging;

    QMetaObject::invokeMethod(
        m_client,
        [this, generation, params, isRateChange] {
        const bool started = isRateChange
            ? m_client->start(params, kRateChangeConnectTimeoutMs)
            : m_client->start(params);

        if (!started) {
            QMetaObject::invokeMethod(
                this,
                [this, generation] {
                if (generation != m_connectGeneration)
                    return;

                emit connectionError(
                    QStringLiteral(
                        "ANAN: could not open the UDP socket"));

                if (m_pendingBandwidthKsps != 0) {
                    retryPendingRateChange();
                } else {
                    m_rateChanging = false;
                    emitPanState();
                }

            }, Qt::QueuedConnection);

            return;
        }

        if (m_sliceFreqHz > 0.0)
            m_client->setDdc0FrequencyHz(m_sliceFreqHz);

    }, Qt::QueuedConnection);
}

void AnanBackend::disconnectRadio()
{
    ++m_connectGeneration;

    //DH1KLM: Stop whichever transport is actually active.
    if (m_protocol == Protocol::P1) {
        if (m_p1Client) {
            QMetaObject::invokeMethod(
                m_p1Client,
                "stop",
                Qt::BlockingQueuedConnection);
        }
    } else {
        if (m_client) {
            QMetaObject::invokeMethod(
                m_client,
                "stop",
                Qt::BlockingQueuedConnection);
        }
    }

    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    m_pendingBandwidthKsps = 0;
    m_rateChanging = false;

    if (m_dsp) {
        QMetaObject::invokeMethod(
            m_dsp,
            "setAudioMuted",
            Qt::QueuedConnection,
            Q_ARG(bool, false));
    }
}

void AnanBackend::pushModeFilterShift()
{
    if (!m_dsp)
        return;

    const double bfo = cwBfoHz();

    QMetaObject::invokeMethod(
        m_dsp,
        "setMode",
        Qt::QueuedConnection,
        Q_ARG(WdspChannel::Mode,
              modeFromString(m_mode)));

    QMetaObject::invokeMethod(
        m_dsp,
        "setFilter",
        Qt::QueuedConnection,
        Q_ARG(double,
              static_cast<double>(m_filterLowHz) + bfo),
        Q_ARG(double,
              static_cast<double>(m_filterHighHz) + bfo));

    QMetaObject::invokeMethod(
        m_dsp,
        "setShift",
        Qt::QueuedConnection,
        Q_ARG(double, -bfo));
}

void AnanBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);

    m_sliceFreqHz = hz;

    //DH1KLM: Keep the pending P1 session parameters synchronized with the
    //DH1KLM: current slice frequency so a subsequent P1 restart uses it.
    if (m_protocol == Protocol::P1) {
        m_pendingP1Params.rxFrequencyHz =
            static_cast<std::uint32_t>(
                std::llround(hz));
    }

    emitSliceState();
    scheduleTuneApply();
}

void AnanBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);

    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] =
            defaultPassbandForMode(mode);

        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }

    m_mode = mode;
    pushModeFilterShift();
    emitSliceState();
}

void AnanBackend::setSliceFilter(
    int sliceId,
    int lowHz,
    int highHz)
{
    Q_UNUSED(sliceId);

    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;

    if (m_dsp) {
        const double bfo = cwBfoHz();

        QMetaObject::invokeMethod(
            m_dsp,
            "setFilter",
            Qt::QueuedConnection,
            Q_ARG(double,
                  static_cast<double>(lowHz) + bfo),
            Q_ARG(double,
                  static_cast<double>(highHz) + bfo));
    }

    emitSliceState();
}

void AnanBackend::setSliceAgc(
    int sliceId,
    const QString& mode,
    int thresholdDb)
{
    Q_UNUSED(sliceId);

    const QString m = mode.trimmed().toLower();

    int wdspMode = 3;

    if (m == QLatin1String("off"))
        wdspMode = 0;
    else if (m == QLatin1String("slow"))
        wdspMode = 2;
    else if (m == QLatin1String("fast"))
        wdspMode = 4;

    const double ceilingDb =
        static_cast<double>(thresholdDb) * 0.6;

    m_agcMode = wdspMode;
    m_agcCeilingDb = ceilingDb;

    if (m_dsp) {
        QMetaObject::invokeMethod(
            m_dsp,
            "setAgc",
            Qt::QueuedConnection,
            Q_ARG(int, wdspMode),
            Q_ARG(double, ceilingDb));
    }

    emitSliceState();
}

void AnanBackend::setPanCenter(
    const QString& panId,
    double hz,
    PanCenterIntent intent)
{
    Q_UNUSED(panId);
    Q_UNUSED(intent);

    setSliceFrequency(kSliceId, hz);
}

int AnanBackend::nearestDdc0RateKsps(
    int requestedKsps) noexcept
{
    static constexpr std::array<int, 6> kRatesKsps = {
        48, 96, 192, 384, 768, 1536
    };

    if (requestedKsps <= 0)
        return kRatesKsps.front();

    int best = kRatesKsps.front();
    double bestDistance =
        std::numeric_limits<double>::infinity();

    for (const int r : kRatesKsps) {
        const double distance =
            std::abs(
                std::log(
                    static_cast<double>(requestedKsps) / r));

        if (distance < bestDistance) {
            bestDistance = distance;
            best = r;
        }
    }

    return best;
}

//DH1KLM: Protocol 1 supports only the four legacy OpenHPSDR sample rates.
int AnanBackend::nearestP1RateKsps(int requestedKsps) noexcept
{
    constexpr std::array<int, 4> kP1Rates{48, 96, 192, 384};

    int best = kP1Rates.front();
    int bestDistance = std::abs(requestedKsps - best);

    for (const int rate : kP1Rates) {
        const int distance = std::abs(requestedKsps - rate);
        if (distance < bestDistance) {
            best = rate;
            bestDistance = distance;
        }
    }

    return best;
}

void AnanBackend::setPanBandwidth(
    const QString& panId,
    double hz)
{
    Q_UNUSED(panId);

    if (hz <= 0.0 || !m_connected)
        return;

    const int requestedKsps =
        static_cast<int>(hz / 1000.0 + 0.5);

    //DH1KLM: Select a protocol-specific legal sample rate.
    const int snappedKsps =
        (m_protocol == Protocol::P1)
            ? nearestP1RateKsps(requestedKsps)
            : nearestDdc0RateKsps(requestedKsps);

    if (snappedKsps == m_pendingParams.ddc0RateKsps) {
        m_pendingBandwidthKsps = 0;
        emitPanState();
        return;
    }

    if (!m_rateChanging) {
        beginRateChange(snappedKsps);
    } else {
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

void AnanBackend::beginRateChange(
    int newRateKsps)
{
    //DH1KLM: P1's sampleRate is expressed in samples/sec, while the common
    //DH1KLM: backend state uses kS/s.
    m_pendingParams.ddc0RateKsps =
        newRateKsps;

    if (m_protocol == Protocol::P1) {
        m_pendingP1Params.sampleRate =
            newRateKsps * 1000;
    }

    m_pendingDspConfig.inputSampleRateHz =
        newRateKsps * 1000;

    m_pendingDspConfig.mode =
        modeFromString(m_mode);

    m_pendingDspConfig.filterLowHz =
        static_cast<double>(m_filterLowHz) + cwBfoHz();

    m_pendingDspConfig.filterHighHz =
        static_cast<double>(m_filterHighHz) + cwBfoHz();

    m_pendingDspConfig.agcMode =
        m_agcMode;

    m_pendingDspConfig.maximumAgcGainDb =
        m_agcCeilingDb;

    m_rateChanging = true;

    ++m_connectGeneration;

    const quint64 generation =
        m_connectGeneration;

    const AnanRxDsp::Config cfg =
        m_pendingDspConfig;

    QMetaObject::invokeMethod(
        m_dsp,
        [this, generation, cfg] {
        m_dsp->beginRebuild();

        QMetaObject::invokeMethod(
            m_dspBuildContext,
            [this, generation, cfg] {
            AnanRxDsp::RebuildResult result =
                AnanRxDsp::buildChannel(cfg);

            const bool ok =
                result.channel != nullptr;

            const QString errStr =
                ok
                ? QString()
                : QStringLiteral(
                      "ANAN: DSP rebuild failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(
                m_dsp,
                [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(
                    std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(
                this,
                [this, generation, ok, errStr] {
                finishRateChange(
                    generation,
                    ok,
                    errStr);
            }, Qt::QueuedConnection);

        }, Qt::QueuedConnection);

    }, Qt::QueuedConnection);
}

void AnanBackend::finishRateChange(
    quint64 generation,
    bool ok,
    const QString& error)
{
    if (generation != m_connectGeneration)
        return;

    if (!ok) {
        emit connectionError(error);

        m_rateChanging = false;
        emitPanState();
        retryPendingRateChange();
        return;
    }

    // Mute before touching the live transport.
    QMetaObject::invokeMethod(
        m_dsp,
        "setAudioMuted",
        Qt::QueuedConnection,
        Q_ARG(bool, true));

    //DH1KLM: Stop the transport selected for this connection.
    if (m_protocol == Protocol::P1) {
        QMetaObject::invokeMethod(
            m_p1Client,
            "stop",
            Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(
            m_client,
            "stop",
            Qt::QueuedConnection);
    }

    QTimer::singleShot(
        kRateChangeRestartSettleMs,
        this,
        [this, generation] {
        if (generation != m_connectGeneration)
            return;

        //DH1KLM: Restart the same protocol after the rate change.
        if (m_protocol == Protocol::P1)
            startP1ClientSession(generation);
        else
            startP2ClientSession(generation);
    });
}

void AnanBackend::setPanFrameRate(
    const QString& panId,
    int fps)
{
    Q_UNUSED(panId);

    if (m_dsp)
        QMetaObject::invokeMethod(
            m_dsp,
            "setSpectrumRateFps",
            Qt::QueuedConnection,
            Q_ARG(int, fps));
}

void AnanBackend::setCwPitch(int hz)
{
    m_cwPitchHz = hz;
    pushModeFilterShift();
    emitSliceState();
}

void AnanBackend::setKeying(bool key)
{
    if (key)
        qWarning(
            "AnanBackend::setKeying(true) called on a "
            "receive-only backend (canTransmit=false) -- "
            "the engine TX guard should have refused this");
}

void AnanBackend::invokeExtension(
    const QString& ns,
    const QString& verb,
    quint64 requestId,
    const QVariant& arg)
{
    Q_UNUSED(ns);
    Q_UNUSED(verb);
    Q_UNUSED(arg);

    if (requestId != 0)
        emit extensionError(
            requestId,
            QStringLiteral(
                "ANAN: no extension namespaces implemented"));
}

void AnanBackend::emitSliceState()
{
    SliceDelta d;

    d.frequency =
        m_sliceFreqHz / 1.0e6;

    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.active = true;
    d.panId = kPanId;

    emit sliceChanged(
        kSliceId,
        d);
}

void AnanBackend::emitPanState()
{
    const double sampleRateHz =
        static_cast<double>(
            m_pendingDspConfig.inputSampleRateHz > 0
                ? m_pendingDspConfig.inputSampleRateHz
                : 48000);

    emit panCenterBandwidthChanged(
        kPanId,
        m_sliceFreqHz / 1.0e6,
        sampleRateHz / 1.0e6);
}

void AnanBackend::scheduleTuneApply()
{
    if (!m_tuneThrottleTimer->isActive()) {
        applyTuneToRadioAndPan();
        m_tuneThrottleTimer->start(
            kTuneThrottleMs);
    } else {
        m_tunePendingApply = true;
    }
}

void AnanBackend::applyTuneToRadioAndPan()
{
    if (m_connected) {
        const double hz =
            m_sliceFreqHz;

        if (m_protocol == Protocol::P1) {
            const auto frequency =
                static_cast<std::uint32_t>(
                    std::llround(hz));

            QMetaObject::invokeMethod(
                m_p1Client,
                [this, frequency] {
                m_p1Client->setRxFrequencyHz(
                    frequency);
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(
                m_client,
                [this, hz] {
                m_client->setDdc0FrequencyHz(hz);
            }, Qt::QueuedConnection);
        }
    }

    emitPanState();
}

}  // namespace AetherSDR::anan