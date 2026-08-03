#include "core/backends/icom/IcomCivBackend.h"

#include <QDateTime>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>

#include "core/Resampler.h"

namespace AetherSDR::icom {
namespace {

// Metering is examined this often; the MeterPoller decides what is actually
// due. Deliberately faster than the fastest meter interval so a due meter is
// not delayed by up to a whole tick.
constexpr int kMeterTickMs = 40;
// Transport counters publish on a FIXED cadence, not on receive: "nothing
// arrived this second" is the observation the heartbeat's alarm path waits for,
// and a backend that emits only on receive can never report its own silence.
constexpr int kLinkTickMs = 1000;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

// AetherSDR's slider is 0..100; the radio's register is 0..255.
int percentToRaw(int percent) { return std::clamp(percent, 0, 100) * 255 / 100; }

// Default RX passband per mode, in Hz relative to the carrier. Sign carries the
// sideband, matching SliceModel's convention.
//
// THE BACKEND MUST SUPPLY THIS. radiocert's passband-after-mode-change stage
// found it missing on the first run: CW -> DIGU left the window at -1500..1500,
// so a decoder in a wide mode saw a narrow slot. A radio that owns its own DSP
// sends no passband echo to heal that, and the IC-705's three fixed IF filters
// cannot be read back as Hz — so nothing else in the chain can fill it in.
//
// These are the radio's own defaults for each mode, not arbitrary picks.
std::pair<int, int> defaultPassbandFor(const QString& mode)
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return {-2700, -300};
    if (u == QLatin1String("USB"))  return {300, 2700};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("RTTY")) return {-3000, -150};
    // CW is symmetric about the pitch; the radio centres its filter on the tone.
    if (u == QLatin1String("CW") || u == QLatin1String("CWU")
        || u == QLatin1String("CWL"))
        return {-250, 250};
    if (u == QLatin1String("AM"))   return {-4500, 4500};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-7000, 7000};
    if (u == QLatin1String("WFM"))  return {-100000, 100000};
    return {-1500, 1500};
}

}  // namespace

IcomCivBackend::IcomCivBackend(QObject* parent)
    : IRadioBackend(parent), m_model(&unknownModel())
{
}

IcomCivBackend::~IcomCivBackend() = default;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

RadioCapabilities IcomCivBackend::capabilities() const
{
    const IcomModel& m = *m_model;
    RadioCapabilities c;
    c.family = QStringLiteral("icom");
    c.model  = m_deviceName.isEmpty() ? QString::fromUtf8(m.name.data(),
                                                          static_cast<int>(m.name.size()))
                                      : m_deviceName;

    c.maxSlices = m.receivers;
    c.maxPanadapters = m.hasScope ? m.receivers : 0;
    c.tuningMinHz = static_cast<double>(m.tuningMinHz);
    c.tuningMaxHz = static_cast<double>(m.tuningMaxHz);

    c.canTransmit = m.hasTransmit;
    c.txPowerMaxWatts = m.txPowerMaxWatts;

    // The RADIO modulates. Contrast the HL2, where the host does — this drives
    // the mic-source list and the PC-audio lock, so getting it wrong opens the
    // host microphone on a radio that will never use it.
    c.hostModulates = false;

    // NR / NB / notch are 0x16 commands executed in the radio's own firmware.
    c.hasRadioSideDsp = true;

    // NO IQ, on any networked Icom. Not deferred — absent. See icom-oracle §8.1.
    c.hasDaxStreams = false;

    // The radio HAS a GPS and the protocol will not carry its data.
    c.hasGpsLocation = false;

    c.hasSupplyVoltageTelemetry = true;   // 0x15 0x15 Vd

    // No internal ATU on the IC-705. `1C 01` drives an EXTERNAL AH-705 and
    // there is no command to detect whether one is attached, so the capability
    // is unanswerable from the radio; false is the safer default.
    c.hasTuner = false;

    // The radio chooses its own modulation input from its own menu (MOD Input
    // > DATA MOD, which must be WLAN for us to be heard at all). A client
    // cannot pick MIC / BAL / LINE / ACC, so the Phone applet collapses to PC.
    c.hasSelectableMicInputs = false;

    c.hasProfiles = false;
    c.hasWaveforms = false;
    c.hasMultiClientSessions = false;
    c.hasRadioSideWaterfallAutoBlack = false;
    c.persistsMemories = false;

    // A one-way trip over WiFi: 0x18 0x00 powers the radio off, which drops the
    // WLAN interface, so the 0x18 0x01 that would bring it back has no path.
    c.canReboot = false;

    // EMPTY, and load-bearing. An Icom remembers its own frequency, mode and
    // filter across power cycles and reports them on request, so Constitution
    // II/III says the client must not re-assert them. This backend READS state
    // at connect; it never pushes a restored one.
    c.clientSettingsDomains = {};

    return c;
}

