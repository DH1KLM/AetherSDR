#include "core/RadioCertification.h"

#include "core/AudioEngine.h"
#include "core/ClientTxTestTone.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QByteArray>
#include <QEventLoop>
#include <QTimer>

#include <cmath>
#include <complex>
#include <vector>

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Correlate a real audio buffer against a frequency. Used instead of a full FFT
// because we are asking one question about one known frequency.
double tonePower(const std::vector<float>& mono, double hz, double fs)
{
    if (mono.empty() || fs <= 0.0)
        return 0.0;
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * kPi * hz / fs;
    for (std::size_t n = 0; n < mono.size(); ++n) {
        const double ph = w * static_cast<double>(n);
        acc += static_cast<double>(mono[n])
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(mono.size());
}

double rms(const std::vector<float>& mono)
{
    if (mono.empty())
        return 0.0;
    double acc = 0.0;
    for (const float v : mono)
        acc += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(acc / static_cast<double>(mono.size()));
}

double db(double v) { return 20.0 * std::log10(std::max(1e-12, v)); }

}  // namespace

RadioCertification::RadioCertification(RadioModel* radio, AudioEngine* audio)
    : m_radio(radio), m_audio(audio) {}

void RadioCertification::spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void RadioCertification::record(const QString& id, const QString& title,
                             const QJsonObject& measured,
                             const QString& observation,
                             const QString& concern,
                             const QString& reference)
{
    QJsonObject stage{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("measured"), measured},
        {QStringLiteral("observation"), observation},
    };
    if (!concern.isEmpty())
        stage[QStringLiteral("concern")] = concern;
    if (!reference.isEmpty())
        stage[QStringLiteral("reference")] = reference;
    m_stages.append(stage);
}

void RadioCertification::keyViaOperatorPath(bool on)
{
    if (!m_radio)
        return;
    auto& tx = m_radio->transmitModel();
    if (on)
        tx.requestPttOn(TransmitModel::PttSource::Mox);
    else
        tx.requestPttOff(TransmitModel::PttSource::Mox);
}

QJsonObject RadioCertification::meterSnapshot() const
{
    QJsonObject out;
    if (!m_radio)
        return out;
    const auto& meters = m_radio->meterModel();
    auto put = [&](const char* key, const char* source, const char* name) {
        const int idx = meters.findMeter(QString::fromLatin1(source),
                                         QString::fromLatin1(name));
        if (idx >= 0)
            out[QString::fromLatin1(key)] = static_cast<double>(meters.value(idx));
    };
    put("micPeakDbfs", "TX", "MICPEAK");
    put("swr", "TX", "SWR");
    put("paTempC", "RAD", "PATEMP");
    put("sLevelDbm", "SLC", "LEVEL");
    return out;
}

// ---------------------------------------------------------------------------
// Receive stages. Every one of these is a transcription of HERMES.md 15.
// ---------------------------------------------------------------------------

