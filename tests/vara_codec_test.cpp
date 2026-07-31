// Verifies the C++ codec against the committed model and, where it matters,
// against numbers measured on the bench.
//
// The strong test is not round-tripping — an encoder and decoder that agree with
// each other can both be wrong. It is that encoding a payload produces the
// symbols a REAL modem produced for that same payload, which is checked here
// against the model's own generator rows, and that a corrupted frame is detected
// rather than silently mis-decoded.

#include "core/VaraCodec.h"
#include "core/VaraWaveform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>

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

QString modelPath()
{
    // The test binary runs from the build directory; the model is committed
    // alongside the source.
    for (const char* candidate : {"data/vara/vara_model.json",
                                  "../data/vara/vara_model.json",
                                  "../../data/vara/vara_model.json"}) {
        if (QFile::exists(QString::fromLatin1(candidate)))
            return QString::fromLatin1(candidate);
    }
    return {};
}

QByteArray payloadWithBits(int lengthBytes, const QVector<int>& bits)
{
    QByteArray p(lengthBytes, '\0');
    for (int b : bits)
        p[b / 8] = char(p.at(b / 8) | (1 << (7 - (b % 8))));
    return p;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString path = modelPath();
    if (path.isEmpty()) {
        std::printf("[SKIP] model file not found; nothing to verify against\n");
        return 0;
    }

    Codec codec;
    QString err;
    expect(codec.load(path, &err), "the committed model loads");
    if (!codec.isLoaded()) {
        std::printf("       %s\n", qPrintable(err));
        return 1;
    }

    expect(codec.frameSymbols() == Waveform::kFrameSymbols,
           "the model's frame length agrees with the waveform's 407 symbols");
    expect(codec.payloadBlockBits() == 512,
           "the payload block is 512 bits, as measured");
    const QVector<int> lengths = codec.availablePayloadLengths();
    expect(lengths.contains(32),
           "a 32-byte baseline is present (the length the sweep used)");

    // ── Encode matches the model's own rows ─────────────────────────────
    // A single-bit payload must encode to exactly baseline XOR that row, which
    // is how the row was defined. This ties the encoder to the measurement.
    {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonObject code = root.value(QStringLiteral("code")).toObject();
        const QJsonArray base =
            code.value(QStringLiteral("baselines_by_payload_bytes")).toObject()
                .value(QStringLiteral("32")).toArray();
        const QJsonArray offs =
            root.value(QStringLiteral("scrambler")).toObject()
                .value(QStringLiteral("offsets")).toArray();
        const QJsonObject rows = code.value(QStringLiteral("rows")).toObject();

        int checked = 0;
        bool allMatch = true;
        for (int bit : {0, 1, 7, 31, 100, 255}) {
            const QJsonArray row = rows.value(QString::number(bit)).toArray();
            if (row.isEmpty())
                continue;
            const QVector<int> got = codec.encode(payloadWithBits(32, {bit}));
            if (got.size() != base.size()) { allMatch = false; break; }
            for (int i = 0; i < got.size(); ++i) {
                // d = baseline XOR row, then the transmitted tone is d + r.
                const int d = base.at(i).toInt() ^ row.at(i).toInt();
                const int want = (d + offs.at(i).toInt()) % 16;
                if (got.at(i) != want) { allMatch = false; break; }
            }
            ++checked;
            if (!allMatch) break;
        }
        expect(checked > 0 && allMatch,
               "a single-bit payload encodes to baseline XOR that generator row");
        std::printf("       (%d payload bits checked against the model)\n", checked);
    }

    // ── Round trip ──────────────────────────────────────────────────────
    const QByteArray zero(32, '\0');
    {
        const QVector<int> sym = codec.encode(zero);
        expect(sym.size() == Waveform::kFrameSymbols,
               "an all-zero payload encodes to a full frame");
        int residual = -1;
        const QByteArray back = codec.decode(sym, 32, &residual);
        expect(back == zero && residual == 0,
               "the all-zero payload round-trips with residual 0");
    }

    QByteArray mixed = payloadWithBits(32, {3, 17, 42, 99, 200, 255});
    {
        const QVector<int> sym = codec.encode(mixed);
        int residual = -1;
        const QByteArray back = codec.decode(sym, 32, &residual);
        expect(back == mixed && residual == 0,
               "a multi-bit payload round-trips with residual 0");
    }

    // ── Through the actual waveform ─────────────────────────────────────
    {
        const QVector<int> sym = codec.encode(mixed);
        const QVector<qint16> pcm = Waveform::synthesise(sym);
        QVector<float> f(pcm.size());
        for (int i = 0; i < pcm.size(); ++i) f[i] = float(pcm.at(i));
        const QVector<int> demod = Waveform::demodulate(f);
        int residual = -1;
        const QByteArray back = codec.decode(demod, 32, &residual);
        expect(back == mixed && residual == 0,
               "payload survives encode -> synthesise -> demodulate -> decode");
    }

    // ── Error detection ─────────────────────────────────────────────────
    {
        auto* rng = QRandomGenerator::global();
        int flagged = 0;
        const int trials = 20;
        for (int t = 0; t < trials; ++t) {
            QVector<int> sym = codec.encode(mixed);
            const int pos = int(rng->bounded(quint32(sym.size())));
            sym[pos] = (sym.at(pos) + 1 + int(rng->bounded(15u))) % 16;
            int residual = -1;
            codec.decode(sym, 32, &residual);
            if (residual > 0) ++flagged;
        }
        expect(flagged == trials,
               "a single corrupted symbol is always detected by a nonzero residual");
    }

    // ── Error correction ────────────────────────────────────────────────
    {
        auto* rng = QRandomGenerator::global();
        int corrected = 0;
        int wrongButConfident = 0;
        const int trials = 5;
        for (int t = 0; t < trials; ++t) {
            QVector<int> sym = codec.encode(mixed);
            for (int e = 0; e < 3; ++e) {
                const int pos = int(rng->bounded(quint32(sym.size())));
                sym[pos] = (sym.at(pos) + 1 + int(rng->bounded(15u))) % 16;
            }
            int residual = -1;
            const QByteArray back = codec.decodeCorrecting(sym, 32, 3000, &residual);
            if (back == mixed) ++corrected;
            else if (residual >= 0 && residual <= 12) ++wrongButConfident;
        }
        expect(corrected == trials,
               "three corrupted symbols are corrected by information-set decoding");
        // The dangerous failure is a WRONG payload returned with a low residual.
        expect(wrongButConfident == 0,
               "no wrong payload is ever returned with a low residual");
    }

    // ── Refusals rather than nonsense ───────────────────────────────────
    {
        QString why;
        const QVector<int> bad = codec.encode(QByteArray(7, '\0'), &why);
        expect(bad.isEmpty() && !why.isEmpty(),
               "a payload length with no baseline is refused, with a reason");
        const QByteArray shortFrame = codec.decode(QVector<int>(10, 0), 32);
        expect(shortFrame.isEmpty(),
               "a frame of the wrong length is refused rather than guessed at");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