void IcomCivBackend::publishCapabilities() { emit capabilitiesChanged(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void IcomCivBackend::connectRadio(const RadioConnectRequest& request)
{
    disconnectRadio();

    IcomSession::Params p;
    p.host = QHostAddress(request.host);
    p.controlPort = request.port ? request.port : kControlPort;
    p.serialPort  = static_cast<quint16>(
        request.params.value(QStringLiteral("icom.serialPort"), kSerialPort).toUInt());
    p.audioPort   = static_cast<quint16>(
        request.params.value(QStringLiteral("icom.audioPort"), kAudioPort).toUInt());
    p.username = request.params.value(QStringLiteral("icom.username")).toString();
    p.password = request.params.value(QStringLiteral("icom.password")).toString();
    p.civAddress = static_cast<std::uint8_t>(
        request.params.value(QStringLiteral("icom.civAddress"), 0xA4).toUInt());
    p.sampleRateHz = kRadioAudioRateHz;

    m_session = std::make_unique<IcomSession>();
    connect(m_session.get(), &IcomSession::connected, this, &IcomCivBackend::onSessionConnected);
    connect(m_session.get(), &IcomSession::disconnected, this,
            &IcomCivBackend::onSessionDisconnected);
    connect(m_session.get(), &IcomSession::civFrameReady, this, &IcomCivBackend::onCivFrame);
    connect(m_session.get(), &IcomSession::audioReady, this, &IcomCivBackend::onAudio);

    if (!m_session->start(p))
        emit connectionError(QStringLiteral("could not open the Icom session"));
}

void IcomCivBackend::disconnectRadio()
{
    for (QTimer** t : {&m_meterTimer, &m_linkTimer}) {
        if (*t) {
            (*t)->stop();
            (*t)->deleteLater();
            *t = nullptr;
        }
    }
    if (m_session) {
        m_session->stop();
        m_session.reset();
    }
    m_rxResampler.reset();
    m_scope.reset();
    m_meters.reset();
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
}

bool IcomCivBackend::isConnected() const { return m_connected; }

void IcomCivBackend::onSessionConnected(const QString& deviceName)
{
    m_deviceName = deviceName;
    m_connected = true;

    // RESOLVE THE MODEL FROM THE NAME, NOW.
    //
    // capabilities() answers from m_model, which starts as unknownModel() —
    // deliberately conservative: no scope, NO TRANSMIT. That default is right
    // for a radio we cannot characterise, and wrong the moment we can: the
    // 0x19 0x00 address query needs a serial stream that does not exist until
    // after this point, so anything reading capabilities on the connect edge
    // saw canTransmit=false and refused to key a radio that transmits fine.
    // radiocert's meters and tx phases both did exactly that.
    //
    // The capabilities packet already told us the name during the handshake, so
    // use it. The address query still runs and still wins — it is the
    // authority, this is just early enough to be useful.
    if (const IcomModel* byName = modelForName(deviceName.toStdString()))
        m_model = byName;

    // The radio's audio is 48 kHz mono; the seam's per-slice contract is 24 kHz
    // interleaved stereo. Built once here rather than per-buffer: r8brain is
    // stateful, and a fresh instance per callback restarts its filter history
    // every block, which is audible as a periodic tick.
    m_rxResampler = std::make_unique<Resampler>(
        static_cast<double>(kRadioAudioRateHz), static_cast<double>(kEngineAudioRateHz), 4096);

    // ASK the radio what it is. The CI-V address is user-changeable and several
    // models speak this same transport, so a hardcoded 0xA4 would silently
    // mis-decode an IC-9700 someone pointed this at.
    m_session->sendCiv(cmdReadId(m_session->civAddress()));
    m_session->sendCiv(cmdReadFrequency(m_session->civAddress()));
    m_session->sendCiv(cmdReadMode(m_session->civAddress()));

    applyScopeStartup();

    // CONNECTED FIRST, then the state.
    //
    // RadioModel stages the previous session's slices and CLEARS m_slices on
    // the connect edge (stagePreviousSessionModelsForReconnect). Publishing the
    // slice before connected() therefore created it and had it swept away in
    // the same breath — the model ended with no slice at all, which is why
    // click-to-tune reported "Slice capacity is full" (the spectrum could not
    // resolve a tune target, so it fell through to the create-a-slice path
    // against a one-slice radio) and why txSlice never took.
    emit connected();
    publishCapabilities();

    // THE PAN FIRST, then the slice that names it.
    //
    // RadioModel maps a backend pan id to a neutral index on FIRST SIGHT, and
    // the slice delta below carries that id. Announcing the slice first left it
    // pointing at a pan nothing had registered, so the slice belonged to no
    // pane — which is why click-to-tune reported "Slice capacity is full": the
    // spectrum could not resolve a tune target on a pan it thought was empty,
    // and fell through to the create-a-slice path against a one-slice radio.
    //
    // Provisional geometry: the first 0x27 sweep replaces it a few tens of ms
    // later. A placeholder that is replaced beats an association that never forms.
    emit panCenterBandwidthChanged(panId(), 0.0, 0.0);

    // One slice, and it exists from the moment we connect. Without it nothing
    // downstream has anything to attach audio to — including the TCI receiver
    // channel, which is routed by slice.
    SliceDelta s;
    s.panId = panId();
    s.inUse = true;
    s.active = true;
    s.txSlice = true;   // one receiver IS the transmitter
    emit sliceChanged(sliceId(), s);

    publishMeterDefs();

    // The RF-gain control is a THREE-POSITION preamp, not a dB register.
    // Advertising the real, discrete range is what makes the existing slider
    // snap to three detents instead of sweeping smoothly over a control that
    // cannot follow it.
    emit panRfGainInfoChanged(panId(), 0, 2, 1);

    // A small default set so the status bar is alive before any UI declares
    // what it is showing. setMeterVisible() narrows or widens this.
    m_meters.setVisible(MeterId::SMeter, true);
    m_meters.setVisible(MeterId::Vd, true);
    m_meters.setVisible(MeterId::Overflow, true);

    m_meterTimer = new QTimer(this);
    connect(m_meterTimer, &QTimer::timeout, this, &IcomCivBackend::onMeterTick);
    m_meterTimer->start(kMeterTickMs);

    m_linkTimer = new QTimer(this);
    connect(m_linkTimer, &QTimer::timeout, this, &IcomCivBackend::onLinkTick);
    m_linkTimer->start(kLinkTickMs);


}

void IcomCivBackend::onSessionDisconnected(const QString& reason)
{
    const bool was = m_connected;
    m_connected = false;
    if (was)
        emit disconnected();
    if (!reason.isEmpty())
        emit connectionError(reason);
}

void IcomCivBackend::applyScopeStartup()
{
    if (!m_session || !m_model->hasScope)
        return;
    // BOTH switches. Enabling only 0x27 0x10 turns the scope on the radio's own
    // screen and sends us nothing — the number-one "black panadapter" cause.
    m_session->sendCiv(cmdScopeOnOff(m_session->civAddress(), true));
    m_session->sendCiv(cmdScopeDataOutput(m_session->civAddress(), true));
}

// ---------------------------------------------------------------------------
// CI-V decode
// ---------------------------------------------------------------------------

void IcomCivBackend::onCivFrame(const CivFrame& frame)
{
    // Scope first: it is by far the highest-rate frame, and the decoder already
    // rejects anything that is not waveform data.
    if (auto sweep = m_scope.feed(frame)) {
        ScopeGeometry geom;
        geom.points = m_model->scopePoints ? m_model->scopePoints : kScopePointsIc705;
        geom.maxAmplitude = m_model->scopeMaxAmplitude ? m_model->scopeMaxAmplitude
                                                       : kScopeMaxAmplitude;
        emit panCenterBandwidthChanged(panId(),
                                       static_cast<double>(sweep->centreHz()) / 1e6,
                                       static_cast<double>(sweep->bandwidthHz()) / 1e6);
        emit spectrumFrameReady(0, floatBytes(toDbm(*sweep, geom, m_scopeCal)));
        return;
    }

    switch (frame.cmd) {
    case cmd::kReadId: {
        if (auto addr = parseModelIdReply(frame)) {
            if (const IcomModel* m = modelForCivAddress(*addr)) {
                m_model = m;
                // The span limits and scope geometry are model facts, so they
                // can only be published once the radio has named itself.
                const auto widths = availableBandwidthsHz();
                if (!widths.empty() && m_model->hasScope)
                    emit panBandwidthLimitsChanged(panId(), widths.front() / 1e6,
                                                   widths.back() / 1e6);
                publishMeterDefs();
                publishCapabilities();
            }
            RadioDelta r;
            r.model = QString::fromUtf8(m_model->name.data(),
                                        static_cast<int>(m_model->name.size()));
            emit radioChanged(r);
        }
        return;
    }

    case cmd::kReadFreq:
    case cmd::kSetFreqTrx: {
        // 0x00 is the TRANSCEIVE push the radio sends unprompted when the
        // operator turns the dial; 0x03 is the answer to our poll. Same payload,
        // and both are the truth — which is why they share a case.
        if (auto hz = decodeFreq(frame.data)) {
            m_frequencyHz = *hz;
            SliceDelta s;
            s.frequency = static_cast<double>(*hz) / 1e6;
            emit sliceChanged(sliceId(), s);
        }
        return;
    }

    case cmd::kReadMode:
    case cmd::kSetModeTrx: {
        if (frame.data.empty())
            return;
        m_mode = static_cast<CivMode>(frame.data[0]);
        const QString neutral = QString::fromStdString(modeToNeutral(m_mode, m_dataMode));
        if (neutral.isEmpty())
            return;   // D-STAR: a waveform, not a demodulator setting
        SliceDelta s;
        s.mode = neutral;
        // The passband travels WITH the mode, in the same delta, because the
        // radio will never send one. Applied after the mode by SliceModel's own
        // ordering, which is what stops a narrow CW window surviving into DIGU.
        const auto [low, high] = defaultPassbandFor(neutral);
        s.filterLow  = low;
        s.filterHigh = high;
        emit sliceChanged(sliceId(), s);
        return;
    }

    case cmd::kMeter: {
        if (!frame.hasSub)
            return;
        const MeterSpec* spec = meterSpecForSub(frame.sub);
        if (!spec)
            return;
        auto raw = decodeLevel(frame.data);
        if (!raw)
            return;

        m_meters.markAnswered(spec->id, QDateTime::currentMSecsSinceEpoch());
        const double value = meterValue(spec->id, *raw, s9ReferenceFor(m_frequencyHz));

        if (spec->id == MeterId::Overflow) {
            m_overflow = value > 0.5;
        } else if (spec->id == MeterId::Vd) {
            m_vdVolts = value;
        } else if (spec->id == MeterId::Id) {
            m_idAmps = value;
        }
        // "SOURCE:NAME", the id every consumer looks up by. Emitting the bare
        // name published a meter nothing could find: radiocert's inventory
        // reported SLC:LEVEL as never defined while the S-meter was decoding
        // correctly the whole time — the orphaned-meter-seam defect, again.
        emit meterUpdate(QStringLiteral("%1:%2")
                             .arg(QString::fromUtf8(spec->source.data(),
                                                    static_cast<int>(spec->source.size())),
                                  QString::fromUtf8(spec->name.data(),
                                                    static_cast<int>(spec->name.size()))),
                         value);
        return;
    }

    case cmd::kControl: {
        if (frame.hasSub && frame.sub == control::kPtt && !frame.data.empty()) {
            m_keyed = frame.data[0] != 0;
            m_meters.setTransmitting(m_keyed);
            TransmitDelta t;
            t.mox = m_keyed;
            emit transmitChanged(t);
        }
        return;
    }

    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// Audio — the path WSJT-X depends on
// ---------------------------------------------------------------------------

void IcomCivBackend::onAudio(const std::vector<float>& mono)
{
    if (mono.empty() || !m_rxResampler)
        return;

    // 48 kHz MONO from the radio -> 24 kHz interleaved STEREO for the engine.
    //
    // This one line is the whole TCI/WSJT-X path. The seam's per-slice contract
    // is interleaved stereo float32 at 24 kHz — Hl2RxDsp::audioReady names it
    // `stereoPcm` and TciServer constructs its resampler with a 24000 source
    // rate — and the radio hands us neither. Skipping the rate conversion plays
    // back an octave low; skipping the channel duplication feeds TciServer half
    // the frames it thinks it has, because it divides by 2*sizeof(float).
    const QByteArray stereo24k =
        m_rxResampler->processMonoToStereo(mono.data(), static_cast<int>(mono.size()));
    if (stereo24k.isEmpty())
        return;

    // The speaker feed.
    emit audioFrameReady(stereo24k);

    // And the PER-SLICE feed, which is a different consumer and not optional:
    // the TCI receiver channels are routed by slice, because a mixed feed
    // cannot say which slice a buffer belongs to. This is the signal that ends
    // up as TCI audio channel 1 for WSJT-X.
    //
    // Emitted PRE-mute and PRE-gain by contract — muting a slice must silence
    // the monitor without stopping a decoder that is running on it.
    emit sliceAudioFrameReady(sliceId(), stereo24k);
}

void IcomCivBackend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz)
{
    if (!m_session || !m_connected)
        return;
    // The engine hands us interleaved int16 stereo; the radio wants mono at its
    // negotiated rate. Downmix here rather than in IcomSession so the session
    // stays a transport.
    const int frames = static_cast<int>(int16Stereo.size() / (2 * sizeof(qint16)));
    if (frames <= 0)
        return;
    const auto* src = reinterpret_cast<const qint16*>(int16Stereo.constData());
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        mono[static_cast<std::size_t>(i)] =
            (src[i * 2] + src[i * 2 + 1]) * 0.5f / 32768.0f;

    // Rate mismatch is a silent corruption rather than a failure, so it is
    // reported once rather than resampled behind the operator's back.
    if (sampleRateHz != kRadioAudioRateHz) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            emit connectionError(QStringLiteral(
                "transmit audio arrived at %1 Hz but the radio negotiated %2 Hz")
                                     .arg(sampleRateHz)
                                     .arg(kRadioAudioRateHz));
        }
        return;
    }
    m_session->sendAudio(mono);
}

// ---------------------------------------------------------------------------
// Intents DOWN
// ---------------------------------------------------------------------------

void IcomCivBackend::sendUserCommand(const std::vector<std::uint8_t>& frame)
{
    if (!m_session || !m_connected)
        return;
    // Tell the scheduler a real command just went out, so metering yields and
    // the command is not stuck behind a queue of polls.
    m_meters.noteUserCommand(QDateTime::currentMSecsSinceEpoch());
    m_session->sendCiv(frame);
}

void IcomCivBackend::setSliceFrequency(int, double hz)
{
    if (hz <= 0.0)
        return;
    sendUserCommand(cmdSetFrequency(m_session ? m_session->civAddress() : 0xA4,
                                    static_cast<std::uint64_t>(std::llround(hz))));
}

void IcomCivBackend::setSliceMode(int, const QString& mode)
{
    bool data = false;
    auto civ = modeFromNeutral(mode.toStdString(), data);
    if (!civ) {
        // No IC-705 equivalent (SAM, DRM, DSB). Refusing beats substituting USB:
        // a slice that asked for SAM and silently got USB has a mode indicator
        // that lies about what is being demodulated.
        return;
    }
    m_dataMode = data;
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, *civ, 1));

    // PUBLISH THE PASSBAND NOW, from the mode we just commanded.
    //
    // Waiting for the radio to report the mode back is not good enough: the
    // report only arrives if CI-V Transceive is on, and even then it lands
    // milliseconds later. radiocert's passband-after-mode-change stage caught
    // exactly that — CW then DIGU left the window at the previous mode's width,
    // so a decoder in a wide mode saw a narrow slot. The radio owns its DSP and
    // sends no passband, so this is the only place it can come from.
    const auto [low, high] = defaultPassbandFor(mode);
    SliceDelta d;
    d.mode = mode.toUpper();
    d.filterLow  = low;
    d.filterHigh = high;
    emit sliceChanged(sliceId(), d);
}