void RadioCertification::stageModeMap()
{
    // A mode name the backend does not recognise falls through to a default —
    // silently. On the HL2, plain "CW" was absent while "CWU" was present, so CW
    // was demodulated as SSB with the mode indicator reading correctly, and
    // "RTTY" is advertised over TCI to this day with no mapping behind it.
    //
    // This enumerates every mode the application can actually emit and asks the
    // slice to take each one. It cannot see inside the backend's lookup, so what
    // it reports is the readback — but a mode that does not survive a round trip
    // is certainly wrong, and one that does is at least addressable.
    static const QStringList kModes{
        QStringLiteral("USB"), QStringLiteral("LSB"),
        QStringLiteral("CW"),  QStringLiteral("CWU"), QStringLiteral("CWL"),
        QStringLiteral("AM"),  QStringLiteral("SAM"),
        QStringLiteral("FM"),  QStringLiteral("NFM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"),
        QStringLiteral("RTTY"),
    };

    QJsonObject results;
    QStringList notRetained;
    auto* slice = m_radio ? m_radio->slice(0) : nullptr;
    const QString restore = slice ? slice->mode() : QString();

    for (const QString& mode : kModes) {
        if (!slice)
            break;
        slice->setMode(mode);
        spin(250);
        const QString back = slice->mode();
        results[mode] = back;
        if (back.compare(mode, Qt::CaseInsensitive) != 0)
            notRetained << (mode + QStringLiteral("->") + back);
    }
    if (slice && !restore.isEmpty())
        slice->setMode(restore);

    QString concern;
    if (!notRetained.isEmpty())
        concern = QStringLiteral(
            "these modes did not survive a round trip: ") + notRetained.join(", ")
            + QStringLiteral(". A mode the backend does not map does not fail — "
                             "it becomes the default, usually USB, while the UI "
                             "still shows what was asked for");

    record(QStringLiteral("mode-map"),
           QStringLiteral("Every mode the app can emit survives a round trip"),
           results,
           QStringLiteral(
               "Readback is a weak proof — it shows the model kept the name, not "
               "that the demodulator understood it. Treat a clean result as "
               "'nothing obviously dropped', not as 'all modes work'."),
           concern,
           QStringLiteral("HERMES.md 15.7"));
}

void RadioCertification::stageConsumerAgreement(const Options& o)
{
    // The panadapter and the demodulator are INDEPENDENT consumers of the same
    // IQ buffer, and on the HL2 each was wired to the other's convention. The
    // audio was right at normal tuning and the panadapter was visibly mirrored —
    // and the panadapter, the one instrument with no compensating error, was the
    // easiest to dismiss as "a display bug".
    //
    // This stage does not try to decide which is right. It reports whether they
    // agree, because a disagreement means one of them is compensating for
    // something and that is the fact worth surfacing.
    if (!m_radio)
        return;

    QJsonObject m{
        {QStringLiteral("note"), QStringLiteral(
            "Requires a signal OFF the pan centre. A mirror is invisible on its "
            "own axis, which is how a live sideband sweep once confirmed 'all "
            "four modes correct' while the display was plainly mirrored.")},
        {QStringLiteral("referenceCarrierMhz"), o.referenceCarrierMhz},
        {QStringLiteral("dialOffsetHz"), o.referenceOffsetHz},
    };

    record(QStringLiteral("consumer-agreement"),
           QStringLiteral("Panadapter and demodulator agree on which side a signal is"),
           m,
           QStringLiteral(
               "Park a known carrier off-centre, then compare where the "
               "panadapter draws it against which sideband recovers it. They are "
               "independent consumers and can disagree."),
           QStringLiteral(
               "NOT YET AUTOMATED — needs a spectrum tap through the seam. Until "
               "then this is an operator check, and it is the one that found the "
               "receive inversion"),
           QStringLiteral("HERMES.md 15.5"));
}

void RadioCertification::stageZeroShift(const Options& o)
{
    // Two compensating errors cancel at any non-zero shift, so a measurement
    // taken at normal off-centre tuning sees a corrected result and proves
    // nothing. Zero shift is the one geometry where nothing can compensate.
    //
    // Force it by exploiting the NCO re-centre rule: tune far enough that the
    // NCO must jump, then land on the target, and the NCO follows it exactly.
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    const double target = o.referenceCarrierMhz - (o.referenceOffsetHz / 1.0e6);
    slice->setFrequency(target - 3.0);   // far: forces the NCO to move
    spin(1200);
    slice->setFrequency(target);         // land: NCO re-centres, shift == 0
    spin(1500);

    QJsonObject m{
        {QStringLiteral("dialMhz"), slice->frequency()},
        {QStringLiteral("intendedMhz"), target},
        {QStringLiteral("method"), QStringLiteral(
            "tuned 3 MHz away to force an NCO jump, then landed on the target so "
            "the NCO follows and the slice shift is exactly zero")},
    };

    record(QStringLiteral("zero-shift"),
           QStringLiteral("Establish the zero-shift geometry"),
           m,
           QStringLiteral(
               "Any handedness measurement taken at a non-zero shift can be "
               "corrected by a second, opposite error. This puts the radio in the "
               "one geometry where that cannot happen — do the sideband checks "
               "from here."),
           slice->frequency() > 0.0 ? QString()
                                    : QStringLiteral("could not establish a dial frequency"),
           QStringLiteral("HERMES.md 15.4"));
}

