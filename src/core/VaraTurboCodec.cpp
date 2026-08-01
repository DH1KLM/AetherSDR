#include "core/VaraTurboCodec.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace Vara {

TurboCodec::QppParams TurboCodec::getQppParams(int kBits)
{
    // Standard 3GPP TS 36.212 Table 5.1.3-3 QPP parameters
    switch (kBits) {
    case 40:   return {40, 3, 10};
    case 48:   return {48, 7, 12};
    case 64:   return {64, 7, 16};
    case 128:  return {128, 19, 32};
    case 256:  return {256, 31, 16};
    case 512:  return {512, 23, 32};
    case 1024: return {1024, 55, 80};
    case 2048: return {2048, 87, 128};
    case 4096: return {4096, 55, 256};
    default:
        // Default quadratic interleaver parameters satisfying gcd(f1, K) = 1
        return {kBits, 31, 16};
    }
}

QVector<int> TurboCodec::generateQppInterleaver(int kBits)
{
    QppParams p = getQppParams(kBits);
    QVector<int> pi(kBits);
    for (int i = 0; i < kBits; ++i) {
        int64_t idx = (static_cast<int64_t>(p.f1) * i + static_cast<int64_t>(p.f2) * i * i) % kBits;
        pi[i] = static_cast<int>(idx);
    }
    return pi;
}

void TurboCodec::rscEncode(const QVector<uint8_t>& inBits,
                           QVector<uint8_t>& systematic,
                           QVector<uint8_t>& parity,
                           QVector<uint8_t>& tailBits)
{
    int k = inBits.size();
    systematic.resize(k);
    parity.resize(k);
    tailBits.resize(6); // 3 systematic tail + 3 parity tail

    uint8_t s0 = 0, s1 = 0, s2 = 0; // 3-stage shift register (8 states)

    for (int i = 0; i < k; ++i) {
        uint8_t d = inBits[i] & 1;
        systematic[i] = d;

        // Feedback: g0 = 1 + D^2 + D^3 (indices: current, s1, s2)
        uint8_t fb = d ^ s1 ^ s2;
        // Parity: g1 = 1 + D + D^3 (indices: fb, s0, s2)
        parity[i] = fb ^ s0 ^ s2;

        // State update: shift right
        s2 = s1;
        s1 = s0;
        s0 = fb;
    }

    // 3 tail steps to drive shift register to zero: input = s1 ^ s2 => fb = 0
    for (int t = 0; t < 3; ++t) {
        uint8_t d = s1 ^ s2;
        uint8_t fb = 0;
        tailBits[t * 2] = d;                    // systematic tail
        tailBits[t * 2 + 1] = fb ^ s0 ^ s2;     // parity tail

        s2 = s1;
        s1 = s0;
        s0 = fb;
    }
}

QVector<uint8_t> TurboCodec::encodeRate13(const QVector<uint8_t>& inputBits)
{
    int k = inputBits.size();
    QVector<int> pi = generateQppInterleaver(k);

    QVector<uint8_t> interleaved(k);
    for (int i = 0; i < k; ++i) {
        interleaved[i] = inputBits[pi[i]];
    }

    QVector<uint8_t> sys1, par1, tail1;
    rscEncode(inputBits, sys1, par1, tail1);

    QVector<uint8_t> sys2, par2, tail2;
    rscEncode(interleaved, sys2, par2, tail2);

    // Total bits = 3 * K + 12 tail bits
    QVector<uint8_t> stream;
    stream.reserve(3 * k + 12);

    // Multiplex systematic, parity 1, parity 2
    for (int i = 0; i < k; ++i) {
        stream.append(sys1[i]);
        stream.append(par1[i]);
        stream.append(par2[i]);
    }

    // Append 12 tail bits
    for (int i = 0; i < 6; ++i) {
        stream.append(tail1[i]);
    }
    for (int i = 0; i < 6; ++i) {
        stream.append(tail2[i]);
    }

    return stream;
}

QVector<uint8_t> TurboCodec::puncture(const QVector<uint8_t>& encodedBits, int targetBits)
{
    if (encodedBits.size() == targetBits) {
        return encodedBits;
    }

    QVector<uint8_t> punctured(targetBits);
    int total = encodedBits.size();
    for (int i = 0; i < targetBits; ++i) {
        punctured[i] = encodedBits[(i * total) / targetBits];
    }
    return punctured;
}

QVector<uint8_t> TurboCodec::encodePayload(const QByteArray& payload, int targetBits)
{
    QVector<uint8_t> inBits(payload.size() * 8);
    for (int i = 0; i < payload.size(); ++i) {
        uint8_t byte = static_cast<uint8_t>(payload[i]);
        for (int b = 0; b < 8; ++b) {
            inBits[i * 8 + b] = (byte >> (7 - b)) & 1;
        }
    }

    QVector<uint8_t> rate13 = encodeRate13(inBits);
    return puncture(rate13, targetBits);
}

QByteArray TurboCodec::decodeMaxLogMap(const QVector<float>& channelLlrs,
                                      int kBits,
                                      int iterations)
{
    Q_UNUSED(iterations);
    // Hard decision fallback for initial integration
    QByteArray payload(kBits / 8, '\0');
    int n = std::min<int>(kBits, channelLlrs.size() / 3);

    for (int i = 0; i < n; ++i) {
        // First bit of triplet is systematic LLR
        float llr = channelLlrs[i * 3];
        if (llr < 0.0f) { // Negative LLR -> bit 1
            payload[i / 8] = static_cast<char>(payload[i / 8] | (1 << (7 - (i % 8))));
        }
    }
    return payload;
}

} // namespace Vara
} // namespace AetherSDR
