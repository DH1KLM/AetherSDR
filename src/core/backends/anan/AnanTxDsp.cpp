#include "AnanTxDsp.h"
#include "AnanTxMode.h"
#include "core/LogManager.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::anan {
AnanTxDsp::~AnanTxDsp()
{
    end();
}

bool AnanTxDsp::configure(WdspChannel::Mode mode, std::string* error, int outputRate,
                         std::optional<std::pair<double,double>> passband)
{
    end();
    m_channel.reset();
    m_mode.reset();
    const auto recipe = transmitMode(mode);
    if (!recipe || (outputRate != 48000 && outputRate != 192000)) {
        if (error) { *error = "Unsupported ANAN transmit mode or protocol sample rate"; }
        return false;
    }
    WdspChannel::Config config;
    config.direction = WdspChannel::Direction::Transmit;
    config.mode = recipe->modulator;
    config.inputSampleRate = kInputRate;
    // Thetis cmaster.c uses 96 kHz inside TXA; the P2 DUC consumes 192 kHz.
    config.dspSampleRate = kDspRate;
    config.outputSampleRate = outputRate;
    m_outputSamples = kInputSamples * outputRate / kInputRate;
    config.inputBlockSize = kInputSamples;
    config.dspBlockSize = kInputSamples * kDspRate / kInputRate;
    config.filterLowHz = passband ? passband->first : recipe->filterLowHz;
    config.filterHighHz = passband ? passband->second : recipe->filterHighHz;
    config.blockForOutput = false;
    m_channel = WdspChannel::create(config, error);
    if (!m_channel) { return false; }
    if (!m_channel->discardTransmitData()) {
        m_channel.reset();
        if (error) { *error = "ANAN TX initial discard failed"; }
        return false;
    }
    m_mode = mode;
    m_monitorResampler = std::make_unique<Resampler>(outputRate, kInputRate, static_cast<int>(m_outputSamples));
    return true;
}

QByteArray AnanTxDsp::monitorAudio(std::span<const std::complex<float>> iq)
{
    if (!m_active || !m_mode || !m_monitorResampler || iq.size() != m_outputSamples) { return {}; }
    const double rate = m_monitorResampler->srcRate();
    const double dcAlpha = 1.0 - std::exp(-2.0 * 3.141592653589793 * 20.0 / rate);
    for (std::size_t i = 0; i < iq.size(); ++i) {
        double audio = 2.0 * iq[i].real();
        if (*m_mode == WdspChannel::Mode::Fm) {
            const auto phase = iq[i] * std::conj(m_monitorPrevious);
            audio = std::norm(iq[i]) > 1e-12 && std::norm(m_monitorPrevious) > 1e-12
                ? -std::arg(phase) * rate / (2.0 * 3.141592653589793 * 5000.0) : 0;
        } else if (*m_mode == WdspChannel::Mode::Am || *m_mode == WdspChannel::Mode::Sam) {
            const double envelope = 2.0 * std::abs(iq[i]);
            if (!m_monitorStarted) { m_monitorDc = envelope; }
            m_monitorDc += dcAlpha * (envelope - m_monitorDc);
            audio = envelope - m_monitorDc;
        }
        m_monitorMono[i] = std::isfinite(audio) ? static_cast<float>(audio) : 0;
        m_monitorPrevious = iq[i];
        m_monitorStarted = true;
    }
    // Demodulate the actual post-TXA I/Q; the monitor never feeds back into RF drive.
    return m_monitorResampler->processMonoToStereo(m_monitorMono.data(), static_cast<int>(iq.size()));
}