void RadioCertification::stageRxSidebands(const Options& o)
{
    // All FOUR SSB-family modes against a known off-centre carrier. Not one mode,
    // and not at the pan centre.
    //
    // A sideband test that runs in a single mode proves nothing about handedness:
    // hl2_shift_test validated in LSB, the one mode the inversion made correct,
    // and passed throughout.
    if (!m_audio || !m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    QJsonObject perMode;
    for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                QStringLiteral("DIGU"), QStringLiteral("DIGL")}) {
        slice->setMode(mode);
        spin(900);
        m_audio->startAutomationAudioCapture(1500,
                                             QStringList{QStringLiteral("output")});
        spin(1700);
        const QJsonObject snap = m_audio->automationAudioCaptureSnapshot(true);

        std::vector<float> mono;
        for (const QJsonValue& cv : snap.value(QStringLiteral("chunks")).toArray()) {
            const QJsonObject c = cv.toObject();
            const int ch = std::max(1, c.value(QStringLiteral("channels")).toInt(1));
            const QByteArray pcm = QByteArray::fromBase64(
                c.value(QStringLiteral("pcmBase64")).toString().toLatin1());
            const auto* f = reinterpret_cast<const float*>(pcm.constData());
            const int frames = static_cast<int>(pcm.size() / sizeof(float)) / ch;
            for (int n = 0; n < frames; ++n)
                mono.push_back(f[n * ch]);
        }

        const double fs = AudioEngine::DEFAULT_SAMPLE_RATE;
        perMode[mode] = QJsonObject{
            {QStringLiteral("toneAtOffsetDb"), db(tonePower(mono, o.referenceOffsetHz, fs))},
            {QStringLiteral("overallRmsDb"), db(rms(mono))},
            {QStringLiteral("frames"), static_cast<int>(mono.size())},
        };
    }
    slice->setMode(o.mode);

    record(QStringLiteral("rx-sidebands"),
           QStringLiteral("All four SSB-family modes against a known carrier"),
           perMode,
           QStringLiteral(
               "With the dial parked so a known carrier sits at the configured "
               "offset, the modes whose passband covers that side should recover "
               "the tone and the other two should not. USB and DIGU should agree "
               "with each other, LSB and DIGL likewise, and the two pairs should "
               "disagree — if all four recover it, the dial is probably not where "
               "this stage thinks it is."),
           QStringLiteral(
               "Interpretation needs a real carrier present. With no antenna or "
               "no signal at the reference frequency every mode reads the noise "
               "floor and this stage is inconclusive rather than passing"),
           QStringLiteral("HERMES.md 15.3, 15.4"));
}

void RadioCertification::stagePassbandAfterModeChange(const Options& o)
{
    // SetRXAMode DISCARDS a passband applied before it, so a backend that pushes
    // the filter and then the mode ends up with a sticky window. Arriving at DIGU
    // from CW handed the decoder a ~500 Hz passband and it decoded nothing, with
    // the mode indicator reading correctly the whole time.
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (!slice)
        return;

    slice->setMode(QStringLiteral("CW"));
    spin(700);
    const int cwLow = slice->filterLow(), cwHigh = slice->filterHigh();
    slice->setMode(QStringLiteral("DIGU"));
    spin(700);
    const int digLow = slice->filterLow(), digHigh = slice->filterHigh();
    slice->setMode(o.mode);
    spin(500);

    QJsonObject m{
        {QStringLiteral("cwWidthHz"), cwHigh - cwLow},
        {QStringLiteral("diguWidthHz"), digHigh - digLow},
        {QStringLiteral("cwPassband"), QStringLiteral("%1..%2").arg(cwLow).arg(cwHigh)},
        {QStringLiteral("diguPassband"), QStringLiteral("%1..%2").arg(digLow).arg(digHigh)},
    };

    QString concern;
    if (digHigh - digLow <= cwHigh - cwLow)
        concern = QStringLiteral(
            "the passband did not widen moving from CW to DIGU — it looks sticky. "
            "A radio that owns its DSP gets no mode echo to heal this, so the "
            "backend must supply a per-mode default passband and apply it AFTER "
            "the mode");

    record(QStringLiteral("passband-after-mode-change"),
           QStringLiteral("The passband follows a mode change"),
           m,
           QStringLiteral(
               "CW then DIGU is the ordering that exposed this: the narrow window "
               "survived into a wide mode and the decoder saw nothing."),
           concern,
           QStringLiteral("HERMES.md 15.7"));
}

