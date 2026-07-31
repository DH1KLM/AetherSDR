#include "core/VaraReceiver.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace Vara {

namespace {
constexpr int kSym = Waveform::kSymbolSamples;
constexpr int kFrame = Waveform::kFrameSymbols;

// Symbols used to score a candidate symbol alignment. Enough that noise
// averages out, few enough that the search stays cheap.
constexpr int kAlignSymbols = 8;
// Coarse search step, then a fine sweep either side of the winner.
constexpr int kCoarseStep = 16;

// Largest residual at which a decode is believed. A wrong solution fails about
// half of the 1628 parity equations; a correct one fails at most four per
// corrupted symbol, so this separates them by a wide margin while still
// accepting a badly damaged but recoverable frame.
constexpr int kMaxResidual = 64;

// Keep at most this much audio buffered. A frame is 208384 samples; allowing a
// little over two frames means a burst that straddles chunk boundaries is still
// contiguous, without letting the buffer grow without bound on a quiet channel.
constexpr int kMaxBuffer = 3 * kFrame * kSym;

double blockEnergy(const float* p, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += double(p[i]) * p[i];
    return acc / std::max(n, 1);
}
} // namespace

Receiver::Receiver(QObject* parent) : QObject(parent) {}

bool Receiver::loadModel(const QString& path, QString* error)
{
    return m_codec.load(path, error);
}

void Receiver::reset()
{
    m_buffer.clear();
    m_noiseFloor = 0.0;
    m_haveFloor = false;
}

void Receiver::feed(const QVector<float>& samples, int sampleRate)
{
    if (sampleRate != Waveform::kSampleRate) {
        // Refused, not resampled. The symbol grid is defined in samples, so
        // decoding at the wrong rate would produce confident nonsense rather
        // than an obvious failure. Warn once so a misconfigured audio path is
        // visible without flooding the log.
        if (!m_warnedRate) {
            m_warnedRate = true;
            emit unsupportedSampleRate(sampleRate);
        }
        return;
    }
    m_warnedRate = false;
    m_buffer.append(samples);
    scanForFrames();

    if (m_buffer.size() > kMaxBuffer)
        m_buffer.remove(0, m_buffer.size() - kMaxBuffer);
}

int Receiver::findSymbolAlignment(int start, double* bestConfidence) const
{
    const int need = kAlignSymbols * kSym;
    auto score = [&](int offset) -> double {
        if (offset < 0 || offset + need > m_buffer.size())
            return -1.0;
        // The WORST symbol in the window, not the median or the mean.
        //
        // Offsets a whole symbol apart share the same phase, so they all look
        // aligned on a per-symbol basis; what separates the true frame start
        // from one symbol early is that the early window contains a symbol of
        // noise. A median over eight symbols barely notices one bad symbol,
        // which is exactly how an earlier version locked 512 samples early and
        // decoded every symbol shifted by one. The minimum notices immediately.
        double worst = -1.0;
        for (int s = 0; s < kAlignSymbols; ++s) {
            double c = 0.0;
            Waveform::demodulateSymbol(m_buffer.constData() + offset + s * kSym, &c);
            if (worst < 0.0 || c < worst)
                worst = c;
        }
        return worst;
    };

    int bestOffset = -1;
    double best = -1.0;
    for (int o = start; o < start + kSym; o += kCoarseStep) {
        const double s = score(o);
        if (s > best) { best = s; bestOffset = o; }
    }
    if (bestOffset < 0) {
        if (bestConfidence) *bestConfidence = 0.0;
        return -1;
    }
    for (int o = std::max(start, bestOffset - kCoarseStep);
         o <= bestOffset + kCoarseStep; ++o) {
        const double s = score(o);
        if (s > best) { best = s; bestOffset = o; }
    }
    if (bestConfidence) *bestConfidence = best;
    return bestOffset;
}

void Receiver::scanForFrames()
{
    // Walk block by block looking for a rise above the noise floor, then try to
    // pull a whole frame out from there.
    int pos = 0;
    while (pos + kSym <= m_buffer.size()) {
        const double e = blockEnergy(m_buffer.constData() + pos, kSym);

        if (!m_haveFloor) {
            m_noiseFloor = e;
            m_haveFloor = true;
        }

        const bool loud = e > m_noiseFloor * m_threshold;
        if (!loud) {
            // Track the floor only on quiet blocks, so a long transmission
            // cannot drag the estimate up and hide whatever follows it.
            m_noiseFloor = 0.95 * m_noiseFloor + 0.05 * e;
            pos += kSym;
            continue;
        }

        // A frame needs this much audio after the burst starts; if it has not
        // all arrived yet, leave it buffered and come back on the next chunk.
        const int needed = (kFrame + 2) * kSym;
        if (pos + needed > m_buffer.size())
            return;

        // Energy rises in the block CONTAINING the frame start, so the start
        // lies in [pos, pos + kSym). Searching earlier than pos invites an
        // offset one symbol ahead of the signal, which is phase-identical and
        // therefore indistinguishable without the minimum-confidence metric.
        double conf = 0.0;
        const int aligned = findSymbolAlignment(pos, &conf);
        if (aligned < 0 || aligned + kFrame * kSym > m_buffer.size()) {
            pos += kSym;
            continue;
        }
        // A genuine DATA frame demodulates with an enormous margin. A modest
        // floor here rejects noise bursts and other modes without being so
        // strict that a weak but decodable frame is thrown away.
        if (conf < 20.0) {
            pos += kSym;
            continue;
        }

        QVector<float> frame(m_buffer.constData() + aligned,
                             m_buffer.constData() + aligned + kFrame * kSym);
        QVector<double> confs;
        const QVector<int> symbols = Waveform::demodulate(frame, &confs);
        std::sort(confs.begin(), confs.end());
        const double median = confs.isEmpty() ? 0.0 : confs.at(confs.size() / 2);
        emit frameDetected(symbols, median);

        if (m_codec.isLoaded() && m_payloadBytes > 0) {
            int residual = -1;
            QByteArray payload =
                m_codec.decode(symbols, m_payloadBytes, &residual);
            if (payload.isEmpty() || residual > 0) {
                // Not a clean codeword. Spend a little effort trying to correct
                // it before giving up; a receiver that only handles perfect
                // frames is not much use on HF.
                int corrected = -1;
                const QByteArray fixed =
                    m_codec.decodeCorrecting(symbols, m_payloadBytes, 1500, &corrected);
                if (!fixed.isEmpty() && corrected >= 0
                    && (residual < 0 || corrected < residual)) {
                    payload = fixed;
                    residual = corrected;
                }
            }
            // Only hand up a payload the residual actually supports. A wrong
            // solution fails roughly half of the 1628 equations while a right
            // one fails at most four per corrupted symbol, so a large residual
            // means the decode failed — reporting it as a payload would be
            // worse than reporting nothing. The frame itself was already
            // emitted via frameDetected, so nothing is hidden.
            if (!payload.isEmpty() && residual >= 0 && residual <= kMaxResidual)
                emit framePayload(payload, residual);
        }

        // Consume through the frame so its own body cannot re-trigger detection.
        const int consumed = aligned + kFrame * kSym;
        m_buffer.remove(0, consumed);
        pos = 0;
    }
}

} // namespace Vara
} // namespace AetherSDR
