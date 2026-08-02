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

    // ── Transmit arming (Constitution VI) ───────────────────────────────
    //
    // Fail-closed: a Transmitter nobody has deliberately armed cannot key.
    // This class synthesises RF audio and is the last thing between a decision
    // and an emission on the operator's licence, so the default is inhibited
    // and arming is an explicit, auditable act.
    //
    // FOUR PROPERTIES, all required by the #4645 triage:
    //
    //   1. Defaults to inhibited and fails closed — m_txInhibited{true}.
    //   2. Armed only by a deliberate operator action. armForSession() exists
    //      to make that call site greppable: it must be reached from an
    //      operator gesture, never from a state change, a timer, or a frame
    //      arriving on the wire.
    //   3. The armed state does NOT persist. Nothing here reads or writes
    //      AppSettings, and SessionController disarms on every transition to
    //      Disconnected — so a crash-restart or a reconnect comes back
    //      inhibited rather than silently re-arming an auto-answering
    //      transmitter.
    //   4. It gates where the transmission is actually produced, not in the
    //      UI. Every synthesis entry point checks it AND so does
    //      packageBurstWithPtt(), which is the single choke point all audio
    //      passes through on its way out. Any future PTT assertion must be
    //      added there, behind that check.
    void armForSession();
    void disarm();

    void setTransmitInhibited(bool inhibited) { m_txInhibited = inhibited; }
    bool isTransmitInhibited() const { return m_txInhibited; }
    bool isArmed() const { return !m_txInhibited; }

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
