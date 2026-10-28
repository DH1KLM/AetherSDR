#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace AetherSDR::rxtrace {

struct Level {
    double sumSquares = 0;
    double peak = 0;
    std::uint64_t samples = 0;
    std::uint64_t invalid = 0;
    std::uint64_t fullScale = 0;
    std::int64_t peakNs = 0;

    void add(double power, std::int64_t now) noexcept
    {
        if (!std::isfinite(power)) { ++invalid; return; }
        ++samples;
        sumSquares += power;
        if (power >= 1.0) { ++fullScale; }
        if (power > peak) { peak = power; peakNs = now; }
    }
    double rmsDb() const noexcept { return 10 * std::log10(std::max(samples ? sumSquares / samples : 0.0, 1e-30)); }
    double peakDb() const noexcept { return 10 * std::log10(std::max(peak, 1e-30)); }
};

struct Sample {
    enum class Stage { Anan, Playback };
    Stage stage = Stage::Anan;
    int channel = -1;
    int rate = 0;
    std::int64_t startNs = 0, endNs = 0;
    std::uint64_t mouseStart = 0, mouseEnd = 0;
    std::int64_t lastMouseNs = 0;
    Level iq, dsp, pcm;
    std::array<std::uint64_t, 6> results{};
    std::int64_t firstUnderrunNs = 0, lastUnderrunNs = 0;
    std::int64_t inputGapMaxNs = 0, exchangeMaxNs = 0, callbackMaxNs = 0;
    double meterMaxDb = -300;
    std::int64_t meterMaxNs = 0;
    std::uint64_t gaps = 0;
};

// Fixed storage and bounded admission: producers never log, allocate or wait for the UI.
class Trace final {
public:
    static Trace& instance() { static Trace trace; return trace; }
    void mouse(std::int64_t now) noexcept
    {
        m_lastMouse.store(now, std::memory_order_relaxed);
        m_mouse.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t mouseCount() const noexcept { return m_mouse.load(std::memory_order_relaxed); }
    std::int64_t lastMouse() const noexcept { return m_lastMouse.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

    bool publish(const Sample& value) noexcept
    {
        std::uint64_t position = m_write.load(std::memory_order_relaxed);
        for (int attempt = 0; attempt < 4; ++attempt) {
            Cell& cell = m_cells[position % kCapacity];
            if (cell.sequence.load(std::memory_order_acquire) != position) { break; }
            if (m_write.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                cell.sample = value;
                cell.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        }
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Single consumer: the existing GUI performance heartbeat.
    bool take(Sample& value) noexcept
    {
        Cell& cell = m_cells[m_read % kCapacity];
        if (cell.sequence.load(std::memory_order_acquire) != m_read + 1) { return false; }
        value = cell.sample;
        cell.sequence.store(m_read + kCapacity, std::memory_order_release);
        ++m_read;
        return true;
    }
private:
    static constexpr std::size_t kCapacity = 128;
    struct Cell { std::atomic<std::uint64_t> sequence{0}; Sample sample; };
    Trace() { for (std::size_t n = 0; n < kCapacity; ++n) { m_cells[n].sequence.store(n); } }
    std::array<Cell, kCapacity> m_cells;
    std::atomic<std::uint64_t> m_write{0}, m_mouse{0}, m_dropped{0};
    std::atomic<std::int64_t> m_lastMouse{0};
    std::uint64_t m_read = 0;
};

class Window final {
public:
    Sample data;
    void begin(Sample::Stage stage, int channel, int rate, std::int64_t now) noexcept
    {
        if (data.startNs == 0 || data.channel != channel || data.rate != rate
            || now - data.endNs > 1'000'000'000LL) {
            data = {};
            data.stage = stage;
            data.channel = channel;
            data.rate = rate;
            data.startNs = now;
            data.mouseStart = Trace::instance().mouseCount();
            m_lastInputNs = 0;
        }
        if (m_lastInputNs) { data.inputGapMaxNs = std::max(data.inputGapMaxNs, now - m_lastInputNs); }
        m_lastInputNs = now;
        data.endNs = now;
    }
    void finish(std::int64_t now) noexcept
    {
        data.callbackMaxNs = std::max(data.callbackMaxNs, now - data.endNs);
        data.endNs = now;
        if (now - data.startNs < 100'000'000LL) { return; }
        data.mouseEnd = Trace::instance().mouseCount();
        data.lastMouseNs = Trace::instance().lastMouse();
        Trace::instance().publish(data);
        const auto stage = data.stage;
        const int channel = data.channel, rate = data.rate;
        data = {};
        data.stage = stage; data.channel = channel; data.rate = rate;
        data.startNs = now; data.endNs = now;
        data.mouseStart = Trace::instance().mouseCount();
    }
private:
    std::int64_t m_lastInputNs = 0;
};
} // namespace AetherSDR::rxtrace
