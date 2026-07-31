#pragma once

#include <QByteArray>
#include <QVector>

#include <cstdint>

namespace AetherSDR {
namespace Vara {

// The VARA HF low-speed DATA waveform: modulation and demodulation.
//
// Recovered by measurement, not from any documentation, and every constant here
// was checked against real captures. See docs/vara-cleanroom-design.md for the
// provenance record; the summary is:
//
//   * 16-ary orthogonal CPFSK. Tone s is a pure sine sitting exactly on DFT bin
//     9 + s of a 512-point transform, i.e. (9 + s) * 93.75 Hz, spanning 843.75
//     to 2250 Hz.
//   * Symbols are 512 samples at 48 kHz, so each occupies a whole number of
//     cycles in its own window. That is why the tones are exactly orthogonal,
//     why every symbol starts and ends at zero, and why demodulation is
//     unambiguous — the winner-to-runner-up magnitude ratio measures around
//     186000 on clean captures, with the runner-up literally at zero.
//   * A DATA frame is 407 symbols. Only the FINAL symbol is tapered: full
//     amplitude through sample 495, then a fade to zero over the last 16.
//   * Amplitude 12509.706905, from a least-squares fit over 406 symbols.
//
// Resynthesis from demodulated symbols reproduces real captures to within 1 LSB
// across 14 sessions — the residual takes only the values -1, 0 and +1, which is
// the modem and this code rounding the same ideal waveform slightly differently.
//
// THIS IS THE LOW-SPEED WAVEFORM ONLY. VARA selects among several; a second one
// (1024-sample symbols, 32 tones on a 46.875 Hz grid) is characterised in the
// clean-room note but not implemented here, and the higher-rate levels a paid
// licence unlocks have never been observed.
class Waveform {
public:
    static constexpr int kSampleRate    = 48000;
    static constexpr int kSymbolSamples = 512;
    static constexpr int kToneCount     = 16;
    static constexpr int kFirstDftBin   = 9;     // tone 0 sits on this bin
    static constexpr int kFrameSymbols  = 407;
    static constexpr double kAmplitude  = 12509.706905;
    // First sample of the final symbol's fade-out.
    static constexpr int kTaperStart    = 496;

    // Tone index -> centre frequency in Hz. Exact, not approximate: the tone
    // spacing is 48000/512 = 93.75 Hz by construction.
    static constexpr double toneFrequency(int tone)
    {
        return (kFirstDftBin + tone) * (double(kSampleRate) / kSymbolSamples);
    }

    // Synthesise one symbol into `out` (which must have room for 512 samples).
    // `taper` applies the final-symbol fade; pass false for every symbol except
    // the last of a frame.
    static void synthesiseSymbol(int tone, bool taper, float* out);

    // Symbols -> 16-bit PCM. The last symbol is tapered automatically, matching
    // what the modem transmits.
    static QVector<qint16> synthesise(const QVector<int>& symbols);

    // Demodulate one symbol. Returns the tone index and, in `confidence`, the
    // ratio of the winning tone's magnitude to the runner-up's. A clean symbol
    // gives an enormous ratio; anything near 1 means the decision was a coin
    // flip and the caller should treat it as unreliable.
    static int demodulateSymbol(const float* samples, double* confidence = nullptr);

    // Demodulate a whole frame. `samples` must be a multiple of 512 long.
    static QVector<int> demodulate(const QVector<float>& samples,
                                   QVector<double>* confidence = nullptr);

    // The measured final-symbol envelope, 16 values covering samples 496..511.
    // No closed form fits it well — a cos^2 over 17 samples is the best tried,
    // at RMS 0.012 — so it is carried as measured data rather than a formula.
    static const double* finalSymbolTaper();
};

} // namespace Vara
} // namespace AetherSDR
