#include "core/VaraWaveform.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace Vara {

namespace {

// Measured envelope of the final symbol, samples 496..511. Recovered by least
// squares across captures whose final tones differ, so every sample is covered:
// a per-capture ratio is useless where that capture's tone crosses zero, and
// thresholding those out starves the samples nearest the end of the fade. An
// earlier median-of-ratios version with a magnitude threshold made the worst
// reconstruction error seven times larger.
constexpr double kTaper[Waveform::kSymbolSamples - Waveform::kTaperStart] = {
    0.9976, 0.9785, 0.9409, 0.8865, 0.8172, 0.7355, 0.6452, 0.5490,
    0.4510, 0.3548, 0.2643, 0.1828, 0.1135, 0.0590, 0.0215, 0.0024,
};

constexpr double kTwoPi = 6.283185307179586476925286766559;

} // namespace

const double* Waveform::finalSymbolTaper()
{
    return kTaper;
}

void Waveform::synthesiseSymbol(int tone, bool taper, float* out)
{
    if (!out)
        return;
    const int t = std::clamp(tone, 0, kToneCount - 1);
    // Whole cycles per window, so the phase argument is exact in the integer
    // bin index rather than accumulating error from a frequency in Hz.
    const int bin = kFirstDftBin + t;
    for (int n = 0; n < kSymbolSamples; ++n) {
        double v = kAmplitude * std::sin(kTwoPi * bin * n / double(kSymbolSamples));
        if (taper && n >= kTaperStart)
            v *= kTaper[n - kTaperStart];
        out[n] = static_cast<float>(v);
    }
}

QVector<qint16> Waveform::synthesise(const QVector<int>& symbols)
{
    QVector<qint16> pcm;
    if (symbols.isEmpty())
        return pcm;
    pcm.resize(symbols.size() * kSymbolSamples);
    QVector<float> scratch(kSymbolSamples);
    for (int i = 0; i < symbols.size(); ++i) {
        const bool last = (i == symbols.size() - 1);
        synthesiseSymbol(symbols.at(i), last, scratch.data());
        for (int n = 0; n < kSymbolSamples; ++n) {
            const double r = std::lround(scratch.at(n));
            pcm[i * kSymbolSamples + n] =
                static_cast<qint16>(std::clamp(r, -32768.0, 32767.0));
        }
    }
    return pcm;
}

int Waveform::demodulateSymbol(const float* samples, double* confidence)
{
    if (!samples) {
        if (confidence) *confidence = 0.0;
        return 0;
    }
    // A Goertzel-style single-bin DFT per candidate tone. Only 16 bins are of
    // interest out of 257, so a full FFT would be wasted work.
    double best = -1.0;
    double second = -1.0;
    int bestTone = 0;
    for (int t = 0; t < kToneCount; ++t) {
        const int bin = kFirstDftBin + t;
        double re = 0.0;
        double im = 0.0;
        for (int n = 0; n < kSymbolSamples; ++n) {
            const double phase = kTwoPi * bin * n / double(kSymbolSamples);
            re += samples[n] * std::cos(phase);
            im -= samples[n] * std::sin(phase);
        }
        const double mag = std::sqrt(re * re + im * im);
        if (mag > best) {
            second = best;
            best = mag;
            bestTone = t;
        } else if (mag > second) {
            second = mag;
        }
    }
    if (confidence)
        *confidence = best / std::max(second, 1e-12);
    return bestTone;
}

QVector<int> Waveform::demodulate(const QVector<float>& samples,
                                  QVector<double>* confidence)
{
    QVector<int> symbols;
    const int count = samples.size() / kSymbolSamples;
    symbols.reserve(count);
    if (confidence) {
        confidence->clear();
        confidence->reserve(count);
    }
    for (int i = 0; i < count; ++i) {
        double c = 0.0;
        symbols.append(demodulateSymbol(samples.constData() + i * kSymbolSamples, &c));
        if (confidence)
            confidence->append(c);
    }
    return symbols;
}

} // namespace Vara
} // namespace AetherSDR