void IcomCivBackend::setSliceFilter(int, int lowHz, int highHz)
{
    // The radio has three fixed IF filters, not a continuous passband, so this
    // can only SNAP. What the radio actually took comes back on its own mode
    // report — we must not echo the requested width as if it were applied.
    const int width = std::abs(highHz - lowHz);
    const int filter = filterForWidthHz(width);
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, m_mode, filter));
}

void IcomCivBackend::setSliceAgc(int, const QString& mode, int)
{
    // thresholdDb has NOWHERE to go: the radio offers FAST/MID/SLOW and no
    // threshold. A documented no-op beats inventing a mapping.
    const QString m = mode.toUpper();
    int value = 2;   // MID
    if (m == QLatin1String("FAST"))
        value = 1;
    else if (m == QLatin1String("SLOW"))
        value = 3;
    else if (m == QLatin1String("OFF"))
        value = 1;   // the radio has no AGC-off; FAST is the closest honest thing
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kAgc, value));
}

void IcomCivBackend::setPanCenter(const QString&, double hz)
{
    // In centre mode the scope follows the operating frequency, so moving the
    // pan centre IS retuning.
    setSliceFrequency(sliceId(), hz);
}

void IcomCivBackend::setPanBandwidth(const QString&, double hz)
{
    if (hz <= 0.0)
        return;
    // hz is a TOTAL width and Icom's span is a HALF-width, so the conversion is
    // not a rename. It also SNAPS to one of eight values — what was actually
    // taken comes back with the next sweep, via panCenterBandwidthChanged.
    const int span = spanForBandwidthHz(static_cast<int>(std::llround(hz)));
    sendUserCommand(cmdScopeSpan(m_session ? m_session->civAddress() : 0xA4, span));
}

