#include "core/VaraOfdmWaveform.h"

#include <cmath>
#include <cstring>
#include <numbers>

namespace AetherSDR {
namespace Vara {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Bit-reversal permutation for 1024 points (10 bits)
uint32_t bitReverse10(uint32_t x)
{
    x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
    x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
    x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
    x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));
    return ((x >> 16) | (x << 16)) >> 22; // 32 - 10 = 22
}

} // namespace

OfdmWaveform::SubcarrierRange OfdmWaveform::subcarrierRange(Bandwidth bw)
{
    switch (bw) {
    case Bandwidth::Bw500:
        return {16, 11}; // 16..26 (750.00 Hz - 1218.75 Hz)
    case Bandwidth::Bw2750:
        return {6, 57};  // 6..62 (281.25 Hz - 2906.25 Hz)
    case Bandwidth::Bw2300:
    default:
        return {8, 49};  // 8..56 (375.00 Hz - 2625.00 Hz)
    }
}

int OfdmWaveform::activeCarriers(Bandwidth bw)
{
    return subcarrierRange(bw).carrierCount;
}

int OfdmWaveform::bitsPerCarrier(Modulation mod)
{
    switch (mod) {
    case Modulation::Bpsk:  return 1;
    case Modulation::Qpsk:  return 2;
    case Modulation::Qam16: return 4;
    case Modulation::Qam64: return 6;
    }
    return 1;
}

std::complex<float> OfdmWaveform::mapBitsToSymbol(uint8_t bits, Modulation mod)
{
    constexpr float kInvSqrt2 = 0.70710678f;
    constexpr float kInvSqrt10 = 0.31622777f;
    constexpr float kInvSqrt42 = 0.15430335f;

    switch (mod) {
    case Modulation::Bpsk: {
        // 0 -> +1, 1 -> -1
        return (bits & 1) ? std::complex<float>(-1.0f, 0.0f) : std::complex<float>(1.0f, 0.0f);
    }
    case Modulation::Qpsk: {
        // Gray code: 00 -> (+1,+1), 01 -> (-1,+1), 11 -> (-1,-1), 10 -> (+1,-1)
        float i = (bits & 1) ? -kInvSqrt2 : kInvSqrt2;
        float q = (bits & 2) ? -kInvSqrt2 : kInvSqrt2;
        return std::complex<float>(i, q);
    }
    case Modulation::Qam16: {
        // 4 bits: b0,b1 -> I, b2,b3 -> Q using 2-bit Gray (-3, -1, 1, 3) * 1/sqrt(10)
        auto gray2 = [](uint8_t b) -> float {
            switch (b & 3) {
            case 0: return  3.0f * kInvSqrt10;
            case 1: return  1.0f * kInvSqrt10;
            case 2: return -3.0f * kInvSqrt10;
            case 3: return -1.0f * kInvSqrt10;
            }
            return 0.0f;
        };
        return std::complex<float>(gray2(bits & 3), gray2((bits >> 2) & 3));
    }
    case Modulation::Qam64: {
        // 6 bits: 3 bits I, 3 bits Q using 3-bit Gray (-7..+7) * 1/sqrt(42)
        auto gray3 = [](uint8_t b) -> float {
            switch (b & 7) {
            case 0: return  7.0f * kInvSqrt42;
            case 1: return  5.0f * kInvSqrt42;
            case 2: return  1.0f * kInvSqrt42;
            case 3: return  3.0f * kInvSqrt42;
            case 4: return -7.0f * kInvSqrt42;
            case 5: return -5.0f * kInvSqrt42;
            case 6: return -1.0f * kInvSqrt42;
            case 7: return -3.0f * kInvSqrt42;
            }
            return 0.0f;
        };
        return std::complex<float>(gray3(bits & 7), gray3((bits >> 3) & 7));
    }
    }
    return std::complex<float>(1.0f, 0.0f);
}

uint8_t OfdmWaveform::demapSymbolToBits(std::complex<float> sym, Modulation mod)
{
    float i = sym.real();
    float q = sym.imag();

    switch (mod) {
    case Modulation::Bpsk:
        return (i < 0.0f) ? 1 : 0;
    case Modulation::Qpsk: {
        uint8_t b = 0;
        if (i < 0.0f) b |= 1;
        if (q < 0.0f) b |= 2;
        return b;
    }
    case Modulation::Qam16: {
        constexpr float t1 = 2.0f * 0.31622777f;
        auto slice2 = [t1](float val) -> uint8_t {
            if (val > t1)  return 0; // +3
            if (val > 0.0f) return 1; // +1
            if (val > -t1) return 3; // -1
            return 2;                // -3
        };
        return slice2(i) | (slice2(q) << 2);
    }
    case Modulation::Qam64: {
        constexpr float t1 = 2.0f * 0.15430335f;
        constexpr float t2 = 4.0f * 0.15430335f;
        constexpr float t3 = 6.0f * 0.15430335f;
        auto slice3 = [t1, t2, t3](float val) -> uint8_t {
            if (val > t3)   return 0; // +7
            if (val > t2)   return 1; // +5
            if (val > t1)   return 3; // +3
            if (val > 0.0f)  return 2; // +1
            if (val > -t1)  return 6; // -1
            if (val > -t2)  return 7; // -3
            if (val > -t3)  return 5; // -5
            return 4;                 // -7
        };
        return slice3(i) | (slice3(q) << 3);
    }
    }
    return 0;
}

