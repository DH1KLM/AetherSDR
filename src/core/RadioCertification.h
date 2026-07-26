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
class RadioCertification {
public:
    // Bring-up order, and it is deliberate: each phase depends only on the ones
    // before it.
    //
    //   Tune    control plane only — no DSP, no audio, no meters
    //   Rx      demodulation and handedness — audio evidence, still no meters
    //   Tx      keying and modulation — audio evidence where it exists
    //   Meters  the instruments themselves, LAST, against known stimuli
    //
    // Meters come last because they are not trustworthy until something has
    // checked them, and the earlier phases must therefore not lean on them. A
    // transmit stage that concludes "no RF" from a missing SWR reading is really
    // reporting "no SWR reading", and the two are only the same thing once the
    // meters have been validated. Where a stage does use a meter, it labels the
    // conclusion meterDependent so a failure can be attributed to the right
    // subsystem instead of the nearest one.
    enum class Phase { Tune, Rx, Tx, Meters, All };

    struct Options {
        Phase phase = Phase::All;
        double frequencyMhz = 14.200;   // mid-band: both sidebands stay in band
        QString mode = QStringLiteral("USB");
        int settleMs = 2500;            // per keyed measurement
        bool includeAudioProbe = true;  // the demodulated-sideband stage

        // A known off-air carrier for the receive stages. WWV is the default
        // because it is free, always on, on an exactly known frequency, and
        // comes from a source that is not us — which makes it the one external
        // reference available without a second radio.
        double referenceCarrierMhz = 10.000;
        double referenceOffsetHz = 1500.0;   // park the dial this far off it
    };

    RadioCertification(RadioModel* radio, AudioEngine* audio);

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

    // ---- control-plane stages (no DSP, no meters) ----
    void stageModeMap();
    void stageTuning(const Options& o);

    // ---- meter stages, LAST ----
    void stageMeterInventory();
    void stageMeterScale(const Options& o);

    // ---- receive stages ----
    //
    // These run FIRST when both phases are selected, and not by accident: the
    // wire's handedness is one fact that transmit and receive both consume, and
    // transmit cannot be reasoned about until it is settled (HERMES.md 15.6).
    void stageConsumerAgreement(const Options& o);
    void stageZeroShift(const Options& o);
    void stageRxSidebands(const Options& o);
    void stagePassbandAfterModeChange(const Options& o);

    // ---- transmit stages, in the order the signal travels ----
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

    // Drive used for the sideband probe only. Enough to measure, low enough
    // that the receiver listening to its own transmitter is not driven into
    // clipping — at full power it saturates and the measurement is worthless.
    static constexpr int kSidebandProbePowerPercent = 5;

    void spin(int ms);
    QJsonObject meterSnapshot() const;

    RadioModel* m_radio = nullptr;
    AudioEngine* m_audio = nullptr;
    QJsonArray m_stages;
};

}  // namespace AetherSDR