void RadioCertification::stagePreconditions()
{
    const bool connected = m_radio && m_radio->isConnected();
    const bool canTx = m_radio && m_radio->backendCanTransmit();
    QJsonObject m{
        {QStringLiteral("connected"), connected},
        {QStringLiteral("family"), m_radio ? m_radio->family() : QString()},
        {QStringLiteral("canTransmit"), canTx},
        {QStringLiteral("hostModulation"),
         m_radio ? m_radio->transmitModel().hostModulation() : false},
        {QStringLiteral("micSelection"),
         m_radio ? m_radio->transmitModel().micSelection() : QString()},
        {QStringLiteral("txAudioStreaming"), m_audio && m_audio->isTxStreaming()},
    };

    QString concern;
    if (!connected)
        concern = QStringLiteral("not connected — nothing below this will mean anything");
    else if (!canTx)
        concern = QStringLiteral("transmit unavailable; the backend reports RX-only");
    else if (m_audio && !m_audio->isTxStreaming())
        concern = QStringLiteral(
            "TX audio capture is NOT running. On a host-modulating backend this "
            "silences the microphone AND the test tone, because the tone is "
            "injected inside the mic callback — the radio will key and transmit "
            "nothing");

    record(QStringLiteral("preconditions"),
           QStringLiteral("Backend, transmit gate and audio capture"),
           m,
           QStringLiteral("What the app believes about itself before any key."),
           concern,
           QStringLiteral("HERMES.md 14.1 defects 1 and 2"));
}

void RadioCertification::stageControlPlane(const Options& o)
{
    if (!m_radio)
        return;
    auto* slice = m_radio->slice(0);
    if (slice) {
        slice->setMode(o.mode);
        slice->setFrequency(o.frequencyMhz);
    }
    spin(1500);

    const double readbackMhz = slice ? slice->frequency() : 0.0;
    const QString readbackMode = slice ? slice->mode() : QString();
    QJsonObject m{
        {QStringLiteral("requestedMhz"), o.frequencyMhz},
        {QStringLiteral("readbackMhz"), readbackMhz},
        {QStringLiteral("requestedMode"), o.mode},
        {QStringLiteral("readbackMode"), readbackMode},
    };

    QString concern;
    if (std::fabs(readbackMhz - o.frequencyMhz) > 1e-6)
        concern = QStringLiteral(
            "the slice did not take the requested frequency; everything after "
            "this is measuring an unknown frequency");
    else if (readbackMode.compare(o.mode, Qt::CaseInsensitive) != 0)
        concern = QStringLiteral("the slice did not take the requested mode");

    record(QStringLiteral("control-plane"),
           QStringLiteral("Frequency and mode readback"),
           m,
           QStringLiteral(
               "Readback only proves the MODEL agrees. A radio that reports no "
               "VFO cannot be asked what it is really tuned to, so the app is "
               "authoritative and anything it fails to push is inherited from "
               "the previous session — that is how a VFO once read 10 MHz while "
               "the radio transmitted on 14."),
           concern,
           QStringLiteral("HERMES.md 14.1 defect on connect-time state"));
}

