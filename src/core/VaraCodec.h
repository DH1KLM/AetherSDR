#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

namespace AetherSDR {
namespace Vara {

// The VARA HF low-speed code: payload bits to DATA-frame symbols and back.
//
// THE MODEL. Recovered by measurement (docs/vara-cleanroom-design.md §3.27
// onward). The transmitted tone is
//
//     s[p] = ( d[p] + r[p] ) mod 16
//
// where `d` is a coded symbol that is GF(2)-linear in the payload across all
// four of its bit planes, and `r` is a fixed 407-value offset that does not
// depend on the payload — an ADDITIVE mod-16 scrambler. Adding rather than
// XORing is what made the top bits look nonlinear: the carry chain gives the
// four planes algebraic degrees 1, 1, 2 and 3, which is exactly what was
// measured before the model was found.
//
// Because `d` is linear, a baseline plus one generator row per payload bit
// determines everything:
//
//     d(payload) = d(0) XOR ( XOR of row_i for every set payload bit i )
//
// The payload block is 512 bits. `r` is only ever determinable modulo 8 —
// subtracting 8 flips just bit 3 and four terms cancel it — which is harmless,
// since the ambiguity is absorbed into the baseline and leaves the rows intact.
//
// PROVENANCE OF THE NUMBERS. The model file is data/vara/vara_model.json,
// produced by tools/vara_model_export.py from bench captures and committed so
// this does not depend on repeating nine hours of measurement. That exporter
// and the rest of the recovery tooling live on the `research/vara-cleanroom`
// branch rather than in the product tree — the model is the only artifact the
// build and tests need. A real VARA
// modem decoded a frame synthesised from it for a payload it had never
// transmitted, which is the end-to-end proof that the model is right.
//
// SCOPE. Low-speed waveform only, and no ARQ: this converts payload to symbols
// and back. Control-frame generation and the link state machine are not here.
class Codec {
public:
    // Load the recovered model. Returns false and sets `error` if the file is
    // missing, malformed, or does not carry a usable set of generator rows.
    bool load(const QString& modelPath, QString* error = nullptr);
    bool isLoaded() const { return m_loaded; }

    int frameSymbols() const { return m_frameSymbols; }
    int payloadBlockBits() const { return m_blockBits; }
    // Payload lengths the model carries a baseline for, in bytes.
    QVector<int> availablePayloadLengths() const;

    // Encode a payload into transmitted tone indices. `payload` is taken
    // most-significant-bit first, matching the convention the rows were
    // measured under. Returns an empty vector if the length has no baseline or
    // a set bit has no generator row.
    QVector<int> encode(const QByteArray& payload, QString* error = nullptr) const;

    // Decode transmitted tones back to a payload.
    //
    // `residual` reports how many of the 1628 linear equations the answer fails.
    // Zero means the frame is exactly a codeword. The system is heavily
    // overdetermined — 512 unknowns against 1628 equations — so a corrupted
    // frame leaves a nonzero residual instead of silently yielding a wrong
    // payload. That redundancy is the error detection and it comes free.
    QByteArray decode(const QVector<int>& symbols, int payloadBytes,
                      int* residual = nullptr) const;

    // Error-CORRECTING decode by information-set decoding: solve from random
    // subsets of the equations and keep the solution with the smallest residual
    // over all of them.
    //
    // The criterion is minimum residual, NOT zero. With e corrupted symbols the
    // received word is not a codeword, so nothing satisfies every equation; the
    // true payload still fails at most 4e of them while a wrong one fails about
    // half. Demanding zero fails at even a single corrupted symbol.
    //
    // Measured with a 2-second budget on 407-symbol frames: correct at 10/10
    // trials with 1, 3 and 5 corrupted symbols, 6/6 at 8 and 10, 5/6 at 12, and
    // 3/6 at 14 and 16. At every level there were ZERO cases of a wrong payload
    // returned with a low residual, so a failure is a refusal rather than a lie.
    QByteArray decodeCorrecting(const QVector<int>& symbols, int payloadBytes,
                                int budgetMs = 2000, int* residual = nullptr) const;

private:
    struct Bits {
        QVector<quint8> b;   // 0/1 per bit, 4 per symbol, MSB first
    };
    Bits toBits(const QVector<int>& coded) const;
    QVector<int> descramble(const QVector<int>& symbols) const;
    // The payload bits that can actually vary at a given length; the rest of
    // the 512-bit block is padding folded into that length's baseline.
    QVector<int> rowsForPayload(int payloadBytes) const;
    bool solveSubset(const QVector<int>& bits, const QVector<int>& rows,
                     const QVector<quint8>& delta, QVector<quint8>* out) const;
    int residualOf(const QVector<int>& bits, const QVector<quint8>& x,
                   const QVector<quint8>& delta) const;

    bool m_loaded{false};
    int m_frameSymbols{0};
    int m_blockBits{0};
    QVector<int> m_offsets;                       // r, per symbol position
    QHash<int, QVector<int>> m_baselines;         // payload bytes -> d(0)
    QHash<int, QVector<quint8>> m_rows;           // payload bit -> row, as bits
    QVector<int> m_rowIndex;                      // sorted payload-bit indices
    QVector<int> m_informative;                   // equations touched by any row
    QVector<QVector<quint64>> m_packed;           // coefficients, 64 bits per word
    int m_words{0};
};

} // namespace Vara
} // namespace AetherSDR