void IcomCivBackend::setPanRfGain(const QString&, int gainDb)
{
    // There is NO continuous RF-gain register. The IC-705 has a three-position
    // preamp, so this snaps to it; panRfGainInfoChanged advertises (0, 2, 1) so
    // the slider stops where the hardware does instead of sweeping smoothly
    // over a control that has three detents.
    const int preamp = std::clamp(gainDb, 0, 2);
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kPreamp, preamp));
}

void IcomCivBackend::setKeying(bool key)
{
    if (!m_model->hasTransmit)
        return;   // an unknown radio is not advertised as transmit-capable
    sendUserCommand(cmdSetPtt(m_session ? m_session->civAddress() : 0xA4, key));
    m_keyed = key;
    m_meters.setTransmitting(key);
    if (!key && m_session)
        m_session->flushTxAudio();   // queued audio belongs to the transmission that ended
}

void IcomCivBackend::setTune(bool on, int tunePowerPercent)
{
    // THERE IS NO TUNE-CARRIER COMMAND. `1C 01` is the antenna tuner, which is
    // a different feature and may not even be attached. A steady tune carrier
    // is COMPOSED: set the drive, then key. The mode save/restore that a full
    // implementation needs is deliberately absent here rather than half-done —
    // see the design note.
    if (on && tunePowerPercent >= 0)
        setTxPower(tunePowerPercent);
    setKeying(on);
}

