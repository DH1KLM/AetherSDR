#pragma once

#include "core/VaraProtocol.h"

#include <QByteArray>
#include <QVector>

#include <complex>
#include <cstdint>

namespace AetherSDR {
namespace Vara {

// High-speed OFDM physical layer modulation and demodulation for Levels 5–14.
//
// Recovered through clean-room RF/audio analysis (docs/vara-cleanroom-design.md §3.48).
//
// PARAMETERS:
//   * Sample rate: 48,000 Hz.
//   * FFT size: 1024 points (subcarrier spacing 46.875 Hz).
//   * Cyclic prefix: 128 samples (2.667 ms).
//   * Symbol duration: 1152 samples (24.0 ms, 41.667 baud).
//   * Subcarrier allocations:
//       - 500 Hz:  11 carriers (bins 16..26, 750.00 Hz to 1218.75 Hz)
//       - 2300 Hz: 49 carriers (bins 8..56,  375.00 Hz to 2625.00 Hz)
//       - 2750 Hz: 57 carriers (bins 6..62,  281.25 Hz to 2906.25 Hz)
//   * Modulations: BPSK, QPSK, 16-QAM, 64-QAM (Gray-coded).
//   * Frame structure: 153 symbols total (5 preamble/sync + 148 payload) = 176,256 samples (3.672 s).
class OfdmWaveform {
public:
    static constexpr int kSampleRate       = 48000;
    static constexpr int kFftSize          = 1024;
    static constexpr int kCyclicPrefix     = 128;
    static constexpr int kSymbolSamples    = kFftSize + kCyclicPrefix; // 1152 samples
    static constexpr double kSubcarrierSpacing = 46.875; // 48000 / 1024 Hz
    static constexpr int kFrameSymbols     = 153;
    static constexpr int kPreambleSymbols  = 5;
    static constexpr int kDataSymbols      = 148;
    static constexpr double kTargetRms     = 0.25; // Normalised transmission RMS

    enum class Modulation {
        Bpsk,
        Qpsk,
        Qam16,
        Qam64
    };

    struct SubcarrierRange {
        int firstBin;
        int carrierCount;
    };

    // Active subcarrier bin range for a given bandwidth
    static SubcarrierRange subcarrierRange(Bandwidth bw);

    // Number of active subcarriers
    static int activeCarriers(Bandwidth bw);

    // Bits per symbol per carrier
    static int bitsPerCarrier(Modulation mod);

    // Constellation mapping (Gray coding)
    static std::complex<float> mapBitsToSymbol(uint8_t bits, Modulation mod);

    // Constellation demapping (hard decision)
    static uint8_t demapSymbolToBits(std::complex<float> sym, Modulation mod);

    // Synthesise one 1152-sample OFDM symbol into `outSamples` (must hold >= 1152 floats)
    static void synthesizeSymbol(const QVector<std::complex<float>>& carrierValues,
                                 Bandwidth bw,
                                 float* outSamples);

    // Demodulate one 1152-sample OFDM symbol from `inSamples`
    static QVector<std::complex<float>> demodulateSymbol(const float* inSamples,
                                                         Bandwidth bw);

    // Synthesise a complete OFDM frame from a 2D symbol grid (symbols x carriers)
    static QVector<float> synthesizeFrame(const QVector<QVector<std::complex<float>>>& symbolGrid,
                                          Bandwidth bw);

    // Fast self-contained radix-2 IFFT and FFT (size must be power of 2, e.g. 1024)
    static void ifft1024(const std::complex<float>* in, std::complex<float>* out);
    static void fft1024(const std::complex<float>* in, std::complex<float>* out);
};

} // namespace Vara
} // namespace AetherSDR
