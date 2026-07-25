#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class RadioModel;
class AudioEngine;

// Transmit bring-up instrument: drive the whole transmit chain against the
// radio itself and report what every stage actually did.
//
// THIS IS A DIAGNOSTIC, NOT A CERTIFICATION. It deliberately does not return
// pass/fail. Thresholds that are meaningful for one radio are guesses for the
// next, and a tool that says PASS on a radio nobody has characterised is worse
// than one that says "forward power 3846 counts, mic peak -32 dBFS, sideband
// consistent". Judgement stays with the operator or the agent reading it; this
// gathers the evidence they would otherwise gather by hand over several days.
//
// It assumes NO simulator and no second receiver — everything here runs against
// one radio, because that is the situation a new backend starts in.
//
// WHAT IT IS BUILT FROM. Every stage exists because something in the
// Hermes-Lite 2 bring-up failed silently at exactly that point. The stage list
// is a transcription of HERMES.md section 14, and each result carries the
// reference so a future agent lands on the write-up rather than re-deriving it:
//
//   - four separate defects each produced a correct-looking keyed transmission
//     with ZERO forward power (14.1)
//   - a modulator with 85 dB opposite-sideband suppression radiated double
//     sideband, because the quadrature filter was all-pass in magnitude (14.2)
//   - keying reached the radio from the automation bridge and not from the MOX
//     button, because they drive different models (14.5)
//   - transmit ran on the WRONG SIDEBAND while every internal instrument agreed
//     it was right (14.6)
//
// The last one shapes the design. A convention error is invisible to any check
// that shares the convention, so the sideband stage demodulates our own
// transmission rather than looking at the panadapter: the panadapter reads raw
// wire order and agrees with the transmitter by construction, while the
// demodulator applies the receive conjugation and WDSP's sideband selection
// independently. That is a genuinely different path — though still not as
// strong as an unrelated receiver, which is why the report ends with the manual
// checks a human must still perform.
class TxCertification {
public:
    struct Options {
        double frequencyMhz = 14.200;   // mid-band: both sidebands stay in band
        QString mode = QStringLiteral("USB");
        int settleMs = 2500;            // per keyed measurement
        bool includeAudioProbe = true;  // the demodulated-sideband stage
    };

    TxCertification(RadioModel* radio, AudioEngine* audio);

    // Runs the whole sequence synchronously, spinning the event loop between
    // steps. Returns the report. Expect this to take tens of seconds and to key
    // the transmitter repeatedly — the caller is responsible for having decided
    // that is allowed.
    QJsonObject run(const Options& options);

private:
    // One measurement, recorded whether or not it looked healthy.
    //
    // `concern` is the closest thing to a verdict: it is set when a value falls
    // outside what this radio has previously been observed to do, and it names
    // the suspicion rather than declaring failure. `reference` points at the
    // HERMES.md section that explains the failure mode, so the next agent gets
    // the history rather than a bare number.
    void record(const QString& id, const QString& title,
                const QJsonObject& measured, const QString& observation,
                const QString& concern = QString(),
                const QString& reference = QString());

    // Stages, in the order the signal actually travels.
    void stagePreconditions();
    void stageControlPlane(const Options& o);
    void stageDspLiveness(const Options& o);
    void stageRf(const Options& o);
    void stageSideband(const Options& o);
    void stageCarrierSuppression(const Options& o);
    void stageLifecycle(const Options& o);

    // Key through TransmitModel, NOT RadioModel::setTransmit.
    //
    // The automation bridge drives RadioModel and the MOX button drives
    // TransmitModel, and three separate bugs reached the operator through that
    // gap (HERMES.md 14.5). A transmit diagnostic that keyed the way only the
    // bridge can would inherit exactly the blindness it exists to remove.
    void keyViaOperatorPath(bool on);

    void spin(int ms);
    QJsonObject meterSnapshot() const;

    RadioModel* m_radio = nullptr;
    AudioEngine* m_audio = nullptr;
    QJsonArray m_stages;
};

}  // namespace AetherSDR