void IcomCivBackend::setTxPower(int percent)
{
    m_txPowerPercent = std::clamp(percent, 0, 100);
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kRfPower, percentToRaw(m_txPowerPercent)));
}

void IcomCivBackend::invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                                     const QVariant& arg)
{
    if (ns != QLatin1String("icom")) {
        emit extensionError(requestId, QStringLiteral("unknown namespace %1").arg(ns));
        return;
    }
    if (verb == QLatin1String("tuner.start")) {
        // The ATU cycle — explicitly NOT setTune(). Exposed as an extension so
        // an operator with an AH-705 can reach it without the TUNE button
        // running an ATU that may not be attached.
        sendUserCommand(buildFrameSub(m_session ? m_session->civAddress() : 0xA4,
                                      cmd::kControl, control::kTuner,
                                      std::array<std::uint8_t, 1>{0x02}));
        emit extensionResult(requestId, true);
        return;
    }
    if (verb == QLatin1String("scope.reference")) {
        sendUserCommand(cmdScopeReference(m_session ? m_session->civAddress() : 0xA4,
                                          arg.toDouble()));
        m_scopeCal.referenceDb = arg.toDouble();
        emit extensionResult(requestId, true);
        return;
    }
    emit extensionError(requestId, QStringLiteral("unknown verb %1").arg(verb));
}

