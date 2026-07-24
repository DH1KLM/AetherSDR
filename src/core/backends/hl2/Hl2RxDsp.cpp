#include "core/backends/hl2/Hl2RxDsp.h"

#include <QMetaType>

#include <cmath>

namespace AetherSDR::hl2 {

Hl2RxDsp::Hl2RxDsp(QObject* parent) : QObject(parent)
{
    // Registered so audioReady/spectrumReady can cross a thread boundary once
    // this object is moved onto its own DSP thread (queued connections).
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
}

Hl2RxDsp::~Hl2RxDsp() = default;

bool Hl2RxDsp::configure(const Config& config, std::string* error)
{
    m_config = config;

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    wc.dspBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    wc.inputSampleRate = config.inputSampleRateHz;   // RF/IF rate from the HL2
    wc.dspSampleRate = config.audioSampleRateHz;     // WDSP decimates IF -> audio
    wc.outputSampleRate = config.audioSampleRateHz;
    wc.mode = config.mode;
    wc.filterLowHz = config.filterLowHz;
    wc.filterHighHz = config.filterHighHz;
    wc.blockForOutput = config.blockForOutput;

    auto channel = WdspChannel::create(wc, error);
    if (!channel)
        return false;
    m_channel = std::move(channel);
    m_spectrum = std::make_unique<Hl2Spectrum>(config.fftSize);

    m_iqBuffer.clear();
    m_i.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    const std::size_t outN = m_channel->outputBlockSize();
    m_left.assign(outN, 0.0f);
    m_right.assign(outN, 0.0f);
    m_stereo.assign(outN * 2, 0.0f);
    return true;
}

void Hl2RxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    if (m_channel)
        m_channel->setMode(mode);
}

void Hl2RxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel)
        m_channel->setFilter(lowHz, highHz);
}

void Hl2RxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // Panadapter: the FFT sees the full-rate IQ.
    if (m_spectrum->process(iq, m_bins) > 0)
        emit spectrumReady(m_bins);

    // Audio: buffer into fixed WdspChannel blocks.
    m_iqBuffer.insert(m_iqBuffer.end(), iq.begin(), iq.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        for (std::size_t n = 0; n < block; ++n) {
            m_i[n] = m_iqBuffer[consumed + n].real();
            m_q[n] = m_iqBuffer[consumed + n].imag();
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        if (res != WdspChannel::ProcessResult::Ok)
            continue;   // Underrun while the pipeline fills, etc. — no output yet

        const std::size_t outN = m_left.size();
        double sumSq = 0.0;
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_left[k];
            m_stereo[2 * k + 1] = m_right[k];
            sumSq += static_cast<double>(m_left[k]) * m_left[k];
        }
        emit audioReady(m_stereo);
        const double rms = outN ? std::sqrt(sumSq / static_cast<double>(outN)) : 0.0;
        emit meterUpdate(static_cast<float>(20.0 * std::log10(rms + 1e-12)));
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::hl2
