#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

#include "core/backends/IRadioBackend.h"
#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomMeters.h"
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/IcomScope.h"
#include "core/backends/icom/IcomSession.h"

class QTimer;

namespace AetherSDR {
// Lives in AetherSDR, not the global namespace — declaring it globally makes
// the member below an incomplete type that only fails at the point of use.
class Resampler;
}  // namespace AetherSDR

namespace AetherSDR::icom {

// The IRadioBackend implementor for Icom networked radios.
//
// Everything below this class is transport and codec; everything here is
// translation into AetherSDR's neutral seam. The split is what lets the whole
// session be tested against a fake radio without constructing a backend, and
// what will let a future local-serial transport reuse the same translation.
//
// THREE THINGS THIS BACKEND IS NOT, stated up front because each one is a
// tempting wrong assumption:
//
//   * It does NOT modulate on the host. The radio owns the modulator, so
//     hostModulates is false and submitTxAudio ships PCM rather than baseband
//     IQ. (Contrast the HL2, where the opposite is true of both.)
//   * It does NOT produce IQ, and cannot. No networked Icom emits samples.
//     hasDaxStreams is false and there is no IQ path to add later.
//   * It does NOT own the radio's operating state. An Icom remembers its own
//     frequency, mode and filter across power cycles and reports them on
//     request, so clientSettingsDomains is EMPTY and this backend must never
//     push a restored state (Constitution II/III).
class IcomCivBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit IcomCivBackend(QObject* parent = nullptr);
    ~IcomCivBackend() override;

    // ---- identity & capability ----
    [[nodiscard]] RadioCapabilities capabilities() const override;

    // TRUE. Demodulated audio arrives over the seam, not through a Flex
    // PanadapterStream — this is the gate the RX-audio wiring keys off.
    [[nodiscard]] bool ownsRxAudio() const override { return true; }

    // ---- lifecycle ----
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    [[nodiscard]] bool isConnected() const override;

    // ---- intents DOWN ----
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setKeying(bool key) override;
    void setTune(bool on, int tunePowerPercent = -1) override;
    void setTxPower(int percent) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz) override;
    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg = {}) override;

    // ---- diagnostics ----
    [[nodiscard]] HealthSnapshot healthSnapshot() const override;
    [[nodiscard]] LinkStats linkStats() const override;

    // Which meters the UI is currently showing. Metering shares the CI-V stream
    // with tuning, so an unwatched meter's round trip is pure contention — see
    // MeterPoller. Public so the seam can drive it once a verb exists; until
    // then the backend polls a small default set.
    void setMeterVisible(MeterId id, bool visible);

    // The model this backend resolved from CI-V 0x19 0x00, or the conservative
    // fallback until the radio answers.
    [[nodiscard]] const IcomModel& model() const noexcept { return *m_model; }

private slots:
    void onSessionConnected(const QString& deviceName);
    void onSessionDisconnected(const QString& reason);
    void onCivFrame(const AetherSDR::icom::CivFrame& frame);
    void onAudio(const std::vector<float>& mono);
    void onMeterTick();
    void onLinkTick();

private:
    void publishCapabilities();
    void publishMeterDefs();
    void sendUserCommand(const std::vector<std::uint8_t>& frame);
    void applyScopeStartup();
    [[nodiscard]] int sliceId() const noexcept { return 0; }
    [[nodiscard]] QString panId() const { return QStringLiteral("0"); }

    std::unique_ptr<IcomSession> m_session;
    const IcomModel* m_model = nullptr;

    ScopeDecoder m_scope;
    ScopeCalibration m_scopeCal;
    MeterPoller m_meters;

    // 48 kHz mono from the radio -> 24 kHz interleaved stereo for the engine.
    //
    // BOTH halves of that conversion are load-bearing and neither is optional:
    // the seam's per-slice audio contract is interleaved stereo float32 at
    // 24 kHz (Hl2RxDsp::audioReady says so in its signature, and TciServer's
    // resampler is constructed with a 24000 source rate). Feeding it 48 kHz
    // mono plays back an octave low in one ear, which through TCI means WSJT-X
    // decodes nothing and the spectrum looks half as wide as it is.
    std::unique_ptr<Resampler> m_rxResampler;
    static constexpr int kRadioAudioRateHz  = 48000;
    static constexpr int kEngineAudioRateHz = 24000;

    QTimer* m_meterTimer = nullptr;
    QTimer* m_linkTimer = nullptr;

    QString m_deviceName;
    std::uint64_t m_frequencyHz = 0;
    CivMode m_mode = CivMode::Usb;
    bool m_dataMode = false;
    bool m_connected = false;
    bool m_keyed = false;
    bool m_overflow = false;
    double m_vdVolts = 0.0;
    double m_idAmps = 0.0;
    int m_txPowerPercent = 0;
    // Keying can originate at the radio's own PTT, so transmit state is POLLED
    // rather than inferred from our own commands. Slow: it only has to notice a
    // transmission, and it shares the CI-V stream with tuning.
    std::int64_t m_lastPttPollMs = 0;
    static constexpr int kPttPollMs = 250;
    // The scope geometry the RADIO last reported, from its own sweeps. Both pan
    // intents reason against it: a zoom step needs to know which of the eight
    // spans it is leaving, and a centre request needs a truth to snap back to.
    // Zero means no sweep has arrived yet, in which case neither intent acts.
    std::int64_t m_scopeCentreHz = 0;
    std::int64_t m_scopeSpanHz = 0;
    LinkStats m_link;
};

}  // namespace AetherSDR::icom