// ---------------------------------------------------------------------------
// Metering and diagnostics
// ---------------------------------------------------------------------------

void IcomCivBackend::setMeterVisible(MeterId id, bool visible)
{
    m_meters.setVisible(id, visible);
}

void IcomCivBackend::publishMeterDefs()
{
    int index = 0;
    for (const MeterSpec& s : meterSpecs()) {
        MeterDef d;
        d.index = index++;
        d.source = QString::fromUtf8(s.source.data(), static_cast<int>(s.source.size()));
        d.name = QString::fromUtf8(s.name.data(), static_cast<int>(s.name.size()));
        d.unit = QString::fromUtf8(s.unit.data(), static_cast<int>(s.unit.size()));
        d.low = s.low;
        d.high = s.high;
        // The Po meter's high depends on the model's measured curve, and a
        // model we have no curve for must NOT claim watts — see powerCurveFor.
        if (s.id == MeterId::Power) {
            const auto curve = powerCurveFor(*m_model);
            if (curve.empty()) {
                d.unit = QStringLiteral("Percent");
                d.high = 100.0;
            } else {
                d.high = curve.back().value;
            }
        }
        emit meterDefined(d);
    }
}

void IcomCivBackend::onMeterTick()
{
    if (!m_session || !m_connected)
        return;
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    for (MeterId id : m_meters.due(now)) {
        const MeterSpec* spec = meterSpecFor(id);
        if (!spec)
            continue;
        // Deliberately NOT sendUserCommand(): a meter poll must not reset the
        // scheduler's own user-command guard, or metering would permanently
        // suppress itself.
        m_session->sendCiv(cmdReadMeter(m_session->civAddress(), spec->sub));
    }
}

