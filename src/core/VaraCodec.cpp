#include "core/VaraCodec.h"

#include "core/VaraWaveform.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtNumeric>

#include <algorithm>

namespace AetherSDR {
namespace Vara {

namespace {
constexpr int kBitsPerSymbol = 4;

// Headroom over the number of unknowns when drawing an information-set subset.
// The equation set is heavily redundant — 375 of the 1628 equations are
// identically zero, being the pilot symbols and the bit planes that are
// payload-independent at symbols which are not — so a subset must be drawn only
// from the informative equations and must comfortably exceed the unknown count
// to reach full rank. Measured for 256 unknowns: 350 rows reaches full rank
// reliably where 300 never does (8 of 8 against 0 of 8).
constexpr int kSubsetHeadroom = 94;

// Residual at or below which a solution is accepted immediately. A wrong answer
// fails roughly half of the 1628 equations; the true answer fails at most four
// per corrupted symbol, so 64 separates them with a wide margin and still
// tolerates a badly damaged frame.
constexpr int kAcceptResidual = 64;
} // namespace

QVector<int> Codec::availablePayloadLengths() const
{
    QVector<int> out = m_baselines.keys().toVector();
    std::sort(out.begin(), out.end());
    return out;
}

bool Codec::load(const QString& modelPath, QString* error)
{
    m_loaded = false;
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };

    QFile f(modelPath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1").arg(modelPath));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull() || !doc.isObject())
        return fail(QStringLiteral("malformed model: %1").arg(perr.errorString()));
    const QJsonObject root = doc.object();

    const QJsonObject mod = root.value(QStringLiteral("modulation")).toObject();
    m_frameSymbols = mod.value(QStringLiteral("frame_symbols"))
                        .toInt(Waveform::kFrameSymbols);

    const QJsonArray offs =
        root.value(QStringLiteral("scrambler")).toObject()
            .value(QStringLiteral("offsets")).toArray();
    if (offs.size() != m_frameSymbols)
        return fail(QStringLiteral("model has %1 offsets, expected %2")
                        .arg(offs.size()).arg(m_frameSymbols));
    m_offsets.resize(m_frameSymbols);
    for (int i = 0; i < m_frameSymbols; ++i)
        m_offsets[i] = offs.at(i).toInt();

    const QJsonObject code = root.value(QStringLiteral("code")).toObject();
    m_blockBits = code.value(QStringLiteral("payload_block_bits")).toInt(512);

    m_baselines.clear();
    const QJsonObject bl =
        code.value(QStringLiteral("baselines_by_payload_bytes")).toObject();
    for (const QString& key : bl.keys()) {
        const QJsonArray a = bl.value(key).toArray();
        if (a.size() != m_frameSymbols)
            continue;
        QVector<int> v(m_frameSymbols);
        for (int i = 0; i < m_frameSymbols; ++i)
            v[i] = a.at(i).toInt();
        m_baselines.insert(key.toInt(), v);
    }
    if (m_baselines.isEmpty())
        return fail(QStringLiteral("model carries no usable baseline"));

    m_rows.clear();
    m_rowIndex.clear();
    const QJsonObject rows = code.value(QStringLiteral("rows")).toObject();
    for (const QString& key : rows.keys()) {
        const QJsonArray a = rows.value(key).toArray();
        if (a.size() != m_frameSymbols)
            continue;
        QVector<int> coded(m_frameSymbols);
        for (int i = 0; i < m_frameSymbols; ++i)
            coded[i] = a.at(i).toInt();
        m_rows.insert(key.toInt(), toBits(coded).b);
        m_rowIndex.append(key.toInt());
    }
    if (m_rows.isEmpty())
        return fail(QStringLiteral("model carries no generator rows"));
    std::sort(m_rowIndex.begin(), m_rowIndex.end());

    // Equations no row touches carry no information and make a random subset
    // rank-deficient, so they are excluded from sampling once, here.
    const int equations = m_frameSymbols * kBitsPerSymbol;
    m_informative.clear();
    for (int e = 0; e < equations; ++e) {
        for (int bit : m_rowIndex) {
            if (m_rows.value(bit).at(e)) {
                m_informative.append(e);
                break;
            }
        }
    }

    // Pack each equation's coefficients into 64-bit words. The elimination is
    // otherwise byte-per-coefficient, which is ~64x slower and — measured — too
    // slow to draw enough random subsets inside a decode budget: only about 2%
    // of subsets avoid the damaged equations when three symbols are corrupted,
    // so throughput of attempts is what decides whether correction succeeds.
    //
    // Columns follow m_rowIndex order, and the bits a shorter payload can vary
    // are a prefix of it, so a payload of k bits uses the low k columns with no
    // repacking.
    m_words = (m_rowIndex.size() + 63) / 64;
    m_packed.assign(equations, QVector<quint64>(m_words, 0));
    for (int e = 0; e < equations; ++e) {
        for (int c = 0; c < m_rowIndex.size(); ++c) {
            if (m_rows.value(m_rowIndex.at(c)).at(e))
                m_packed[e][c >> 6] |= (quint64(1) << (c & 63));
        }
    }

    m_loaded = true;
    return true;
}

Codec::Bits Codec::toBits(const QVector<int>& coded) const
{
    Bits out;
    out.b.resize(coded.size() * kBitsPerSymbol);
    for (int i = 0; i < coded.size(); ++i) {
        const int v = coded.at(i);
        for (int k = 0; k < kBitsPerSymbol; ++k)
            out.b[i * kBitsPerSymbol + k] =
                quint8((v >> (kBitsPerSymbol - 1 - k)) & 1);
    }
    return out;
}

QVector<int> Codec::descramble(const QVector<int>& symbols) const
{
    QVector<int> d(symbols.size());
    for (int i = 0; i < symbols.size() && i < m_offsets.size(); ++i)
        d[i] = ((symbols.at(i) - m_offsets.at(i)) % 16 + 16) % 16;
    return d;
}

QVector<int> Codec::encode(const QByteArray& payload, QString* error) const
{
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QVector<int>();
    };
    if (!m_loaded)
        return fail(QStringLiteral("no model loaded"));
    if (!m_baselines.contains(payload.size()))
        return fail(QStringLiteral("no baseline for a %1-byte payload")
                        .arg(payload.size()));

