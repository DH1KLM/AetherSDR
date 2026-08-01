#include "core/VaraOfdmWaveform.h"

#include <cmath>
#include <complex>
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

void testFftRoundTrip()
{
    // Test 1024-point FFT -> IFFT round-trip identity
    std::complex<float> timeIn[1024];
    for (int i = 0; i < 1024; ++i) {
        timeIn[i] = std::complex<float>(std::cos(2.0 * 3.14159265 * 10 * i / 1024.0),
                                        std::sin(2.0 * 3.14159265 * 10 * i / 1024.0));
    }

    std::complex<float> freq[1024];
    OfdmWaveform::fft1024(timeIn, freq);

    std::complex<float> timeOut[1024];
    OfdmWaveform::ifft1024(freq, timeOut);

    float maxErr = 0.0f;
    for (int i = 0; i < 1024; ++i) {
        float err = std::abs(timeIn[i] - timeOut[i]);
        if (err > maxErr) maxErr = err;
    }
    expect(maxErr < 1e-4f, "FFT-IFFT 1024 round-trip precision < 1e-4");
}

void testConstellations()
{
    // BPSK
    expect(OfdmWaveform::mapBitsToSymbol(0, OfdmWaveform::Modulation::Bpsk).real() > 0.0f, "BPSK 0 maps to +1");
    expect(OfdmWaveform::mapBitsToSymbol(1, OfdmWaveform::Modulation::Bpsk).real() < 0.0f, "BPSK 1 maps to -1");
    expect(OfdmWaveform::demapSymbolToBits(std::complex<float>(1.0f, 0.0f), OfdmWaveform::Modulation::Bpsk) == 0, "BPSK demap +1 -> 0");
    expect(OfdmWaveform::demapSymbolToBits(std::complex<float>(-1.0f, 0.0f), OfdmWaveform::Modulation::Bpsk) == 1, "BPSK demap -1 -> 1");

    // QPSK all 4 quadrants
    for (uint8_t b = 0; b < 4; ++b) {
        std::complex<float> sym = OfdmWaveform::mapBitsToSymbol(b, OfdmWaveform::Modulation::Qpsk);
        uint8_t demap = OfdmWaveform::demapSymbolToBits(sym, OfdmWaveform::Modulation::Qpsk);
        expect(demap == b, "QPSK Gray mapping round-trip for symbol");
    }

    // 16-QAM all 16 points
    for (uint8_t b = 0; b < 16; ++b) {
        std::complex<float> sym = OfdmWaveform::mapBitsToSymbol(b, OfdmWaveform::Modulation::Qam16);
        uint8_t demap = OfdmWaveform::demapSymbolToBits(sym, OfdmWaveform::Modulation::Qam16);
        expect(demap == b, "16-QAM Gray mapping round-trip for symbol");
    }
}

void testSymbolSynthesisDemodulation()
{
    int carriers = OfdmWaveform::activeCarriers(Bandwidth::Bw2300);
    expect(carriers == 49, "2300 Hz mode has exactly 49 active subcarriers");

    QVector<std::complex<float>> inputSymbols(carriers);
    for (int i = 0; i < carriers; ++i) {
        inputSymbols[i] = (i % 2 == 0) ? std::complex<float>(0.707f, 0.707f)
                                       : std::complex<float>(-0.707f, 0.707f);
    }

    float samples[OfdmWaveform::kSymbolSamples];
    OfdmWaveform::synthesizeSymbol(inputSymbols, Bandwidth::Bw2300, samples);

    // Verify cyclic prefix continuity: samples[0..127] == samples[1024..1151]
    float cpErr = 0.0f;
    for (int i = 0; i < OfdmWaveform::kCyclicPrefix; ++i) {
        float diff = std::abs(samples[i] - samples[1024 + i]);
        if (diff > cpErr) cpErr = diff;
    }
    expect(cpErr < 1e-5f, "Cyclic prefix matches trailing symbol samples exactly");

    // Demodulate symbol without noise
    QVector<std::complex<float>> recovered = OfdmWaveform::demodulateSymbol(samples, Bandwidth::Bw2300);
    expect(recovered.size() == carriers, "Demodulated carrier count matches active carriers");

    float demodErr = 0.0f;
    for (int i = 0; i < carriers; ++i) {
        // Output from forward FFT is unnormalized (factor of N=1024), scaled by IFFT (1/1024) -> factor of 1.0f on real part
        float diff = std::abs(recovered[i].real() - inputSymbols[i].real());
        if (diff > demodErr) demodErr = diff;
    }
    expect(demodErr < 1e-4f, "OFDM modulation-demodulation carrier recovery error < 1e-4");
}

} // namespace

int main()
{
    std::printf("=== Running vara_ofdm_waveform_test ===\n");
    testFftRoundTrip();
    testConstellations();
    testSymbolSynthesisDemodulation();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