void RadioCertification::stageDspLiveness(const Options& o)
{
    if (!m_audio)
        return;
    // A known tone through the REAL audio path — the same entry the microphone
    // and the TONE button use — so this measures the chain, not a shortcut.
    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }
    keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject meters = meterSnapshot();
    keyViaOperatorPath(false);
    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    spin(800);

    const double micPeak = meters.value(QStringLiteral("micPeakDbfs")).toDouble(-140.0);
    QString concern;
    if (micPeak <= -139.0)
        concern = QStringLiteral(
            "no audio reached the modulator. The chain is broken ABOVE the DSP: "
            "either capture is not running, or the TX audio callback is gated on "
            "something this backend never satisfies");

    record(QStringLiteral("dsp-liveness"),
           QStringLiteral("Audio reaches the modulator"),
           meters,
           QStringLiteral(
               "Mic peak is measured pre-ALC, so it reports the level actually "
               "arriving rather than the ALC's success."),
           concern,
           QStringLiteral("HERMES.md 14.1 defects 1 and 2"));
}

void RadioCertification::stageRf(const Options& o)
{
    if (!m_audio)
        return;
    const QJsonObject idle = meterSnapshot();

    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }
    keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject keyed = meterSnapshot();
    keyViaOperatorPath(false);
    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    spin(1200);
    const QJsonObject after = meterSnapshot();

    const double tempIdle = idle.value(QStringLiteral("paTempC")).toDouble();
    const double tempKeyed = keyed.value(QStringLiteral("paTempC")).toDouble();
    QJsonObject m{
        {QStringLiteral("idle"), idle},
        {QStringLiteral("keyed"), keyed},
        {QStringLiteral("afterUnkey"), after},
        {QStringLiteral("paTempRiseC"), tempKeyed - tempIdle},
    };

    QString concern;
    if (!keyed.contains(QStringLiteral("swr")))
        concern = QStringLiteral(
            "no SWR reading while keyed, which usually means the radio reported "
            "no forward power — the transmitter is not producing RF even though "
            "audio reached the modulator. Check the PA enable and the drive level");
    else if (tempKeyed - tempIdle < 0.2)
        concern = QStringLiteral(
            "PA temperature did not rise. A wiring error can fake every other "
            "reading here; dissipation is the one that cannot be faked");

    record(QStringLiteral("rf"),
           QStringLiteral("The radio actually produces RF"),
           m,
           QStringLiteral(
               "SWR is meaningful uncalibrated because it is a ratio from one "
               "converter. Absolute power is NOT reported: raw counts dressed up "
               "as watts would be a confident lie."),
           concern,
           QStringLiteral("HERMES.md 14.1 defects 3 and 4"));
}

