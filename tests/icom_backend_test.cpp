// IcomCIV — the IRadioBackend seam, against the fake IC-705.
//
// The load-bearing assertion here is the TCI/WSJT-X AUDIO CONTRACT. Everything
// downstream of this backend — the speaker, the TCI receiver channel, the
// decoders WSJT-X runs — consumes interleaved stereo float32 at 24 kHz. The
// radio delivers 48 kHz MONO. Neither half of that conversion is optional:
//
//   * skip the rate conversion and playback runs an octave low, so WSJT-X sees
//     every tone at twice its true frequency and decodes nothing;
//   * skip the channel duplication and TciServer, which divides the buffer by
//     2*sizeof(float), sees half the frames it thinks it has.
//
// Both failures are silent — audio flows, meters move, the session is healthy.
// So they get a test rather than a comment.

#include "IcomFakeRadio.h"

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::icom;
using namespace AetherSDR::icom::test;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AetherSDR::SliceDelta>("SliceDelta");
    qRegisterMetaType<AetherSDR::MeterDef>("MeterDef");

    FakeIc705 radio;
    IcomCivBackend backend;

    int sliceAudioBuffers = 0;
    qint64 sliceAudioBytes = 0;
    int speakerBuffers = 0;
    QObject::connect(&backend, &IRadioBackend::sliceAudioFrameReady, &app,
                     [&](int, const QByteArray& pcm) {
                         ++sliceAudioBuffers;
                         sliceAudioBytes += pcm.size();
                     });
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &app,
                     [&](const QByteArray&) { ++speakerBuffers; });

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    QSignalSpy meterDefSpy(&backend, &IRadioBackend::meterDefined);
    QSignalSpy rfGainInfoSpy(&backend, &IRadioBackend::panRfGainInfoChanged);
    QSignalSpy spectrumSpy(&backend, &IRadioBackend::spectrumFrameReady);

    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radio.controlPort();
    req.params.insert(QStringLiteral("icom.serialPort"), radio.serialPort());
    req.params.insert(QStringLiteral("icom.audioPort"), radio.audioPort());
    req.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
    req.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
    req.params.insert(QStringLiteral("icom.civAddress"), 0xA4);

    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }), "the backend connects");
    check(connectedSpy.count() == 1, "and emits connected() exactly once");

    // ---- capability -------------------------------------------------------
    const RadioCapabilities caps = backend.capabilities();
    check(caps.family == QStringLiteral("icom"), "family is icom");
    check(caps.maxSlices == 1, "one slice");
    // The three that are easy to get backwards, each with a real consequence.
    check(!caps.hostModulates,
          "the RADIO modulates — true here would open the host mic on a radio that never uses it");
    check(!caps.hasDaxStreams, "no IQ on any networked Icom — absent, not deferred");
    check(caps.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "the radio remembers its own state, so the client restores NOTHING");
    check(caps.hasRadioSideDsp, "NR/NB/notch run in the radio's firmware");
    check(!caps.canReboot, "power-off over WiFi is a one-way trip, so no reboot is offered");

    // ---- a slice exists, which TCI routing depends on ---------------------
    check(sliceSpy.count() >= 1, "a slice is published at connect");
    check(!meterDefSpy.isEmpty(), "meter definitions are published");
    check(rfGainInfoSpy.count() == 1, "the RF-gain range is advertised");
    if (rfGainInfoSpy.count() == 1) {
        const auto args = rfGainInfoSpy.first();
        // A three-position preamp, NOT a dB register. Advertising a continuous
        // range gives the operator a slider that sweeps over a control with
        // three detents.
        check(args.at(1).toInt() == 0 && args.at(2).toInt() == 2 && args.at(3).toInt() == 1,
              "as the real discrete 0..2 step 1, not a fabricated dB range");
    }

    // ---- model discovery (Phase 5) ----------------------------------------
    check(waitFor([&] { return backend.model().civAddress == 0xA4; }),
          "the backend ASKS the radio what it is (0x19 0x00) rather than assuming");
    check(backend.model().name == "IC-705", "and resolves the IC-705");
    check(backend.model().verified, "whose capability numbers are tier-1 verified");

    // ---- THE AUDIO CONTRACT (TCI / WSJT-X) --------------------------------
    //
    // Feed exactly 4800 mono samples at 48 kHz — 100 ms. The contract says the
    // seam must see 100 ms of INTERLEAVED STEREO at 24 kHz, which is
    // 2400 frames x 2 channels x 4 bytes = 19200 bytes.
    constexpr int kMonoSamplesIn = 4800;
    constexpr qint64 kExpectedBytes = 2400 * 2 * static_cast<qint64>(sizeof(float));
    {
        std::vector<float> mono(kMonoSamplesIn);
        for (int i = 0; i < kMonoSamplesIn; ++i)
            mono[static_cast<std::size_t>(i)] =
                0.25f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        // Push it the way the radio does: in the protocol's own unequal pair,
        // 682 + 278 samples per 20 ms frame, rather than as one big block.
        std::size_t at = 0;
        while (at < mono.size()) {
            const std::size_t n = std::min<std::size_t>(682, mono.size() - at);
            radio.pushAudio(encodeAudio(AudioCodec::Lpcm1ch16,
                                        std::span<const float>(mono.data() + at, n)));
            at += n;
        }
    }

    check(waitFor([&] { return sliceAudioBytes >= kExpectedBytes * 8 / 10; }),
          "per-slice audio reaches the seam — this IS the TCI receiver channel");

    // r8brain has a startup latency, so the exact byte count lags by a small
    // amount on the first block. What must hold is the RATIO: 4800 mono
    // samples in at 48 kHz must produce ~2400 stereo FRAMES out at 24 kHz.
    const qint64 framesOut = sliceAudioBytes / (2 * static_cast<qint64>(sizeof(float)));
    check(framesOut > 2000 && framesOut < 2600,
          "4800 mono samples at 48 kHz become ~2400 stereo frames at 24 kHz");
    check(framesOut < kMonoSamplesIn * 3 / 4,
          "NOT a passthrough — a backend that skipped the rate conversion would emit ~4800");

    // Stereo, not mono. TciServer divides by 2*sizeof(float); a mono buffer
    // makes it see half the frames it thinks it has.
    check(sliceAudioBytes % (2 * static_cast<qint64>(sizeof(float))) == 0,
          "the buffer is a whole number of INTERLEAVED STEREO frames");

    // Both feeds, and they are different consumers: the speaker gets the mix,
    // the per-slice feed gets one slice for the decoders.
    check(speakerBuffers > 0, "the speaker feed is emitted too");
    check(sliceAudioBuffers == speakerBuffers,
          "one per-slice buffer for every speaker buffer — neither path is starved");

    // ---- spectrum (Phase 2 through the seam) ------------------------------
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x00);
        body.push_back(encodeBcdByte(1));
        body.push_back(encodeBcdByte(1));
        body.push_back(0x00);                       // centre mode
        const auto centre = encodeFreq(14'100'000);
        const auto span   = encodeFreq(100'000);
        body.insert(body.end(), centre.begin(), centre.end());
        body.insert(body.end(), span.begin(), span.end());
        body.push_back(0x00);
        for (int i = 0; i < kScopePointsIc705; ++i)
            body.push_back(static_cast<std::uint8_t>(i % (kScopeMaxAmplitude + 1)));
        std::vector<std::uint8_t> civ{0xFE, 0xFE, kControllerAddress, kIc705Addr,
                                      cmd::kScope, scope::kWaveData};
        civ.insert(civ.end(), body.begin(), body.end());
        civ.push_back(kCivEom);
        radio.pushCiv(civ);
    }
    check(waitFor([&] { return spectrumSpy.count() > 0; }),
          "a scope sweep reaches the seam as a spectrum frame");
    if (spectrumSpy.count() > 0) {
        const QByteArray frame = spectrumSpy.first().at(1).toByteArray();
        check(frame.size() == kScopePointsIc705 * static_cast<int>(sizeof(float)),
              "475 float32 bins");
    }

    // ---- metering (Phase 4) through the seam ------------------------------
    {
        QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
        // The radio answers the S-meter poll the scheduler is already issuing.
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kMeter,
                       meter::kSMeter, 0x01, 0x20, kCivEom});   // BCD 0120 == S9
        check(waitFor([&] { return meterSpy.count() > 0; }), "a meter reading reaches the seam");
        if (meterSpy.count() > 0) {
            const auto args = meterSpy.first();
            check(args.at(0).toString() == QStringLiteral("LEVEL"), "as the LEVEL meter");
            // Raw 120 is S9. On 20 m that is -73 dBm; the value must be
            // calibrated, not the raw byte.
            check(std::fabs(args.at(1).toDouble() - -73.0) < 1.0,
                  "calibrated to -73 dBm (S9 on HF), not passed through raw");
        }
    }

    // ---- health -----------------------------------------------------------
    {
        const auto h = backend.healthSnapshot();
        check(!h.isEmpty(), "the health snapshot is populated");
        // The IC-705 reports no PA temperature, and the key is OMITTED rather
        // than reported as zero — "not reported" and "0 degrees" are different
        // answers on a health readout.
        check(!h.values.contains(QStringLiteral("patemp")),
              "no PA temperature key, because the radio does not report one");
        check(h.values.contains(QStringLiteral("model")), "the resolved model is reported");
    }

    backend.disconnectRadio();
    check(!backend.isConnected(), "the backend disconnects cleanly");

    if (g_failures == 0)
        std::printf("icom_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
