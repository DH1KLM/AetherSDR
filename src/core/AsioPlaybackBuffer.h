#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace AetherSDR {

//DH1KLM: One AudioEngine producer and one ASIO callback consumer; no DSP or locks in the callback.
class AsioPlaybackBuffer {
public:
    explicit AsioPlaybackBuffer(std::size_t frames) : m_frames(std::max<std::size_t>(frames, 1)) {}
    std::size_t freeFrames() const
    {
        return m_frames.size() - static_cast<std::size_t>(m_write.load(std::memory_order_acquire)
            - m_read.load(std::memory_order_acquire));
    }
    std::size_t write(std::span<const float> stereo)
    {
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        const std::size_t count = std::min(stereo.size() / 2, freeFrames());
        for (std::size_t i = 0; i < count; ++i) {
            m_frames[(write + i) % m_frames.size()] = {stereo[2 * i], stereo[2 * i + 1]};
        }
        m_write.store(write + count, std::memory_order_release);
        return count;
    }
    std::array<float, 2> read()
    {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) { return {}; }
        const std::array<float, 2> frame = m_frames[read % m_frames.size()];
        m_read.store(read + 1, std::memory_order_release);
        return frame;
    }
    //DH1KLM: Reset is legal only after the driver has stopped all callbacks.
    void reset() { m_read.store(0); m_write.store(0); }

    //DH1KLM: Steinberg ASIOSampleType values; aligned Int32 samples are right-justified.
    static int sampleBytes(int type)
    {
        switch (type) {
        case 16: return 2;
        case 17: return 3;
        case 18: case 19: case 24: case 25: case 26: case 27: return 4;
        case 20: return 8;
        default: return 0;
        }
    }
    static void encode(float value, int type, std::uint8_t* out)
    {
        const double sample = std::isfinite(value) ? std::clamp<double>(value, -1, 1) : 0;
        if (type == 19) {
            const float converted = static_cast<float>(sample);
            std::memcpy(out, &converted, sizeof(converted));
        } else if (type == 20) {
            std::memcpy(out, &sample, sizeof(sample));
        } else {
            const int bytes = sampleBytes(type);
            if (!bytes) { return; }
            const int bits = type == 24 ? 16 : type == 25 ? 18 : type == 26 ? 20
                : type == 27 ? 24 : bytes * 8;
            const std::int64_t scale = std::int64_t{1} << (bits - 1);
            const std::int64_t integer = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(std::llround(sample * scale)), -scale, scale - 1);
            const std::uint32_t word = static_cast<std::uint32_t>(integer);
            for (int i = 0; i < bytes; ++i) { out[i] = static_cast<std::uint8_t>(word >> (8 * i)); }
        }
    }
private:
    std::vector<std::array<float, 2>> m_frames;
    std::atomic<std::uint64_t> m_read{0};
    std::atomic<std::uint64_t> m_write{0};
};
}
