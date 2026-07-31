// Tests the receiver on audio shaped like what actually arrives off the air,
// not like a capture file.
//
// The offline tooling could split captures on exact digital zeros, because a
// modem's own output really does go to zero between bursts. A receiver gets
// none of that: there is a noise floor, the frame does not start on a sample
// boundary anyone chose, and the stream arrives in arbitrary chunks. So the
// frames here are deliberately buried in noise, offset by a number that is not
// a multiple of the symbol length, and fed in irregular pieces.

#include "core/VaraCodec.h"
#include "core/VaraReceiver.h"
#include "core/VaraWaveform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <random>

using namespace AetherSDR::Vara;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString modelPath()
{
    for (const char* c : {"data/vara/vara_model.json",
                          "../data/vara/vara_model.json",
                          "../../data/vara/vara_model.json"}) {
        if (QFile::exists(QString::fromLatin1(c)))
            return QString::fromLatin1(c);
    }
    return {};
}

// Frame audio embedded in noise at a deliberately awkward offset.
QVector<float> buildStream(const QVector<int>& symbols, int leadSamples,
                           double noiseAmplitude, unsigned seed)
{
    const QVector<qint16> pcm = Waveform::synthesise(symbols);
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, noiseAmplitude);

    QVector<float> out;
    out.reserve(leadSamples + pcm.size() + 4096);
    for (int i = 0; i < leadSamples; ++i)
        out.append(float(gauss(rng)));
    for (int i = 0; i < pcm.size(); ++i)
        out.append(float(pcm.at(i) + gauss(rng)));
    for (int i = 0; i < 4096; ++i)
        out.append(float(gauss(rng)));
    return out;
}

// Feed in irregular chunks, as a real audio callback would.
void feedInChunks(Receiver& rx, const QVector<float>& stream, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> size(700, 5000);
    int pos = 0;
    while (pos < stream.size()) {
        const int n = std::min(size(rng), int(stream.size() - pos));
        rx.feed(stream.mid(pos, n), Waveform::kSampleRate);
        pos += n;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString model = modelPath();
    if (model.isEmpty()) {
        std::printf("[SKIP] model file not found\n");
        return 0;
    }
    Codec codec;
    QString err;
    if (!codec.load(model, &err)) {
        std::printf("[FAIL] could not load the model: %s\n", qPrintable(err));
        return 1;
    }

    QByteArray payload(32, '\0');
    for (int bit : {3, 17, 42, 99, 200, 255})
        payload[bit / 8] = char(payload.at(bit / 8) | (1 << (7 - (bit % 8))));
    const QVector<int> symbols = codec.encode(payload);
    if (symbols.size() != Waveform::kFrameSymbols) {
        std::printf("[FAIL] could not encode a test frame\n");
        return 1;
    }

    // ── A clean frame at an awkward offset ──────────────────────────────
    {
        Receiver rx;
        expect(rx.loadModel(model), "the receiver loads the model");
        rx.setPayloadBytes(32);
        QSignalSpy detected(&rx, &Receiver::frameDetected);
        QSignalSpy decoded(&rx, &Receiver::framePayload);

        // 1237 is deliberately not a multiple of 512: the receiver must recover
        // symbol timing rather than assume the frame starts on the grid.
        feedInChunks(rx, buildStream(symbols, 1237, 3.0, 1), 2);

        expect(detected.count() == 1, "exactly one frame is detected");
        expect(decoded.count() == 1, "exactly one payload is decoded");
        if (decoded.count() == 1) {
            expect(decoded.at(0).at(0).toByteArray() == payload,
                   "the decoded payload is correct despite the offset");
            expect(decoded.at(0).at(1).toInt() == 0,
                   "a clean frame decodes with residual 0");
        }
        if (detected.count() == 1) {
            const double conf = detected.at(0).at(1).toDouble();
            std::printf("       (median tone confidence: %.4g)\n", conf);
            expect(conf > 100.0, "the detected frame demodulates with a large margin");
        }
    }

    // ── Silence must produce nothing ────────────────────────────────────
    {
        Receiver rx;
        rx.loadModel(model);
        QSignalSpy detected(&rx, &Receiver::frameDetected);
        QVector<float> quiet(200000, 0.0f);
        rx.feed(quiet, Waveform::kSampleRate);
        expect(detected.count() == 0, "silence produces no detections");
    }

    // ── Noise alone must not look like a frame ──────────────────────────
    {
        Receiver rx;
        rx.loadModel(model);
        QSignalSpy detected(&rx, &Receiver::frameDetected);
        std::mt19937 rng(9);
        std::normal_distribution<double> gauss(0.0, 2000.0);
        QVector<float> noise(600000);
        for (int i = 0; i < noise.size(); ++i)
            noise[i] = float(gauss(rng));
        rx.feed(noise, Waveform::kSampleRate);
        expect(detected.count() == 0,
               "loud noise is not mistaken for a frame (energy alone is not enough)");
    }

    // ── Two frames back to back ─────────────────────────────────────────
    {
        Receiver rx;
        rx.loadModel(model);
        rx.setPayloadBytes(32);
        QSignalSpy decoded(&rx, &Receiver::framePayload);

        QVector<float> stream = buildStream(symbols, 900, 3.0, 4);
        stream.append(buildStream(symbols, 5000, 3.0, 5));
        feedInChunks(rx, stream, 6);
        expect(decoded.count() == 2,
               "two frames in one stream are both decoded, not merged");
    }

    // ── A wrong sample rate is refused, not silently mis-decoded ────────
    {
        Receiver rx;
        rx.loadModel(model);
        QSignalSpy bad(&rx, &Receiver::unsupportedSampleRate);
        QSignalSpy detected(&rx, &Receiver::frameDetected);
        rx.feed(buildStream(symbols, 512, 1.0, 7), 44100);
        expect(bad.count() == 1 && detected.count() == 0,
               "audio at the wrong rate is refused rather than decoded as nonsense");
    }

    // ── A corrupted frame is still recovered ────────────────────────────
    {
        Receiver rx;
        rx.loadModel(model);
        rx.setPayloadBytes(32);
        QSignalSpy decoded(&rx, &Receiver::framePayload);

        QVector<int> damaged = symbols;
        damaged[40] = (damaged.at(40) + 5) % 16;
        damaged[180] = (damaged.at(180) + 9) % 16;
        feedInChunks(rx, buildStream(damaged, 777, 3.0, 8), 9);
        expect(decoded.count() == 1, "a damaged frame is still decoded");
        if (decoded.count() == 1) {
            expect(decoded.at(0).at(0).toByteArray() == payload,
                   "two corrupted symbols are corrected back to the right payload");
        }
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
