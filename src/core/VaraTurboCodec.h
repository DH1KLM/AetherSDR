#pragma once

#include <QByteArray>
#include <QVector>

#include <cstdint>

namespace AetherSDR {
namespace Vara {

// 3GPP LTE / VARA Rate 1/3 Turbo FEC Codec with QPP Interleaver.
//
// Clean-room specification: docs/vara-cleanroom-design.md §3.49.
//
// ENCODER STRUCTURE:
//   * Two parallel 8-state Recursive Systematic Convolutional (RSC) encoders.
//   * Feedback polynomial:    g0 = 013_8 (1 + D^2 + D^3)
//   * Feedforward polynomial:   g1 = 015_8 (1 + D + D^3)
//   * Interleaver: Quadratic Permutation Polynomial (QPP) Pi(i) = (f1*i + f2*i^2) mod K
//   * Trellis termination: 12 tail bits (3 systematic + 3 parity per encoder).
//   * Output: Systematic bits d_k^(0), Parity 1 bits d_k^(1), Parity 2 bits d_k^(2).
class TurboCodec {
public:
    struct QppParams {
        int kBits; // Block size in bits
        int f1;
        int f2;
    };

    // Standard QPP parameters for supported block sizes
    static QppParams getQppParams(int kBits);

    // Generate QPP interleaver index table of size K
    static QVector<int> generateQppInterleaver(int kBits);

    // Encode payload bits (size K) into rate 1/3 stream (3*K + 12 tail bits)
    static QVector<uint8_t> encodeRate13(const QVector<uint8_t>& inputBits);

    // Puncture rate 1/3 stream to target rate / target bit count
    static QVector<uint8_t> puncture(const QVector<uint8_t>& encodedBits,
                                     int targetBits);

    // Encode a byte array payload into punctured codeword bits for a given level
    static QVector<uint8_t> encodePayload(const QByteArray& payload,
                                          int targetBits);

    // Max-Log-MAP iterative Turbo decoder for rate 1/3 soft LLRs
    static QByteArray decodeMaxLogMap(const QVector<float>& channelLlrs,
                                      int kBits,
                                      int iterations = 4);

private:
    static void rscEncode(const QVector<uint8_t>& inBits,
                          QVector<uint8_t>& systematic,
                          QVector<uint8_t>& parity,
                          QVector<uint8_t>& tailBits);
};

} // namespace Vara
} // namespace AetherSDR
