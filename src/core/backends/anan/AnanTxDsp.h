#pragma once

#include "core/TxCoordinator.h"
#include "core/dsp/WdspChannel.h"
#include "core/Resampler.h"
#include "core/dsp/PureSignalSpectrum.h"
#include <QByteArray>

#include <array>
#include <complex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace AetherSDR::anan {

// Control and exchange share the owning I/O thread. No audio callback waits
// for WDSP; WouldBlock leaves the caller responsible for its bounded input queue.
class AnanTxDsp final {
public:
    static constexpr int kInputRate = 24000;
    static constexpr int kDspRate = 96000;
    static constexpr int kOutputRate = 192000;
    static constexpr std::size_t kInputSamples = 128;
    static constexpr std::size_t kOutputSamples = 1024;
    static constexpr std::size_t kMaximumQueuedInputSamples = 2 * kInputRate;
    enum class Result { Output, WouldBlock, Refused, Fault };

    ~AnanTxDsp();
    bool configure(WdspChannel::Mode mode, std::string* error = nullptr, int outputRate = kOutputRate,
                   std::optional<std::pair<double,double>> passband = std::nullopt);
    bool trySetFilter(double lowHz, double highHz);
    bool preparePureSignalSpectrum();
    void requestPureSignalSpectrum(bool wanted, qint64 nowMs);
    void resetPureSignalSpectrum();
    void feedPureSignalSpectrum(std::span<const std::complex<float>> tx,
        std::span<const std::complex<float>> rx, bool twoTone, qint64 nowMs);
    [[nodiscard]] bool hasPureSignalSpectrum() const noexcept { return m_spectrum && m_spectrum->valid(); }
    [[nodiscard]] const PureSignalSpectrumSnapshot* pureSignalSpectrum(qint64 nowMs) const noexcept;
    QByteArray monitorAudio(std::span<const std::complex<float>> iq);
    [[nodiscard]] std::size_t outputSamples() const noexcept { return m_outputSamples; }
    bool begin(const TxCoordinator::Context& context, qint64 now);
    bool end();
    WdspChannel* pureSignalChannel() const noexcept { return m_channel.get(); }
    [[nodiscard]] std::optional<WdspChannel::Mode> mode() const noexcept { return m_mode; }
    [[nodiscard]] std::optional<double> meter(WdspChannel::TransmitMeter which) const noexcept
    { return m_active && m_channel ? m_channel->transmitMeter(which) : std::nullopt; }
    Result process(std::span<const float> mono, const TxCoordinator::Context& context,
                   qint64 now, std::span<std::complex<float>> output);

private:
    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<PureSignalSpectrum> m_spectrum;
    qint64 m_spectrumWantedUntilMs = 0;
    TxCoordinator::Context m_context;
    bool m_active = false;
    std::optional<WdspChannel::Mode> m_mode;
    std::size_t m_outputSamples = kOutputSamples;
    std::array<float, kInputSamples> m_zeroQ{};
    std::array<float, kOutputSamples> m_i{};
    std::array<float, kOutputSamples> m_q{};
    std::unique_ptr<Resampler> m_monitorResampler;
    std::array<float, kOutputSamples> m_monitorMono{};
    std::complex<float> m_monitorPrevious{};
    double m_monitorDc = 0;
    bool m_monitorStarted = false;
};
} // namespace AetherSDR::anan