void IcomCivBackend::onLinkTick()
{
    if (!m_session)
        return;
    const auto s = m_session->stats();

    LinkStats out;
    out.reported = true;
    const quint64 rxPackets = s.control.rxPackets + s.serial.rxPackets + s.audio.rxPackets;
    out.alive = rxPackets > m_link.rxPackets;
    out.rxBytes = static_cast<qint64>(s.control.rxBytes + s.serial.rxBytes + s.audio.rxBytes);
    out.txBytes = static_cast<qint64>(s.control.txBytes + s.serial.txBytes + s.audio.txBytes);
    out.rxPackets = rxPackets;
    out.rxPacketsLost = s.serial.rxLost + s.audio.rxLost;
    // The ping round trip on the CONTROL stream only: the serial and audio
    // streams carry real traffic and their timing is not a clean round trip.
    out.rttMs = s.control.rttMs;

    m_link = out;
    emit linkStatsUpdated(out);
}

IRadioBackend::HealthSnapshot IcomCivBackend::healthSnapshot() const
{
    HealthSnapshot h;
    h.sections.insert(QStringLiteral("model"), QStringLiteral("Radio"));
    h.values.insert(QStringLiteral("model"),
                    QString::fromUtf8(m_model->name.data(),
                                      static_cast<int>(m_model->name.size())));
    h.labels.insert(QStringLiteral("model"), QStringLiteral("Model"));
    h.order << QStringLiteral("model");

    h.values.insert(QStringLiteral("civ"),
                    QStringLiteral("0x%1").arg(m_model->civAddress, 2, 16, QLatin1Char('0')));
    h.labels.insert(QStringLiteral("civ"), QStringLiteral("CI-V address"));
    h.order << QStringLiteral("civ");

    if (!m_model->verified) {
        // Say so rather than presenting cross-referenced numbers as measured.
        h.values.insert(QStringLiteral("verified"), QStringLiteral("capabilities unverified"));
        h.labels.insert(QStringLiteral("verified"), QStringLiteral("Model data"));
        h.order << QStringLiteral("verified");
    }

    h.sections.insert(QStringLiteral("ovf"), QStringLiteral("Front end"));
    h.values.insert(QStringLiteral("ovf"), m_overflow ? QStringLiteral("OVERLOAD")
                                                      : QStringLiteral("ok"));
    h.labels.insert(QStringLiteral("ovf"), QStringLiteral("ADC overflow"));
    h.order << QStringLiteral("ovf");

    // Vd and Id only if the radio has actually reported them. A key absent from
    // `values` renders as "not reported", which is genuinely different from 0 V.
    if (m_vdVolts > 0.0) {
        h.values.insert(QStringLiteral("vd"), QStringLiteral("%1 V").arg(m_vdVolts, 0, 'f', 1));
        h.labels.insert(QStringLiteral("vd"), QStringLiteral("PA supply"));
        h.order << QStringLiteral("vd");
    }
    if (m_idAmps > 0.0) {
        h.values.insert(QStringLiteral("id"), QStringLiteral("%1 A").arg(m_idAmps, 0, 'f', 2));
        h.labels.insert(QStringLiteral("id"), QStringLiteral("PA current"));
        h.order << QStringLiteral("id");
    }
    // NO PA TEMPERATURE. The IC-705 does not report one, and the key is omitted
    // rather than reported as zero.
    return h;
}

IRadioBackend::LinkStats IcomCivBackend::linkStats() const { return m_link; }

}  // namespace AetherSDR::icom
