#pragma once

#include "core/VaraCodec.h"
#include "core/VaraWaveform.h"

#include <QByteArray>
#include <QObject>
#include <QVector>

namespace AetherSDR {
namespace Vara {

// Finds and decodes VARA DATA frames in a stream of received audio.
//
// The analysis tooling could split captures on exact digital zeros, because a
// modem's own output really does go to zero between bursts. Received audio does
// not: there is a noise floor, the start of a frame is not sample-aligned to
// anything, and the stream arrives in arbitrary-sized chunks. So this has to do
// the two jobs the offline tools never needed.
//
//   BURST DETECTION. Energy per 512-sample block against a running estimate of
//   the noise floor. The floor is tracked from blocks that are NOT part of a
//   burst, so a long transmission cannot drag it up and mask the next one.
//
//   SYMBOL TIMING. A frame's tones sit exactly on DFT bins of a 512-point
//   transform, which is true only when the window is aligned to the symbol
//   grid. Misaligned, the energy smears across bins and the winner-to-runner-up
//   ratio collapses. That ratio is therefore both the demodulator's confidence
//   measure and the timing metric: the correct offset is the one that maximises
//   it. The search is coarse-then-fine rather than all 512 offsets, since a
//   full search per burst is wasted work.
//
// RECEIVE ONLY. Nothing here transmits or can be made to.
class Receiver : public QObject {
    Q_OBJECT

public:
    explicit Receiver(QObject* parent = nullptr);

    // Load the recovered model. Without it, frames are still detected and
    // demodulated — which is useful on its own for monitoring — but cannot be
    // decoded to a payload.
    bool loadModel(const QString& path, QString* error = nullptr);
    bool hasModel() const { return m_codec.isLoaded(); }

    // Payload length to attempt decoding at. The frame itself does not announce
    // it, and the model carries a baseline per length, so the caller states
    // which to try. 0 disables decoding and reports symbols only.
    void setPayloadBytes(int bytes) { m_payloadBytes = bytes; }
    int payloadBytes() const { return m_payloadBytes; }

    // Feed received audio. Chunks may be any size. Anything other than 48 kHz
    // is rejected rather than silently mis-decoded — the symbol grid is defined
    // in samples, so a wrong rate produces confident nonsense.
    void feed(const QVector<float>& samples, int sampleRate);
    void reset();

    // Detection sensitivity, as a multiple of the tracked noise floor.
    void setDetectionThreshold(double ratio) { m_threshold = ratio; }

signals:
    // A frame was found and demodulated. `confidence` is the median
    // winner-to-runner-up ratio: on clean audio this is enormous, and a value
    // near 1 means the symbols are not trustworthy.
    void frameDetected(const QVector<int>& symbols, double confidence);
    // A frame was decoded. `residual` counts failed parity equations — zero
    // means the frame is exactly a codeword.
    void framePayload(const QByteArray& payload, int residual);
    // Audio arrived at a rate this cannot handle.
    void unsupportedSampleRate(int sampleRate);

private:
    int findSymbolAlignment(int start, double* bestConfidence) const;
    void scanForFrames();

    Codec m_codec;
    QVector<float> m_buffer;
    double m_noiseFloor{0.0};
    bool m_haveFloor{false};
    double m_threshold{4.0};
    int m_payloadBytes{32};
    bool m_warnedRate{false};
};

} // namespace Vara
} // namespace AetherSDR