void OfdmWaveform::fft1024(const std::complex<float>* in, std::complex<float>* out)
{
    // In-place bit-reversal
    for (uint32_t i = 0; i < 1024; ++i) {
        out[bitReverse10(i)] = in[i];
    }

    // Cooley-Tukey Radix-2 FFT
    for (int len = 2; len <= 1024; len <<= 1) {
        double angle = -2.0 * kPi / len;
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < 1024; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                std::complex<float> u = out[i + j];
                std::complex<float> v = out[i + j + len / 2] * w;
                out[i + j] = u + v;
                out[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void OfdmWaveform::ifft1024(const std::complex<float>* in, std::complex<float>* out)
{
    // Conjugate input
    std::complex<float> temp[1024];
    for (int i = 0; i < 1024; ++i) {
        temp[i] = std::conj(in[i]);
    }

    // Forward FFT
    fft1024(temp, out);

    // Conjugate and scale output by 1/N
    constexpr float scale = 1.0f / 1024.0f;
    for (int i = 0; i < 1024; ++i) {
        out[i] = std::conj(out[i]) * scale;
    }
}

void OfdmWaveform::synthesizeSymbol(const QVector<std::complex<float>>& carrierValues,
                                    Bandwidth bw,
                                    float* outSamples)
{
    SubcarrierRange range = subcarrierRange(bw);
    std::complex<float> freqDomain[1024];
    std::memset(freqDomain, 0, sizeof(freqDomain));

    // Place active carriers into FFT frequency bins
    int n = std::min<int>(carrierValues.size(), range.carrierCount);
    for (int i = 0; i < n; ++i) {
        int bin = range.firstBin + i;
        if (bin < 512) {
            freqDomain[bin] = carrierValues[i];
            // Hermitian symmetry for real-valued time-domain signal
            freqDomain[1024 - bin] = std::conj(carrierValues[i]);
        }
    }

    std::complex<float> timeDomain[1024];
    ifft1024(freqDomain, timeDomain);

    // Copy cyclic prefix (last 128 samples to front)
    for (int i = 0; i < kCyclicPrefix; ++i) {
        outSamples[i] = timeDomain[1024 - kCyclicPrefix + i].real();
    }
    // Copy 1024 main IFFT samples
    for (int i = 0; i < kFftSize; ++i) {
        outSamples[kCyclicPrefix + i] = timeDomain[i].real();
    }
}

QVector<std::complex<float>> OfdmWaveform::demodulateSymbol(const float* inSamples,
                                                           Bandwidth bw)
{
    SubcarrierRange range = subcarrierRange(bw);
    QVector<std::complex<float>> result(range.carrierCount, std::complex<float>(0.0f, 0.0f));

    std::complex<float> timeDomain[1024];
    // Skip 128-sample cyclic prefix
    for (int i = 0; i < 1024; ++i) {
        timeDomain[i] = std::complex<float>(inSamples[kCyclicPrefix + i], 0.0f);
    }

    std::complex<float> freqDomain[1024];
    fft1024(timeDomain, freqDomain);

    for (int i = 0; i < range.carrierCount; ++i) {
        int bin = range.firstBin + i;
        if (bin < 512) {
            result[i] = freqDomain[bin];
        }
    }
    return result;
}

QVector<float> OfdmWaveform::synthesizeFrame(const QVector<QVector<std::complex<float>>>& symbolGrid,
                                             Bandwidth bw)
{
    int numSymbols = symbolGrid.size();
    QVector<float> frame(numSymbols * kSymbolSamples, 0.0f);

    for (int s = 0; s < numSymbols; ++s) {
        synthesizeSymbol(symbolGrid[s], bw, frame.data() + s * kSymbolSamples);
    }

    // Apply raised-cosine edge taper on frame boundaries (first and last 64 samples)
    constexpr int kTaperLen = 64;
    if (frame.size() >= 2 * kTaperLen) {
        for (int i = 0; i < kTaperLen; ++i) {
            float env = 0.5f * (1.0f - std::cos(static_cast<float>(kPi * i / kTaperLen)));
            frame[i] *= env;
            frame[frame.size() - 1 - i] *= env;
        }
    }

    return frame;
}

} // namespace Vara
} // namespace AetherSDR
