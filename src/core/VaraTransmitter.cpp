#include "core/VaraTransmitter.h"

#include "core/LogManager.h"

#include <cmath>

namespace AetherSDR {
namespace Vara {

Transmitter::Transmitter(QObject* parent)
    : QObject(parent)
{
}

void Transmitter::armForSession()
{
    // Deliberately not a setter taking a bool: arming must read as an act at
    // the call site, so a reviewer can grep armForSession() and see every
    // place transmission becomes possible. Callers must reach this from an
    // operator gesture.
    if (!m_txInhibited)
        return;
    m_txInhibited = false;
    qCInfo(lcVara) << "native transmitter ARMED for this session";
}

void Transmitter::disarm()
{
    if (m_txInhibited)
        return;
    m_txInhibited = true;
    qCInfo(lcVara) << "native transmitter disarmed";
}

bool Transmitter::loadModel(const QString& modelPath, QString* error)
{
    return m_codec.load(modelPath, error);
}

QVector<float> Transmitter::synthesizeMfskDataFrame(const QByteArray& payload)
{
    if (m_txInhibited) {
        emit transmitInhibited();
        return {};
    }

    if (!m_codec.isLoaded()) {
        emit synthesisError(QStringLiteral("MFSK model is not loaded"));
        return {};
    }

    QString err;
    QVector<int> symbols = m_codec.encode(payload, &err);
    if (symbols.isEmpty()) {
        emit synthesisError(err.isEmpty() ? QStringLiteral("MFSK encoding failed") : err);
        return {};
    }

    // Synthesise MFSK PCM int16 waveform and convert to normalized float (-1.0 .. +1.0)
    QVector<qint16> pcm = Waveform::synthesise(symbols);
    QVector<float> audio(pcm.size());
    constexpr float kScale = 1.0f / 32768.0f;
    for (int i = 0; i < pcm.size(); ++i) {
        audio[i] = static_cast<float>(pcm[i]) * kScale;
    }

    return packageBurstWithPtt(audio);
}

QVector<float> Transmitter::synthesizeOfdmDataFrame(const QByteArray& payload, int speedLevel)
{
    if (m_txInhibited) {
        emit transmitInhibited();
        return {};
    }

    int activeCarriers = OfdmWaveform::activeCarriers(m_bandwidth);
    int dataSymbols = OfdmWaveform::kDataSymbols;

    OfdmWaveform::Modulation mod = OfdmWaveform::Modulation::Bpsk;
    if (speedLevel >= 13) {
        mod = OfdmWaveform::Modulation::Qam64;
    } else if (speedLevel >= 10) {
        mod = OfdmWaveform::Modulation::Qam16;
    } else if (speedLevel >= 7) {
        mod = OfdmWaveform::Modulation::Qpsk;
    } else {
        mod = OfdmWaveform::Modulation::Bpsk;
    }

    int bitsPerCarrier = OfdmWaveform::bitsPerCarrier(mod);
    int totalPayloadBits = dataSymbols * activeCarriers * bitsPerCarrier;

    // Turbo encode payload to target bit count
    QVector<uint8_t> codedBits = TurboCodec::encodePayload(payload, totalPayloadBits);

    // Build 2D symbol grid (153 symbols total: 5 preamble + 148 data)
    QVector<QVector<std::complex<float>>> grid;
    grid.reserve(OfdmWaveform::kFrameSymbols);

    // 5 Preamble / Sync symbols (BPSK Frank-Zadoff / Pilot sequences)
    for (int p = 0; p < OfdmWaveform::kPreambleSymbols; ++p) {
        QVector<std::complex<float>> sym(activeCarriers);
        for (int c = 0; c < activeCarriers; ++c) {
            // Alternating BPSK pilot pattern
            float val = ((p + c) % 2 == 0) ? 1.0f : -1.0f;
            sym[c] = std::complex<float>(val, 0.0f);
        }
        grid.append(sym);
    }

    // 148 Data symbols
    int bitIdx = 0;
    for (int s = 0; s < dataSymbols; ++s) {
        QVector<std::complex<float>> sym(activeCarriers);
        for (int c = 0; c < activeCarriers; ++c) {
            uint8_t carrierBits = 0;
            for (int b = 0; b < bitsPerCarrier; ++b) {
                if (bitIdx < codedBits.size()) {
                    carrierBits |= (codedBits[bitIdx] & 1) << b;
                    ++bitIdx;
                }
            }
            sym[c] = OfdmWaveform::mapBitsToSymbol(carrierBits, mod);
        }
        grid.append(sym);
    }

    QVector<float> audio = OfdmWaveform::synthesizeFrame(grid, m_bandwidth);
    return packageBurstWithPtt(audio);
}

QVector<float> Transmitter::synthesizeDataFrame(const QByteArray& payload)
{
    if (m_speedLevel <= 4) {
        return synthesizeMfskDataFrame(payload);
    } else {
        return synthesizeOfdmDataFrame(payload, m_speedLevel);
    }
}

QVector<float> Transmitter::synthesizeAckBurst(bool ack, int ackType)
{
    if (m_txInhibited) {
        emit transmitInhibited();
        return {};
    }

    Q_UNUSED(ack);
    Q_UNUSED(ackType);

    // 32-symbol FSK control burst (tones 0..15)
    QVector<int> symbols(32, 4);
    for (int i = 0; i < 32; ++i) {
        symbols[i] = (i % 2 == 0) ? 2 : 6;
    }

    QVector<qint16> pcm = Waveform::synthesise(symbols);
    QVector<float> audio(pcm.size());
    constexpr float kScale = 1.0f / 32768.0f;
    for (int i = 0; i < pcm.size(); ++i) {
        audio[i] = static_cast<float>(pcm[i]) * kScale;
    }

    return packageBurstWithPtt(audio);
}

QVector<float> Transmitter::packageBurstWithPtt(const QVector<float>& burstAudio)
{
    // Public, and the single path all synthesised audio takes on its way out —
    // so it carries the guard too, not just the synthesis entry points. A
    // caller that reaches this directly with a pre-built buffer must not be
    // able to route around the arm, and any future PTT assertion belongs here,
    // below this check (property 4 in the header).
    if (m_txInhibited) {
        emit transmitInhibited();
        return {};
    }

    if (burstAudio.isEmpty()) {
        return {};
    }

    int leadSamples = (m_leadTimeMs * Waveform::kSampleRate) / 1000;
    int tailSamples = (m_tailTimeMs * Waveform::kSampleRate) / 1000;

    QVector<float> packaged(leadSamples + burstAudio.size() + tailSamples, 0.0f);

    // Copy audio into active payload slot
    for (int i = 0; i < burstAudio.size(); ++i) {
        packaged[leadSamples + i] = burstAudio[i];
    }

    return packaged;
}

} // namespace Vara
} // namespace AetherSDR
