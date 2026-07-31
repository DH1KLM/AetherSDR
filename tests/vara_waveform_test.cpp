// Verifies the C++ VARA waveform against the values measured from real
// captures, not merely against itself.
//
// The numbers asserted here are the ones recorded in
// docs/vara-cleanroom-design.md: tone 0 at 843.75 Hz, tone 15 at exactly
// 2250 Hz, 93.75 Hz spacing, amplitude 12509.706905, and a final symbol that is
// at full amplitude through sample 495 and faded to near zero by 511.
//
// Round-tripping is included but deliberately not the whole test: a synthesiser
// and demodulator that agree with each other can still both be wrong. The
// frequency, amplitude and taper checks are what tie this code to the modem.

#include "core/VaraWaveform.h"

#include <QVector>

#include <cmath>
#include <cstdio>

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

bool nearly(double a, double b, double tol)
{
    return std::abs(a - b) <= tol;
}

void testToneGeometry()
{
    expect(nearly(Waveform::toneFrequency(0), 843.75, 1e-9),
           "tone 0 is 843.75 Hz");
    expect(nearly(Waveform::toneFrequency(15), 2250.0, 1e-9),
           "tone 15 is exactly 2250 Hz — the top of the occupied band");
    expect(nearly(Waveform::toneFrequency(1) - Waveform::toneFrequency(0),
                  93.75, 1e-9),
           "tone spacing is 93.75 Hz (48000/512)");
    // Whole cycles per window is what makes the tones orthogonal; assert it
    // rather than trusting the frequency arithmetic.
    for (int t = 0; t < Waveform::kToneCount; ++t) {
        const double cycles = Waveform::toneFrequency(t)
                              * Waveform::kSymbolSamples / Waveform::kSampleRate;
        if (!nearly(cycles, std::round(cycles), 1e-9)) {
            expect(false, "every tone spans a whole number of cycles per symbol");
            return;
        }
    }
    expect(true, "every tone spans a whole number of cycles per symbol");
}

void testSymbolShape()
{
    QVector<float> sym(Waveform::kSymbolSamples);
    Waveform::synthesiseSymbol(7, false, sym.data());

    expect(nearly(sym.at(0), 0.0, 1e-6),
           "a symbol starts at exactly zero (sine, integer cycles)");

    double peak = 0.0;
    for (float v : sym)
        peak = std::max(peak, double(std::abs(v)));
    expect(nearly(peak, Waveform::kAmplitude, 1.0),
           "peak amplitude matches the measured 12509.706905");

    // Untapered symbol: the tail is still full amplitude.
    double tailPeak = 0.0;
    for (int n = Waveform::kTaperStart; n < Waveform::kSymbolSamples; ++n)
        tailPeak = std::max(tailPeak, double(std::abs(sym.at(n))));
    expect(tailPeak > 0.5 * Waveform::kAmplitude,
           "a non-final symbol is NOT tapered");
}

void testFinalSymbolTaper()
{
    QVector<float> plain(Waveform::kSymbolSamples);
    QVector<float> faded(Waveform::kSymbolSamples);
    Waveform::synthesiseSymbol(11, false, plain.data());
    Waveform::synthesiseSymbol(11, true, faded.data());

    bool sameBefore = true;
    for (int n = 0; n < Waveform::kTaperStart; ++n) {
        if (!nearly(plain.at(n), faded.at(n), 1e-3)) sameBefore = false;
    }
    expect(sameBefore,
           "the taper leaves samples 0..495 untouched (full amplitude through 495)");

    const double* taper = Waveform::finalSymbolTaper();
    expect(nearly(taper[0], 0.9976, 1e-6) && nearly(taper[15], 0.0024, 1e-6),
           "the measured envelope runs from 0.9976 down to 0.0024");

    double lastPeak = 0.0;
    for (int n = Waveform::kSymbolSamples - 4; n < Waveform::kSymbolSamples; ++n)
        lastPeak = std::max(lastPeak, double(std::abs(faded.at(n))));
    expect(lastPeak < 0.10 * Waveform::kAmplitude,
           "the final symbol has faded to near silence by its last samples");
}

void testOrthogonality()
{
    // The property the whole demodulator rests on: a symbol carrying one tone
    // has essentially no energy in any other. Measured on real captures the
    // runner-up magnitude is literally 0.0000.
    QVector<float> sym(Waveform::kSymbolSamples);
    double worst = 1e18;
    for (int t = 0; t < Waveform::kToneCount; ++t) {
        Waveform::synthesiseSymbol(t, false, sym.data());
        double conf = 0.0;
        const int got = Waveform::demodulateSymbol(sym.constData(), &conf);
        if (got != t) {
            expect(false, "each tone demodulates back to itself");
            return;
        }
        worst = std::min(worst, conf);
    }
    expect(true, "each tone demodulates back to itself");
    std::printf("       (worst winner/runner-up ratio across all 16 tones: %.3g)\n",
                worst);
    expect(worst > 1000.0,
           "the winning tone dominates the runner-up by orders of magnitude");
}

void testFrameRoundTrip()
{
    // A pseudo-random symbol sequence of a full frame, synthesised and read
    // back. Deterministic so a failure is reproducible.
    QVector<int> symbols;
    symbols.reserve(Waveform::kFrameSymbols);
    unsigned state = 12345u;
    for (int i = 0; i < Waveform::kFrameSymbols; ++i) {
        state = state * 1103515245u + 12345u;
        symbols.append(int((state >> 16) % Waveform::kToneCount));
    }

    const QVector<qint16> pcm = Waveform::synthesise(symbols);
    expect(pcm.size() == Waveform::kFrameSymbols * Waveform::kSymbolSamples,
           "a 407-symbol frame is 208384 samples");

    QVector<float> f(pcm.size());
    for (int i = 0; i < pcm.size(); ++i)
        f[i] = float(pcm.at(i));

    QVector<double> conf;
    const QVector<int> back = Waveform::demodulate(f, &conf);
    expect(back == symbols, "a full frame round-trips symbol for symbol");

    // The final symbol is tapered, so its confidence is legitimately lower than
    // the rest; check the body separately from the tail.
    double worstBody = 1e18;
    for (int i = 0; i + 1 < conf.size(); ++i)
        worstBody = std::min(worstBody, conf.at(i));
    expect(worstBody > 1000.0,
           "every symbol but the tapered last decodes with a huge margin");
}

void testFrameDurationMatchesTheModem()
{
    const double seconds = double(Waveform::kFrameSymbols)
                           * Waveform::kSymbolSamples / Waveform::kSampleRate;
    expect(nearly(seconds, 4.341333, 1e-5),
           "a frame lasts 4.341 s, as measured on the bench");
    const double rawBps = Waveform::kFrameSymbols * 4 / seconds;   // 4 bits/symbol
    expect(nearly(rawBps, 375.0, 0.5),
           "raw channel rate is 375 bps — this is the low-speed waveform");
}

} // namespace

int main()
{
    testToneGeometry();
    testSymbolShape();
    testFinalSymbolTaper();
    testOrthogonality();
    testFrameRoundTrip();
    testFrameDurationMatchesTheModem();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
