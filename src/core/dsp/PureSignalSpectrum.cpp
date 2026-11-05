#include "PureSignalSpectrum.h"
#include "FftwPlannerLock.h"
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR {
struct PureSignalSpectrum::Impl {
    std::array<std::complex<float>,kSamples> transmit{}, feedback{};
    std::array<bool,kSamples> clipped{};
    std::array<std::complex<double>,kSamples> input{}, output{};
    std::array<double,kSamples> window{}, txPower{}, rxPower{};
    fftw_plan plan = nullptr;
    std::size_t position = 0, count = 0, clippingCount = 0;
    std::int64_t lastFrameMs = 0, lastInputMs = 0;
    double windowSum = 0;
    bool twoTone = false;
    PureSignalSpectrumSnapshot result;

    Impl()
    {
        for (int i = 0; i < kSamples; ++i) {
            window[i] = 0.5*(1-std::cos(2*std::numbers::pi*i/(kSamples-1)));
            windowSum += window[i];
        }
        const auto lock = fftwPlannerLock();
        plan = fftw_plan_dft_1d(kSamples,reinterpret_cast<fftw_complex*>(input.data()),
            reinterpret_cast<fftw_complex*>(output.data()),FFTW_FORWARD,FFTW_ESTIMATE | FFTW_UNALIGNED);
    }
    ~Impl()
    {
        const auto lock = fftwPlannerLock();
        if (plan) { fftw_destroy_plan(plan); }
    }
    double bandPower(const std::array<double,kSamples>& powers, double hz) const
    {
        const int center = static_cast<int>(std::lround(hz*kSamples/kSampleRate));
        double total = 0;
        for (int offset = -2; offset <= 2; ++offset) { total += powers[(center+offset+kSamples)%kSamples]; }
        return total;
    }
    void calculate(std::int64_t nowMs)
    {
        result = {}; result.ready = true; result.capturedAtMs = nowMs; result.possibleClipping = clippingCount != 0;
        for (int source = 0; source < 2; ++source) {
            const auto& ring = source == 0 ? transmit : feedback;
            auto& powers = source == 0 ? txPower : rxPower;
            for (int i = 0; i < kSamples; ++i) {
                const auto sample = ring[(position+i)%kSamples];
                input[i] = std::complex<double>(sample.imag(),sample.real())*window[i];
            }
            fftw_execute(plan);
            for (int i = 0; i < kSamples; ++i) { powers[i] = std::norm(output[i])/(windowSum*windowSum); }
        }
        for (int j = -kSamples/2; j < kSamples/2; ++j) {
            const double hz = double(j)*kSampleRate/kSamples;
            if (std::abs(hz) > 10000) { continue; }
            const int bin = (j+kSamples)%kSamples;
            result.frequencyHz.push_back(hz);
            result.transmitDbfs.push_back(10*std::log10(std::max(txPower[bin],1e-16)));
            result.feedbackDbfs.push_back(10*std::log10(std::max(rxPower[bin],1e-16)));
        }
        lastFrameMs = nowMs;
        if (!twoTone || result.possibleClipping) { return; }
        const double positive = bandPower(txPower,700)+bandPower(txPower,1900);
        const double negative = bandPower(txPower,-700)+bandPower(txPower,-1900);
        const double sign = positive >= negative ? 1 : -1;
        const double tx1 = bandPower(txPower,sign*700), tx2 = bandPower(txPower,sign*1900);
        const double rx1 = bandPower(rxPower,sign*700), rx2 = bandPower(rxPower,sign*1900);
        if (std::min(tx1,tx2) < 1e-10 || std::min(rx1,rx2) < 1e-10
            || std::max(tx1,tx2) > 10*std::min(tx1,tx2)
            || std::max(rx1,rx2) > 10*std::min(rx1,rx2)
            || std::max(positive,negative) < 100*std::min(positive,negative)) { return; }
        const double carrierPower = (rx1+rx2)*0.5;
        for (int i = 0; i < 4; ++i) {
            const int n = i+1;
            const double lower = sign*((n+1)*700-n*1900);
            const double upper = sign*((n+1)*1900-n*700);
            const double distortion = std::max(bandPower(rxPower,lower),bandPower(rxPower,upper));
            result.imdDbc[i] = 10*std::log10(std::max(distortion,1e-30)/carrierPower);
        }
        result.imdValid = true;
    }
};

PureSignalSpectrum::PureSignalSpectrum() : m_impl(std::make_unique<Impl>()) {}
PureSignalSpectrum::~PureSignalSpectrum() = default;
bool PureSignalSpectrum::valid() const noexcept { return m_impl->plan != nullptr; }
bool PureSignalSpectrum::fresh(std::int64_t nowMs) const noexcept
{
    return m_impl->result.ready && nowMs >= m_impl->result.capturedAtMs
        && nowMs-m_impl->result.capturedAtMs <= 500;
}
const PureSignalSpectrumSnapshot& PureSignalSpectrum::snapshot() const noexcept { return m_impl->result; }
void PureSignalSpectrum::reset() noexcept
{
    m_impl->position = m_impl->count = m_impl->clippingCount = 0;
    m_impl->lastFrameMs = m_impl->lastInputMs = 0; m_impl->twoTone = false; m_impl->result = {};
    m_impl->clipped.fill(false);
}
bool PureSignalSpectrum::feed(std::span<const std::complex<float>> tx,
    std::span<const std::complex<float>> rx, bool twoTone, std::int64_t nowMs)
{
    if (!valid() || nowMs < 0 || tx.empty() || tx.size() != rx.size() || tx.size() > 4096
        || (m_impl->count && nowMs < m_impl->lastInputMs)) { reset(); return false; }
    if (m_impl->count && (m_impl->twoTone != twoTone || nowMs-m_impl->lastInputMs > 100)) { reset(); }
    m_impl->lastInputMs = nowMs; m_impl->twoTone = twoTone;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        const auto finite = [](std::complex<float> v) {
            return std::isfinite(v.real()) && std::isfinite(v.imag()) && std::abs(v.real()) <= 1 && std::abs(v.imag()) <= 1;
        };
        if (!finite(tx[i]) || !finite(rx[i])) { reset(); return false; }
        const bool clip = std::abs(tx[i].real()) >= 0.99998 || std::abs(tx[i].imag()) >= 0.99998
            || std::abs(rx[i].real()) >= 0.99998 || std::abs(rx[i].imag()) >= 0.99998;
        if (m_impl->clipped[m_impl->position]) { --m_impl->clippingCount; }
        m_impl->clipped[m_impl->position] = clip;
        if (clip) { ++m_impl->clippingCount; }
        m_impl->transmit[m_impl->position] = tx[i]; m_impl->feedback[m_impl->position] = rx[i];
        m_impl->position = (m_impl->position+1)%kSamples;
        m_impl->count = std::min<std::size_t>(m_impl->count+1,kSamples);
    }
    if (m_impl->count == kSamples && (!m_impl->result.ready || nowMs-m_impl->lastFrameMs >= 100)) { m_impl->calculate(nowMs); }
    return true;
}
} // namespace AetherSDR
