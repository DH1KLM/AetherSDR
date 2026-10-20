#pragma once

#include "core/backends/anan/AnanSliceAudio.h"
#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <vector>

namespace AetherSDR::anan {
// Single-owner mixer for stereo 24 kHz PCM. Call only on the DSP/I/O thread.
// A route joins on its first block and leaves only through configure().
class AnanAudioMixer {
public:
    struct Route {
        int id = 0;
        bool muted = false;
        int gain = 100, pan = 50;
        bool operator==(const Route&) const = default;
    };
    static constexpr std::size_t kCapacityFrames = 6000;
    void reset()
    {
        m_slots.clear();
        m_droppedFrames = 0;
    }
    void configure(const std::vector<Route>& routes)
    {
        std::erase_if(m_slots, [&](const auto& slot) {
            return std::none_of(routes.begin(), routes.end(), [&](const Route& r) {
                return r.id == slot.first;
            });
        });
        for (const Route& route : routes) {
            auto [it, added] = m_slots.try_emplace(route.id);
            if (added) {
                it->second.ring.resize(2 * kCapacityFrames);
            }
            it->second.route = route;
        }
    }
    void resetBuffers()
    {
        for (auto& [id, slot] : m_slots) {
            slot.read = slot.available = 0;
            slot.joined = false;
        }
    }
    bool push(int id, std::span<const float> stereo)
    {
        const auto found = m_slots.find(id);
        if (found == m_slots.end() || stereo.empty() || stereo.size() % 2 != 0) {
            return false;
        }
        Slot& slot = found->second;
        std::size_t frames = stereo.size() / 2;
        if (frames > kCapacityFrames) {
            m_droppedFrames += frames - kCapacityFrames;
            stereo = stereo.last(2 * kCapacityFrames);
            frames = kCapacityFrames;
        }
        if (slot.available + frames > kCapacityFrames) {
            const std::size_t drop = slot.available + frames - kCapacityFrames;
            slot.read = (slot.read + drop) % kCapacityFrames;
            slot.available -= drop;
            m_droppedFrames += drop;
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto write = (slot.read + slot.available + frame) % kCapacityFrames;
            slot.ring[2 * write] = stereo[2 * frame];
            slot.ring[2 * write + 1] = stereo[2 * frame + 1];
        }
        slot.available += frames;
        slot.joined = true;
        return true;
    }
    std::vector<float> drain()
    {
        std::size_t frames = std::numeric_limits<std::size_t>::max();
        bool any = false;
        for (const auto& [id, slot] : m_slots) {
            if (slot.joined) {
                frames = std::min(frames, slot.available);
                any = true;
            }
        }
        if (!any || frames == 0) {
            return {};
        }
        std::vector<float> output(frames * 2, 0.0f);
        for (auto& [id, slot] : m_slots) {
            if (!slot.joined) {
                continue;
            }
            const float gain = slot.route.muted ? 0.0f : sliceAudioAmplitude(slot.route.gain);
            const float left = gain * sliceAudioLeftPanGain(slot.route.pan);
            const float right = gain * sliceAudioRightPanGain(slot.route.pan);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto read = (slot.read + frame) % kCapacityFrames;
                output[2 * frame] += slot.ring[2 * read] * left;
                output[2 * frame + 1] += slot.ring[2 * read + 1] * right;
            }
            slot.read = (slot.read + frames) % kCapacityFrames;
            slot.available -= frames;
        }
        for (float& sample : output) {
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
        return output;
    }
    std::size_t droppedFrames() const { return m_droppedFrames; }
private:
    struct Slot {
        Route route;
        std::vector<float> ring;
        std::size_t read = 0, available = 0;
        bool joined = false;
    };
    std::map<int, Slot> m_slots;
    std::size_t m_droppedFrames = 0;
};
} // namespace AetherSDR::anan
