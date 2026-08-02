#pragma once

#include "core/VaraCodec.h"
#include "core/VaraOfdmWaveform.h"
#include "core/VaraProtocol.h"
#include "core/VaraTurboCodec.h"
#include "core/VaraWaveform.h"

#include <QByteArray>
#include <QObject>
#include <QVector>

namespace AetherSDR {
namespace Vara {

// Clean-room Native VARA HF Audio Transmitter.
//
// Converts payload data and control commands into 48 kHz float audio buffers
// across all supported modulation levels (MFSK Levels 1–4 and OFDM Levels 5–14).
//
// Manages sample-accurate PTT lead-in/lead-out timing and enforces the Fail-Closed
// Transmit Inhibit safety policy (Principle VI).
class Transmitter : public QObject {
    Q_OBJECT

public:
    explicit Transmitter(QObject* parent = nullptr);

    // Load MFSK generator model
    bool loadModel(const QString& modelPath, QString* error = nullptr);
    bool hasModel() const { return m_codec.isLoaded(); }

    // Bandwidth selection
    void setBandwidth(Bandwidth bw) { m_bandwidth = bw; }
    Bandwidth bandwidth() const { return m_bandwidth; }

    // Modulation level (1..14)
    void setSpeedLevel(int level) { m_speedLevel = level; }
    int speedLevel() const { return m_speedLevel; }

    // Transmit Inhibit guard ("Fail-Closed"). Defaults to INHIBITED: this class
    // synthesises RF audio and asserts PTT, so an instance nobody has
    // deliberately armed must not be able to key. Constitution VI — "if the
    // operator's intent to transmit is not unambiguous, it does not key."
    void setTransmitInhibited(bool inhibited) { m_txInhibited = inhibited; }
    bool isTransmitInhibited() const { return m_txInhibited; }

    // Lead-in and tail delay in milliseconds
    void setPttLeadTimeMs(int ms) { m_leadTimeMs = ms; }
    int pttLeadTimeMs() const { return m_leadTimeMs; }

    void setPttTailTimeMs(int ms) { m_tailTimeMs = ms; }
    int pttTailTimeMs() const { return m_tailTimeMs; }

    // ── Frame Synthesis Methods ─────────────────────────────────────────

    // Synthesise a low-speed MFSK DATA frame (Levels 1–4)
    QVector<float> synthesizeMfskDataFrame(const QByteArray& payload);

    // Synthesise a high-speed OFDM DATA frame (Levels 5–14)
    QVector<float> synthesizeOfdmDataFrame(const QByteArray& payload, int speedLevel);

    // Synthesise an adaptive data frame matching current speed level
    QVector<float> synthesizeDataFrame(const QByteArray& payload);

    // Synthesise an ARQ ACK / Control burst
    QVector<float> synthesizeAckBurst(bool ack, int ackType = 1);

    // Package an audio buffer with lead-in and tail silence
    QVector<float> packageBurstWithPtt(const QVector<float>& burstAudio);

signals:
    // PTT state change requested
    void pttAsserted(bool keyed);

    // Audio block ready for transmission to soundcard / DAX / TCI
    void audioReady(const QVector<float>& samples);

    // Transmission was blocked due to active TX Inhibit
    void transmitInhibited();

    // Error occurred during frame synthesis
    void synthesisError(const QString& reason);

private:
    Codec m_codec;
    Bandwidth m_bandwidth{Bandwidth::Bw2300};
    int m_speedLevel{1};
    bool m_txInhibited{true};
    int m_leadTimeMs{50};
    int m_tailTimeMs{20};
};

} // namespace Vara
} // namespace AetherSDR
