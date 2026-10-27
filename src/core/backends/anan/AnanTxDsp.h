#pragma once

#include "core/TxCoordinator.h"
#include "core/dsp/WdspChannel.h"

#include <array>
#include <complex>
#include <memory>
#include <span>
#include <string>

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
    enum class Result { Output, WouldBlock, Refused, Fault };

    ~AnanTxDsp();
    bool configure(WdspChannel::Mode mode, std::string* error = nullptr);
    bool begin(const TxCoordinator::Context& context, qint64 now);
    bool end();
    Result process(std::span<const float> mono, const TxCoordinator::Context& context,
                   qint64 now, std::span<std::complex<float>> output);

private:
    std::unique_ptr<WdspChannel> m_channel;
    TxCoordinator::Context m_context;
    bool m_active = false;
    std::array<float, kInputSamples> m_zeroQ{};
    std::array<float, kOutputSamples> m_i{};
    std::array<float, kOutputSamples> m_q{};
};
} // namespace AetherSDR::anan
