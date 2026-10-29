#pragma once

#include "core/backends/anan/P1Client.h"
#include "core/backends/anan/AnanTxDsp.h"
#include "core/backends/anan/AnanTransmitPower.h"
#include "core/backends/TxAudioSource.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR::anan {
// Backend-private protocol dispatch. Both child clients follow this
// object's affinity onto the existing ANAN IO thread; no extra thread exists.
class AnanClient : public QObject {
    Q_OBJECT
public:
    explicit AnanClient(QObject* parent = nullptr);
    ~AnanClient() override;
    struct Params : P2Client::Params { int protocol = 2; };
    using LinkCounters = P2Client::LinkCounters;
    bool start(const Params& params, int timeoutMs = 2000);
    Q_INVOKABLE void stop();
    // Build the DSP on the existing planning worker before transferring it here.
    bool prepareTransmit(std::unique_ptr<AnanTxDsp> dsp, const P2TxControl& control);
    void setTransmitPlanner(QObject* planner) { m_transmitPlanner = planner; }
    bool stageTransmitDsp(std::shared_ptr<AnanTxDsp> dsp);
    void discardStagedTransmit();
    bool armStagedTransmit(const P2TxControl& control);
    void disarmTransmit(bool discardDsp = true);
    void setTransmitCarrier(bool enabled, bool down);
    void finishTransmitAudio(const TxCoordinator::Context& context);
    [[nodiscard]] std::size_t queuedTransmitFrames() const noexcept
    { return m_transmitPendingFrames.load(std::memory_order_relaxed); }
    bool setTransmitPower(const TransmitPowerProfile& profile, int band, int percent,
                          const TxCoordinator::Operation& operation);
    [[nodiscard]] std::optional<WdspChannel::Mode> transmitMode() const
    { return m_transmitDsp ? m_transmitDsp->mode() : std::nullopt; }
    bool setTransmitKeying(bool key, const TxCoordinator::Operation& operation);
    bool submitTransmitAudio(const QByteArray& pcm, int rate, TxAudioSource source,
        const TxCoordinator::Operation& operation, const TxCoordinator::Context& context);
    Q_INVOKABLE void setDdc0FrequencyHz(double hz);
    Q_INVOKABLE void setFrequencyCorrection(double factor);
    Q_INVOKABLE void setSpeakerAmpMuted(bool muted);
    void setMicOptions(const MicOptions& options);
    bool setReceiveStreams(const std::vector<DdcConfig>& streams,
                           const std::vector<double>& centers);
    Q_INVOKABLE void setFrontEndAttenuation(int adc, int db, int alexDb);
    Q_INVOKABLE void setStepAttenuationDb(int adc, int db);
    Q_INVOKABLE void setRxAntenna(int antenna);
    void setReceiveFilterPlan(const ReceiveFilterPlan& plan);
    Q_INVOKABLE void setHardwareFilters(const QVariantMap& options);
    Q_INVOKABLE void setReceiveHardware(const QVariantMap& options, int antenna);
    Q_INVOKABLE bool setDdcRateLive(int ddc, int rateKsps);
    Q_INVOKABLE void enqueueSpeakerAudio(const QByteArray& audio);
signals:
    void transmitFault(const QString& reason);
    void linkUp();
    void linkDown();
    void connectionError(const QString& reason);
    void ddcIqReady(int index, const std::vector<std::complex<float>>& block);
    void ddc0IqReady(const std::vector<std::complex<float>>& block);
    void ddcSequenceGap(int ddcIndex);
    void linkCountersUpdated(const P2Client::LinkCounters& counters);
    void discoveryInfoReceived(quint8 boardId, quint8 firmwareVer, quint8 reportedReceivers);
    void supplyRailSampled(int counts);
private:
    template<class Function> decltype(auto) transmitTransport(Function&& function)
    {
        if (m_protocol == 1) { return function(m_p1); }
        return function(m_p2);
    }
    int m_transmitFlushBlocks = 0;
    bool m_transmitFinishing = false;
    bool m_transmitCarrier = false;
    static constexpr std::size_t kTransmitAudioCapacity = AnanTxDsp::kMaximumQueuedInputSamples;
    std::atomic<std::size_t> m_transmitPendingFrames{0};
    std::shared_ptr<AnanTxDsp> m_transmitDsp;
    QObject* m_transmitPlanner = nullptr;
    bool m_transmitConfigured = false;
    TxCoordinator::Operation m_transmitOperation;
    TxCoordinator::Context m_transmitContext;
    std::optional<TxAudioSource> m_transmitSource;
    std::array<float, kTransmitAudioCapacity> m_transmitAudio{};
    std::size_t m_transmitHead = 0;
    std::size_t m_transmitCount = 0;
    QTimer* m_transmitAudioTimer = nullptr;
    bool m_transmitKeyed = false;
    bool m_transmitMediaStarted = false;
    void clearTransmitAudio();
    void failTransmit(const QString& reason);
    void pumpTransmitAudio();
    friend struct AnanClientTestAccess;
    P1Client* m_p1;
    P2Client* m_p2;
    int m_protocol = 2;
};
} // namespace AetherSDR::anan
