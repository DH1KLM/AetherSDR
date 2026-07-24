#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QString>

namespace AetherSDR::hl2 {

class MetisClient;
class Hl2RxDsp;

// IRadioBackend implementation for the Hermes-Lite 2 (HPSDR Protocol 1, raw IQ).
// Owns a MetisClient (UDP wire) and an Hl2RxDsp (demod + panadapter) and maps the
// neutral seam verbs/signals onto them. This is the first backend that owns an
// engine-side DSP chain (RFC §5.5) rather than decoding a cooked stream.
//
// RX-only: capabilities().canTransmit is false, so the engine TX guard (RFC §6)
// denies keying; setKeying() is a no-op and nothing here can key the radio.
//
// Phase 1b runs the wire + DSP on this object's thread (iqBlockReady ->
// processIqBlock is a direct call); relocating the DSP onto its own thread is a
// later refinement once the data plane is wired through RadioModel.
class Hl2Backend : public IRadioBackend {
    Q_OBJECT

public:
    explicit Hl2Backend(QObject* parent = nullptr);
    ~Hl2Backend() override;

    RadioCapabilities capabilities() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setKeying(bool key) override;

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

private:
    void emitSliceState();   // sliceChanged(delta) from current freq/mode/filter
    void emitPanState();     // panCenterBandwidthChanged from freq + sample rate

    MetisClient* m_metis = nullptr;
    Hl2RxDsp* m_dsp = nullptr;
    bool m_connected = false;

    // Authoritative RX state (HL2 has no status wire echoing it back).
    double m_rxFreqHz = 10'000'000.0;
    int m_sampleRateHz = 48000;
    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 150;
    int m_filterHighHz = 3000;
    int m_lnaGainDb = 20;

    static constexpr int kSliceId = 0;
    static constexpr const char* kPanId = "hl2";
};

}  // namespace AetherSDR::hl2