void RadioCertification::stageSideband(const Options& o)
{
    if (!m_audio || !m_radio || !o.includeAudioProbe)
        return;

    // THE STAGE THIS WHOLE TOOL EXISTS FOR.
    //
    // Demodulate our own transmission and ask what FREQUENCY comes back. A 1 kHz
    // tone transmitted in USB and received in USB must return 1 kHz of audio. If
    // the transmit sideband is inverted it lands on the other side of the
    // carrier, the USB demodulator hears nothing, and the LSB demodulator hears
    // it instead.
    //
    // This works where the panadapter does not. The panadapter reads raw wire
    // order and so agrees with the transmitter no matter which convention it
    // uses; the demodulator applies the receive conjugation and WDSP's sideband
    // selection, which are an independent implementation of the same question.
    m_radio->setTxAudioMonitor(true);   // hear ourselves, just for this stage

    auto capture = [&](const QString& mode) -> std::vector<float> {
        if (auto* slice = m_radio->slice(0))
            slice->setMode(mode);
        spin(900);
        m_audio->startAutomationAudioCapture(o.settleMs + 500,
                                             QStringList{QStringLiteral("output")});
        keyViaOperatorPath(true);
        spin(o.settleMs);
        keyViaOperatorPath(false);
        m_audio->stopAutomationAudioCapture();
        spin(400);

        // Pull the captured PCM back out and flatten to mono.
        const QJsonObject snap = m_audio->automationAudioCaptureSnapshot(true);
        std::vector<float> mono;
        for (const QJsonValue& cv : snap.value(QStringLiteral("chunks")).toArray()) {
            const QJsonObject c = cv.toObject();
            const int ch = std::max(1, c.value(QStringLiteral("channels")).toInt(1));
            const QByteArray pcm = QByteArray::fromBase64(
                c.value(QStringLiteral("pcmBase64")).toString().toLatin1());
            const auto* f = reinterpret_cast<const float*>(pcm.constData());
            const int frames = static_cast<int>(pcm.size() / sizeof(float)) / ch;
            for (int n = 0; n < frames; ++n)
                mono.push_back(f[n * ch]);
        }
        return mono;
    };

    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setFrequencyHz(1000.0f);
        tone->setLevelDb(-20.0f);
        tone->setEnabled(true);
    }

    const std::vector<float> sameSb = capture(o.mode);
    const QString oppositeMode = o.mode.compare(QStringLiteral("LSB"),
                                                Qt::CaseInsensitive) == 0
                                     ? QStringLiteral("USB") : QStringLiteral("LSB");
    const std::vector<float> oppSb = capture(oppositeMode);

    if (auto* tone = m_audio->clientTxTestTone())
        tone->setEnabled(false);
    m_radio->setTxAudioMonitor(false);
    if (auto* slice = m_radio->slice(0))
        slice->setMode(o.mode);

    // AudioEngine's native rate. The capture reports its own, but every chunk
    // here comes from the same sink.
    const double fs = AudioEngine::DEFAULT_SAMPLE_RATE;
    const double sameTone = tonePower(sameSb, 1000.0, fs);
    const double oppTone  = tonePower(oppSb, 1000.0, fs);
    const double sameAll  = rms(sameSb);
    const double oppAll   = rms(oppSb);

    QJsonObject m{
        {QStringLiteral("transmitMode"), o.mode},
        {QStringLiteral("demodMatched"), QJsonObject{
            {QStringLiteral("mode"), o.mode},
            {QStringLiteral("tone1kDb"), db(sameTone)},
            {QStringLiteral("overallRmsDb"), db(sameAll)},
            {QStringLiteral("frames"), static_cast<int>(sameSb.size())}}},
        {QStringLiteral("demodOpposite"), QJsonObject{
            {QStringLiteral("mode"), oppositeMode},
            {QStringLiteral("tone1kDb"), db(oppTone)},
            {QStringLiteral("overallRmsDb"), db(oppAll)},
            {QStringLiteral("frames"), static_cast<int>(oppSb.size())}}},
        {QStringLiteral("matchedMinusOppositeDb"), db(sameTone) - db(oppTone)},
    };

    QString concern;
    if (sameSb.empty() || oppSb.empty()) {
        concern = QStringLiteral(
            "no receive audio captured while transmitting. The TX audio monitor "
            "may not be implemented for this backend, in which case this stage "
            "cannot run and the sideband must be checked against a second receiver");
    } else if (db(oppTone) > db(sameTone)) {
        concern = QStringLiteral(
            "SIDEBAND LOOKS INVERTED. The tone came back louder demodulated on "
            "the OPPOSITE sideband than the one transmitted. On the HL2 this was "
            "a missing conjugation of the transmit IQ: the wire has the opposite "
            "handedness to the standard analytic convention, and the receive path "
            "already compensates while transmit did not");
    }

    record(QStringLiteral("sideband"),
           QStringLiteral("Demodulated sideband — the independent check"),
           m,
           QStringLiteral(
               "Compares the transmitted tone demodulated on the matching "
               "sideband against the opposite one. The matched side should be "
               "clearly louder. This is deliberately NOT a panadapter check: the "
               "panadapter reads the same wire order as the transmitter and "
               "cannot disagree with it."),
           concern,
           QStringLiteral("HERMES.md 14.6"));
}

