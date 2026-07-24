#include "core/backends/hl2/Hl2Backend.h"

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QByteArray>
#include <QHostAddress>

#include <cstdint>

namespace AetherSDR::hl2 {

namespace {

SampleRate sampleRateEnum(int hz) noexcept
{
    switch (hz) {
    case 96000:  return SampleRate::R96k;
    case 192000: return SampleRate::R192k;
    case 384000: return SampleRate::R384k;
    default:     return SampleRate::R48k;
    }
}

WdspChannel::Mode modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    if (u == QLatin1String("CWL"))  return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("CWU"))  return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("FM"))   return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;
}

// Phase-1 data-plane payload: a raw little-endian float32 array. RadioModel's
// relay decodes it; the binary step-4 frame format supersedes this later.
QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

Hl2Backend::Hl2Backend(QObject* parent) : IRadioBackend(parent)
{
    m_metis = new MetisClient(this);
    m_dsp = new Hl2RxDsp(this);

    // Wire: raw IQ -> DSP (direct call on this thread, Phase 1b).
    connect(m_metis, &MetisClient::iqBlockReady, m_dsp, &Hl2RxDsp::processIqBlock);

    // Link lifecycle: first EP6 -> connected; stop -> disconnected.
    connect(m_metis, &MetisClient::linkUp, this, [this] {
        m_connected = true;
        emit connected();
        // Publish initial slice/pan state AFTER connected(), not in connectRadio():
        // RadioModel::onConnected() stages every existing model as "previous
        // session" leftovers, so anything emitted earlier is wiped before the UI
        // ever sees it (slice panel stuck empty / 0.000000).
        emitSliceState();
        emitPanState();
    });
    connect(m_metis, &MetisClient::linkDown, this, [this] {
        if (m_connected) {
            m_connected = false;
            emit disconnected();
        }
    });

    // DSP outputs -> seam data plane + S-meter.
    connect(m_dsp, &Hl2RxDsp::spectrumReady, this,
            [this](const std::vector<float>& bins) { emit spectrumFrameReady(0, floatBytes(bins)); });
    connect(m_dsp, &Hl2RxDsp::audioReady, this,
            [this](const std::vector<float>& pcm) { emit audioFrameReady(floatBytes(pcm)); });
    connect(m_dsp, &Hl2RxDsp::meterUpdate, this,
            [this](float dbfs) { emit meterUpdate(QStringLiteral("s-meter"), dbfs); });
}

Hl2Backend::~Hl2Backend()
{
    if (m_metis)
        m_metis->stop();
}

RadioCapabilities Hl2Backend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("hl2");
    c.model = QStringLiteral("Hermes-Lite 2");
    c.maxSlices = 1;
    c.maxPanadapters = 1;
    c.sampleRatesHz = {48000, 96000, 192000, 384000};
    c.canTransmit = false;              // RX-only: the engine TX guard denies keying
    c.txPowerMaxWatts = 0.0;
    c.hasTuner = false;
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
    // No extension namespaces (no invokeExtension verbs yet), matching FlexBackend.
    return c;
}

void Hl2Backend::connectRadio(const RadioConnectRequest& request)
{
    const QHostAddress host(request.host);
    if (host.isNull()) {
        emit connectionError(QStringLiteral("HL2: invalid host '%1'").arg(request.host));
        return;
    }

    // Optional overrides from the namespaced params.
    if (request.params.contains(QStringLiteral("sampleRateHz")))
        m_sampleRateHz = request.params.value(QStringLiteral("sampleRateHz")).toInt();
    if (request.params.contains(QStringLiteral("lnaGainDb")))
        m_lnaGainDb = request.params.value(QStringLiteral("lnaGainDb")).toInt();
    if (request.params.contains(QStringLiteral("rxFrequencyHz")))
        m_rxFreqHz = request.params.value(QStringLiteral("rxFrequencyHz")).toDouble();

    Hl2RxDsp::Config dc;
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 48000;
    dc.mode = modeFromString(m_mode);
    dc.filterLowHz = m_filterLowHz;
    dc.filterHighHz = m_filterHighHz;
    std::string err;
    if (!m_dsp->configure(dc, &err)) {
        emit connectionError(QStringLiteral("HL2 DSP: %1").arg(QString::fromStdString(err)));
        return;
    }

    MetisClient::Params mp;
    mp.host = host;
    mp.port = request.port ? request.port : kMetisPort;
    mp.sampleRate = sampleRateEnum(m_sampleRateHz);
    mp.rxFrequencyHz = static_cast<std::uint32_t>(m_rxFreqHz < 0 ? 0 : m_rxFreqHz);
    mp.lnaGainDb = m_lnaGainDb;
    if (!m_metis->start(mp)) {
        emit connectionError(QStringLiteral("HL2: could not open the UDP socket"));
        return;
    }
    // Initial slice/pan state is published from the linkUp handler above, once
    // connected() has fired and RadioModel has finished staging the old session.
}

void Hl2Backend::disconnectRadio()
{
    if (m_metis)
        m_metis->stop();   // emits linkDown -> disconnected() when it was up
}

bool Hl2Backend::isConnected() const
{
    return m_connected;
}

void Hl2Backend::setSliceFrequency(int /*sliceId*/, double hz)
{
    m_rxFreqHz = hz;
    if (m_metis)
        m_metis->setRxFrequencyHz(static_cast<std::uint32_t>(hz < 0 ? 0 : hz));
    emitSliceState();
    emitPanState();
}

void Hl2Backend::setSliceMode(int /*sliceId*/, const QString& mode)
{
    m_mode = mode;
    if (m_dsp)
        m_dsp->setMode(modeFromString(mode));
    emitSliceState();
}

void Hl2Backend::setSliceFilter(int /*sliceId*/, int lowHz, int highHz)
{
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_dsp)
        m_dsp->setFilter(lowHz, highHz);
    emitSliceState();
}

void Hl2Backend::setKeying(bool /*key*/)
{
    // RX-only. capabilities().canTransmit is false, so the engine guard already
    // denies keying above the seam; this is a defensive no-op.
}

void Hl2Backend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/, quint64 requestId,
                                 const QVariant& /*arg*/)
{
    // No HL2 extension verbs yet; honor the async contract without hanging.
    if (requestId != 0)
        emit extensionError(requestId, QStringLiteral("hl2: no extension verbs implemented"));
}

void Hl2Backend::emitSliceState()
{
    SliceDelta d;
    d.panId = QString::fromLatin1(kPanId);
    d.frequency = m_rxFreqHz / 1.0e6;   // MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    emit sliceChanged(kSliceId, d);
}

void Hl2Backend::emitPanState()
{
    emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), m_rxFreqHz / 1.0e6,
                                   static_cast<double>(m_sampleRateHz) / 1.0e6);
}

}  // namespace AetherSDR::hl2
