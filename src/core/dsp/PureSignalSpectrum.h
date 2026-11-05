#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AetherSDR {
struct PureSignalSpectrumSnapshot {
    bool ready = false;
    std::int64_t capturedAtMs = 0;
    bool possibleClipping = false;
    bool imdValid = false;
    std::vector<double> frequencyHz;
    std::vector<double> transmitDbfs;
    std::vector<double> feedbackDbfs;
    std::array<double,4> imdDbc{};
};

// Construct/destroy on the existing DSP planner; feed and read on the owning
// I/O thread. Raw HPSDR I/Q is swapped, as in Spectrum0, for RF-oriented axes.
class PureSignalSpectrum final {
public:
    static constexpr int kSamples = 8192;
    static constexpr int kSampleRate = 192000;
    PureSignalSpectrum();
    ~PureSignalSpectrum();
    PureSignalSpectrum(const PureSignalSpectrum&) = delete;
    PureSignalSpectrum& operator=(const PureSignalSpectrum&) = delete;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool fresh(std::int64_t nowMs) const noexcept;
    void reset() noexcept;
    bool feed(std::span<const std::complex<float>> transmit,
              std::span<const std::complex<float>> feedback,
              bool twoTone, std::int64_t nowMs);
    [[nodiscard]] const PureSignalSpectrumSnapshot& snapshot() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace AetherSDR