void RadioCertification::stageCarrierSuppression(const Options& o)
{
    if (!m_audio)
        return;
    // SSB has no carrier. Keying with NO audio should produce essentially no RF;
    // anything substantial is DC offset in the modulator leaking a carrier at the
    // dial frequency. Never previously tested on this backend.
    keyViaOperatorPath(true);
    spin(o.settleMs);
    const QJsonObject keyedSilent = meterSnapshot();
    keyViaOperatorPath(false);
    spin(800);

    QJsonObject m{{QStringLiteral("keyedWithNoAudio"), keyedSilent}};
    QString concern;
    if (keyedSilent.contains(QStringLiteral("swr")))
        concern = QStringLiteral(
            "the radio reported forward power while keyed with NO audio. In SSB "
            "that is a carrier — most likely a DC offset in the modulator, or "
            "audio left over from a previous transmission that was not flushed");

    record(QStringLiteral("carrier-suppression"),
           QStringLiteral("Keyed with no audio produces no carrier"),
           m,
           QStringLiteral(
               "SSB should be silent when the operator is. A reading here is "
               "either a DC-offset carrier or an unflushed transmit queue."),
           concern,
           QStringLiteral("HERMES.md 14.1; queue flush on unkey"));
}

void RadioCertification::stageLifecycle(const Options& o)
{
    if (!m_audio || !m_radio)
        return;

    // Receive audio must be silent DURING transmit and must not burst afterwards.
    // Muting only the output is not enough: the demodulator keeps running on our
    // own signal, its filters fill, and the backlog drains on unkey — heard as
    // the tail of a transmission playing back after it ended.
    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject before = m_audio->automationAudioCaptureSnapshot(false);

    keyViaOperatorPath(true);
    spin(600);
    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject during = m_audio->automationAudioCaptureSnapshot(false);
    keyViaOperatorPath(false);

    m_audio->startAutomationAudioCapture(1200, QStringList{QStringLiteral("output")});
    spin(1400);
    const QJsonObject afterUnkey = m_audio->automationAudioCaptureSnapshot(false);

    const auto bytes = [](const QJsonObject& o) {
        return o.value(QStringLiteral("capturedBytes")).toInt();
    };
    QJsonObject m{
        {QStringLiteral("rxBytesBeforeKey"), bytes(before)},
        {QStringLiteral("rxBytesDuringTx"), bytes(during)},
        {QStringLiteral("rxBytesAfterUnkey"), bytes(afterUnkey)},
    };

    QString concern;
    if (bytes(during) > 0)
        concern = QStringLiteral(
            "receive audio was still flowing during transmit. The operator will "
            "hear their own carrier as fuzz and their own voice as feedback, and "
            "with an open microphone that closes an acoustic loop that corrupts "
            "the transmitted audio");
    else if (bytes(afterUnkey) == 0 && bytes(before) > 0)
        concern = QStringLiteral(
            "receive audio did not resume after unkey");

    record(QStringLiteral("lifecycle"),
           QStringLiteral("Receive muting across the transmit cycle"),
           m,
           QStringLiteral(
               "Zero bytes during transmit is what is wanted. A non-zero count "
               "AFTER unkey that greatly exceeds the before-key rate would "
               "indicate a drained backlog rather than a resumed stream."),
           concern,
           QStringLiteral("HERMES.md 14.1; RX mute at the demodulator"));
}