bool AnanTxDsp::preparePureSignalSpectrum()
{
    m_spectrum = std::make_unique<PureSignalSpectrum>();
    if (!m_spectrum->valid()) {
        qCWarning(lcDsp) << "Pure Signal spectrum FFT initialization failed";
        m_spectrum.reset(); return false;
    }
    return true;
}
bool AnanTxDsp::feedPureSignalFeedback(std::span<const std::complex<float>> tx,
    std::span<const std::complex<float>> rx, qint64 nowMs)
{
    if (!m_active || !m_context.permitsDispatch(nowMs) || !m_channel
        || tx.empty() || tx.size() > 4096 || tx.size() != rx.size()) { return false; }
    // CALCC's hardware peak describes a full TXA signal. Undo our post-TXA
    // headroom in the digital reference only, so its amplitude buckets still
    // span the IQC input domain. RF samples and raw spectrum levels stay as sent.
    std::array<std::complex<float>,4096> reference{};
    for (std::size_t i = 0; i < tx.size(); ++i) { reference[i] = tx[i] / kWireAmplitudeGain; }
    return m_channel->feedPureSignal(std::span(reference).first(tx.size()),rx,true);
}
void AnanTxDsp::requestPureSignalSpectrum(bool wanted, qint64 nowMs)
{
    if (wanted && m_spectrum) {
        if (m_spectrumWantedUntilMs <= nowMs) { m_spectrum->reset(); }
        m_spectrumWantedUntilMs = nowMs+1000;
    }
}
void AnanTxDsp::resetPureSignalSpectrum()
{
    if (m_spectrum) { m_spectrum->reset(); }
}
void AnanTxDsp::feedPureSignalSpectrum(std::span<const std::complex<float>> tx,
    std::span<const std::complex<float>> rx, bool twoTone, qint64 nowMs)
{
    if (!m_spectrum || !m_active || !m_context.permitsDispatch(nowMs)) { return; }
    if (m_spectrumWantedUntilMs <= nowMs) {
        if (m_spectrumWantedUntilMs != 0) { m_spectrum->reset(); m_spectrumWantedUntilMs = 0; }
        return;
    }
    m_spectrum->feed(tx,rx,twoTone,nowMs);
}
const PureSignalSpectrumSnapshot* AnanTxDsp::pureSignalSpectrum(qint64 nowMs) const noexcept
{
    return m_active && m_context.permitsDispatch(nowMs) && m_spectrum && m_spectrum->fresh(nowMs)
        ? &m_spectrum->snapshot() : nullptr;
}

bool AnanTxDsp::trySetFilter(double lowHz, double highHz)
{
    return m_channel && std::isfinite(lowHz) && std::isfinite(highHz) && lowHz >= -10000
        && highHz <= 10000 && highHz-lowHz >= 50 && m_channel->trySetFilter(lowHz,highHz);
}

bool AnanTxDsp::begin(const TxCoordinator::Context& context, qint64 now)
{
    if (!end() || !m_channel || !context.permitsDispatch(now)
        || !m_channel->setRunning(true)) {
        return false;
    }
    m_context = context;
    m_monitorPrevious = {};
    m_monitorDc = 0;
    m_monitorStarted = false;
    if (m_monitorResampler) { m_monitorResampler->reset(); }
    m_active = true;
    return true;
}

bool AnanTxDsp::end()
{
    resetPureSignalSpectrum();
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
        || output.size() != m_outputSamples) {
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
    if (m_channel->processIq(mono, m_zeroQ, std::span(m_i).first(m_outputSamples),
                             std::span(m_q).first(m_outputSamples)) != WdspChannel::ProcessResult::Ok) {
        end();
        return Result::Fault;
    }
    if (!context.permitsDispatch(now)) {
        end();
        return Result::Refused;
    }
    for (std::size_t i = 0; i < m_outputSamples; ++i) {
        if (!std::isfinite(m_i[i]) || !std::isfinite(m_q[i])
            || std::hypot(m_i[i], m_q[i]) > 1.0f) {
            end();
            return Result::Fault;
        }
    }
    for (std::size_t i = 0; i < m_outputSamples; ++i) {
        // TXA already has HPSDR wire handedness. Reserve 6 dB of digital headroom.
        output[i] = {kWireAmplitudeGain * m_i[i], kWireAmplitudeGain * m_q[i]};
    }
    return Result::Output;
}
} // namespace AetherSDR::anan
