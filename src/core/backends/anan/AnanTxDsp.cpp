#include "AnanTxDsp.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::anan {
AnanTxDsp::~AnanTxDsp()
{
    end();
}

bool AnanTxDsp::configure(WdspChannel::Mode mode, std::string* error)
{
    end();
    m_channel.reset();
    if (mode != WdspChannel::Mode::Usb && mode != WdspChannel::Mode::Lsb) {
        if (error) { *error = "ANAN TX currently supports USB and LSB only"; }
        return false;
    }
    WdspChannel::Config config;
    config.direction = WdspChannel::Direction::Transmit;
    config.mode = mode;
    config.inputSampleRate = kInputRate;
    // Thetis cmaster.c uses 96 kHz inside TXA; the P2 DUC consumes 192 kHz.
    config.dspSampleRate = kDspRate;
    config.outputSampleRate = kOutputRate;
    config.inputBlockSize = kInputSamples;
    config.dspBlockSize = kInputSamples * kDspRate / kInputRate;
    config.filterLowHz = mode == WdspChannel::Mode::Usb ? 300.0 : -2700.0;
    config.filterHighHz = mode == WdspChannel::Mode::Usb ? 2700.0 : -300.0;
    config.blockForOutput = false;
    m_channel = WdspChannel::create(config, error);
    if (!m_channel) { return false; }
    if (!m_channel->discardTransmitData()) {
        m_channel.reset();
        if (error) { *error = "ANAN TX initial discard failed"; }
        return false;
    }
    return true;
}

bool AnanTxDsp::begin(const TxCoordinator::Context& context, qint64 now)
{
    if (!end() || !m_channel || !context.permitsDispatch(now)
        || !m_channel->setRunning(true)) {
        return false;
    }
    m_context = context;
    m_active = true;
    return true;
}

bool AnanTxDsp::end()
{
    m_active = false;
    m_context = {};
    return !m_channel || m_channel->discardTransmitData();
}

AnanTxDsp::Result AnanTxDsp::process(std::span<const float> mono,
    const TxCoordinator::Context& context, qint64 now,
    std::span<std::complex<float>> output)
{
    std::fill(output.begin(), output.end(), std::complex<float>{});
    if (!m_active || !m_channel || !context.sameContext(m_context)
        || !context.permitsDispatch(now) || mono.size() != kInputSamples
        || output.size() != kOutputSamples) {
        end();
        return Result::Refused;
    }
    for (const float sample : mono) {
        if (!std::isfinite(sample) || std::abs(sample) > 1.0f) {
            end();
            return Result::Refused;
        }
    }
    if (!m_channel->outputReady()) { return Result::WouldBlock; }
    if (m_channel->processIq(mono, m_zeroQ, m_i, m_q) != WdspChannel::ProcessResult::Ok) {
        end();
        return Result::Fault;
    }
    if (!context.permitsDispatch(now)) {
        end();
        return Result::Refused;
    }
    for (std::size_t i = 0; i < kOutputSamples; ++i) {
        if (!std::isfinite(m_i[i]) || !std::isfinite(m_q[i])
            || std::hypot(m_i[i], m_q[i]) > 1.0f) {
            end();
            return Result::Fault;
        }
    }
    for (std::size_t i = 0; i < kOutputSamples; ++i) {
        // TXA already has HPSDR wire handedness. Reserve 6 dB of digital headroom.
        output[i] = {0.5f * m_i[i], 0.5f * m_q[i]};
    }
    return Result::Output;
}
} // namespace AetherSDR::anan