QJsonObject RadioCertification::run(const Options& o)
{
    m_stages = QJsonArray{};

    const bool doRx = o.phase != Phase::Tx;
    const bool doTx = o.phase != Phase::Rx;

    // RECEIVE FIRST WHEN BOTH ARE SELECTED, and not for tidiness. The wire's
    // handedness is ONE fact that both directions consume, and transmit cannot
    // be reasoned about until it is settled — a transmit result read before the
    // receive convention is known is a result about an unknown quantity.
    if (doRx) {
        stageModeMap();
        stageZeroShift(o);
        stageRxSidebands(o);
        stageConsumerAgreement(o);
        stagePassbandAfterModeChange(o);
    }

    if (doTx) {
        stagePreconditions();
        stageControlPlane(o);
        stageDspLiveness(o);
        stageRf(o);
        stageCarrierSuppression(o);
        stageSideband(o);
        stageLifecycle(o);
    }

    // Make sure we leave the radio unkeyed whatever happened above.
    keyViaOperatorPath(false);
    if (m_audio) {
        if (auto* tone = m_audio->clientTxTestTone())
            tone->setEnabled(false);
    }
    if (m_radio)
        m_radio->setTxAudioMonitor(false);

    // What this instrument CANNOT determine, stated as work for a human rather
    // than omitted. Everything here needs either a second receiver, an ear, or
    // equipment this application does not have — and pretending otherwise is
    // exactly how a wrong-sideband transmitter passed every check it had.
    QJsonArray manual{
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Sideband against an unrelated receiver")},
            {QStringLiteral("why"), QStringLiteral(
                "The sideband stage above demodulates our own transmission, which "
                "is a different path from the panadapter but still our own code. "
                "Two errors in the same direction would agree. Tune a separate "
                "radio to the same frequency and confirm the mode matches.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Audio quality and intelligibility")},
            {QStringLiteral("why"), QStringLiteral(
                "Level and frequency can both be correct while the audio is "
                "clipped, aliased or unintelligible. Nothing here measures "
                "distortion. Listen on another receiver.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Occupied bandwidth and splatter")},
            {QStringLiteral("why"), QStringLiteral(
                "A modulator can hit 85 dB opposite-sideband suppression and "
                "still radiate outside its passband — that happened here, and "
                "only a deliberate out-of-band probe found it. Check the signal "
                "width on a second receiver's panadapter.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Harmonics and spurs")},
            {QStringLiteral("why"), QStringLiteral(
                "The receive window is a few tens of kHz wide and centred on the "
                "transmit frequency, so it cannot see a harmonic by construction.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("ALC behaviour on real speech")},
            {QStringLiteral("why"), QStringLiteral(
                "A steady tone cannot reveal pumping between words or a first "
                "syllable lost to the attack. Speak, and listen.")}},
        QJsonObject{
            {QStringLiteral("check"), QStringLiteral("Long-transmission thermal behaviour")},
            {QStringLiteral("why"), QStringLiteral(
                "These stages key for a few seconds each. A net-length "
                "transmission is a different question for the PA.")}},
    };

    if (doRx) {
        manual.append(QJsonObject{
            {QStringLiteral("check"), QStringLiteral(
                "Panadapter versus demodulator, with a signal OFF centre")},
            {QStringLiteral("why"), QStringLiteral(
                "They are independent consumers of the same buffer and can "
                "disagree; when they did, each had the other's convention. A "
                "mirror is invisible on the pan centre, so the signal must be "
                "off-axis. This is the check that found the receive inversion, "
                "and it is not yet automated.")}});
        manual.append(QJsonObject{
            {QStringLiteral("check"), QStringLiteral(
                "Spots from a third party in the mode under test")},
            {QStringLiteral("why"), QStringLiteral(
                "PSK Reporter or a cluster spot is evidence that cannot come "
                "from a self-consistent loop. The receive bring-up ended with 63 "
                "spots on 14.074 DIGU — the first proof that was not our own "
                "code agreeing with itself.")}});
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("kind"), QStringLiteral("radio-bringup-diagnostic")},
        {QStringLiteral("phase"), o.phase == Phase::Rx ? QStringLiteral("rx")
                                : o.phase == Phase::Tx ? QStringLiteral("tx")
                                                       : QStringLiteral("all")},
        {QStringLiteral("note"), QStringLiteral(
            "Diagnostic only — this deliberately does not pass or fail. Read the "
            "concerns, then the measurements.")},
        {QStringLiteral("radio"), QJsonObject{
            {QStringLiteral("family"), m_radio ? m_radio->family() : QString()},
            {QStringLiteral("frequencyMhz"), o.frequencyMhz},
            {QStringLiteral("mode"), o.mode}}},
        {QStringLiteral("stages"), m_stages},
        {QStringLiteral("manualChecks"), manual},
    };
}

}  // namespace AetherSDR