    const QVector<int> base = m_baselines.value(payload.size());
    QVector<quint8> d = toBits(base).b;

    // Payload bit 0 is the most significant bit of byte 0 — the convention the
    // rows were measured under. Getting this backwards produces a frame that
    // looks plausible and decodes to nonsense.
    for (int i = 0; i < payload.size() * 8; ++i) {
        const int byte = i / 8;
        const int bit = 7 - (i % 8);
        if (!((payload.at(byte) >> bit) & 1))
            continue;
        auto it = m_rows.constFind(i);
        if (it == m_rows.constEnd())
            return fail(QStringLiteral("no generator row for payload bit %1").arg(i));
        const QVector<quint8>& row = it.value();
        for (int e = 0; e < d.size(); ++e)
            d[e] ^= row.at(e);
    }

    // Reassemble symbols, then add the scrambler offset to get the tone.
    QVector<int> tones(m_frameSymbols);
    for (int i = 0; i < m_frameSymbols; ++i) {
        int v = 0;
        for (int k = 0; k < kBitsPerSymbol; ++k)
            v = (v << 1) | d.at(i * kBitsPerSymbol + k);
        tones[i] = (v + m_offsets.at(i)) % 16;
    }
    return tones;
}

QVector<int> Codec::rowsForPayload(int payloadBytes) const
{
    // A payload shorter than the 512-bit block exercises only its own bits; the
    // remainder of the block is fixed padding whose contribution is already
    // inside that length's baseline. Solving for all 512 unknowns when only 256
    // can vary makes every subset rank-deficient — which is exactly how the
    // first version of this failed, silently returning nothing.
    const int usable = payloadBytes * 8;
    QVector<int> out;
    out.reserve(usable);
    for (int bit : m_rowIndex) {
        if (bit < usable)
            out.append(bit);
    }
    return out;
}

int Codec::residualOf(const QVector<int>& bits, const QVector<quint8>& x,
                      const QVector<quint8>& delta) const
{
    const int k = bits.size();
    const int words = (k + 63) / 64;
    QVector<quint64> xw(words, 0);
    for (int c = 0; c < k; ++c) {
        if (x.at(c))
            xw[c >> 6] |= (quint64(1) << (c & 63));
    }
    int bad = 0;
    for (int e = 0; e < delta.size(); ++e) {
        const QVector<quint64>& row = m_packed.at(e);
        quint64 acc = 0;
        for (int w = 0; w < words; ++w) {
            quint64 v = row.at(w) & xw.at(w);
            if (w == words - 1 && (k & 63))
                v &= (quint64(1) << (k & 63)) - 1;
            acc ^= v;
        }
        if (quint8(qPopulationCount(acc) & 1) != delta.at(e))
            ++bad;
    }
    return bad;
}

bool Codec::solveSubset(const QVector<int>& bits, const QVector<int>& rows,
                        const QVector<quint8>& delta, QVector<quint8>* out) const
{
    const int k = bits.size();
    const int n = rows.size();
    if (n < k || k == 0)
        return false;
    const int words = (k + 63) / 64;

    // Augmented rows: coefficient words followed by one word holding the
    // right-hand side bit, so a single word-wise XOR eliminates everything.
    QVector<quint64> m(qsizetype(n) * (words + 1), 0);
    for (int i = 0; i < n; ++i) {
        const int e = rows.at(i);
        const QVector<quint64>& src = m_packed.at(e);
        quint64* dst = m.data() + qsizetype(i) * (words + 1);
        for (int w = 0; w < words; ++w)
            dst[w] = src.at(w);
        // Mask off any columns beyond this payload's bit count.
        if (k & 63)
            dst[words - 1] &= (quint64(1) << (k & 63)) - 1;
        dst[words] = delta.at(e) ? 1u : 0u;
    }

    int r = 0;
    for (int c = 0; c < k; ++c) {
        const int w = c >> 6;
        const quint64 bit = quint64(1) << (c & 63);
        int sel = -1;
        for (int i = r; i < n; ++i) {
            if (m.at(qsizetype(i) * (words + 1) + w) & bit) { sel = i; break; }
        }
        if (sel < 0)
            return false;                      // subset is rank-deficient
        if (sel != r) {
            for (int j = 0; j <= words; ++j)
                std::swap(m[qsizetype(r) * (words + 1) + j],
                          m[qsizetype(sel) * (words + 1) + j]);
        }
        const quint64* pivot = m.constData() + qsizetype(r) * (words + 1);
        for (int i = 0; i < n; ++i) {
            if (i == r) continue;
            quint64* row = m.data() + qsizetype(i) * (words + 1);
            if (!(row[w] & bit)) continue;
            for (int j = 0; j <= words; ++j)
                row[j] ^= pivot[j];
        }
        ++r;
    }

    out->assign(k, 0);
    for (int c = 0; c < k; ++c)
        (*out)[c] = quint8(m.at(qsizetype(c) * (words + 1) + words) & 1u);
    return true;
}

QByteArray Codec::decode(const QVector<int>& symbols, int payloadBytes,
                         int* residual) const
{
    if (residual) *residual = -1;
    if (!m_loaded || !m_baselines.contains(payloadBytes))
        return {};
    if (symbols.size() != m_frameSymbols)
        return {};

    const QVector<quint8> base = toBits(m_baselines.value(payloadBytes)).b;
    const QVector<quint8> got = toBits(descramble(symbols)).b;
    QVector<quint8> delta(base.size());
    for (int e = 0; e < delta.size(); ++e)
        delta[e] = base.at(e) ^ got.at(e);

    const QVector<int> bits = rowsForPayload(payloadBytes);
    QVector<quint8> x;
    if (!solveSubset(bits, m_informative, delta, &x))
        return {};
    if (residual)
        *residual = residualOf(bits, x, delta);

    QByteArray out(payloadBytes, '\0');
    for (int i = 0; i < bits.size(); ++i) {
        if (!x.at(i))
            continue;
        const int bitIndex = bits.at(i);
        const int byte = bitIndex / 8;
        if (byte >= payloadBytes)
            continue;
        out[byte] = char(out.at(byte) | (1 << (7 - (bitIndex % 8))));
    }
    return out;
}

QByteArray Codec::decodeCorrecting(const QVector<int>& symbols, int payloadBytes,
                                   int budgetMs, int* residual) const
{
    if (residual) *residual = -1;
    if (!m_loaded || !m_baselines.contains(payloadBytes))
        return {};
    if (symbols.size() != m_frameSymbols)
        return {};

    const QVector<quint8> base = toBits(m_baselines.value(payloadBytes)).b;
    const QVector<quint8> got = toBits(descramble(symbols)).b;
    QVector<quint8> delta(base.size());
    for (int e = 0; e < delta.size(); ++e)
        delta[e] = base.at(e) ^ got.at(e);

    const QVector<int> bits = rowsForPayload(payloadBytes);
    const int subsetRows =
        std::min<int>(bits.size() + kSubsetHeadroom, int(m_informative.size()));
    QVector<quint8> best;
    int bestResidual = -1;
    QElapsedTimer timer;
    timer.start();
    auto* rng = QRandomGenerator::global();

    while (timer.elapsed() < budgetMs) {
        // Draw a random subset of the informative equations by partial shuffle.
        QVector<int> pick = m_informative;
        const int take = std::min<int>(subsetRows, int(pick.size()));
        for (int i = 0; i < take; ++i) {
            const int j = i + int(rng->bounded(quint32(pick.size() - i)));
            pick.swapItemsAt(i, j);
        }
        pick.resize(take);

        QVector<quint8> x;
        if (!solveSubset(bits, pick, delta, &x))
            continue;
        const int res = residualOf(bits, x, delta);
        if (bestResidual < 0 || res < bestResidual) {
            bestResidual = res;
            best = x;
            // A wrong solution fails about half the equations; the true one
            // fails at most four per corrupted symbol. Anything this small can
            // only be the true answer, so stop rather than burn the budget.
            if (res <= kAcceptResidual)
                break;
        }
    }
    if (best.isEmpty())
        return {};
    if (residual)
        *residual = bestResidual;

    QByteArray out(payloadBytes, '\0');
    for (int i = 0; i < bits.size(); ++i) {
        if (!best.at(i))
            continue;
        const int bitIndex = bits.at(i);
        const int byte = bitIndex / 8;
        if (byte >= payloadBytes)
            continue;
        out[byte] = char(out.at(byte) | (1 << (7 - (bitIndex % 8))));
    }
    return out;
}

} // namespace Vara
} // namespace AetherSDR
