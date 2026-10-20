#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
namespace AetherSDR::anan {
// One hardware capture can feed several independently demodulated slices.
class AnanReceiveLayout {
  public:
    struct Stream {
        bool active = false;
        double centerHz = 0;
        int rateKsps = 48;
    };
    struct Slice {
        int stream = 0;
        double frequencyHz = 0;
        int labelIndex = 0;
    };
    void reset(int streams, double hz, int rate, int slices = 5)
    {
        m_limit = std::clamp(streams, 1, 5);
        m_sliceLimit = std::clamp(slices, 1, 5);
        m_next = 1;
        m_default = rate;
        m_streams = {};
        m_slices.clear();
        m_streams[0] = {true, hz, rate};
        m_slices[0] = {0, hz};
    }
    int limit() const { return m_limit; }
    const auto& streams() const { return m_streams; }
    const auto& slices() const { return m_slices; }
    const Slice* slice(int id) const
    {
        auto i = m_slices.find(id);
        return i == m_slices.end() ? nullptr : &i->second;
    }
    int occupants(int st) const
    {
        int n = 0;
        for (const auto& [id, s] : m_slices)
            if (s.stream == st)
                ++n;
        return n;
    }
    bool contains(int st, double hz) const
    {
        return st >= 0 && st < m_limit && m_streams[st].active &&
               std::abs(hz - m_streams[st].centerHz) < m_streams[st].rateKsps * 500.0;
    }
    std::optional<int> add(double hz, int requested = -1, bool own = false)
    {
        if (!valid(hz) || m_slices.size() >= std::size_t(m_sliceLimit))
            return {};
        int st = -1;
        if (requested >= 0) {
            if (!contains(requested, hz))
                return {};
            st = requested;
        } else if (!own)
            for (int j = 0; j < m_limit; ++j)
                if (contains(j, hz)) {
                    st = j;
                    break;
                }
        if (st < 0) {
            st = freeStream();
            if (st < 0)
                return {};
            m_streams[st] = {true, hz, m_default};
        }
        const int id = m_next++;
        int label = 0;
        for (; label < m_sliceLimit; ++label) {
            const bool used = std::any_of(m_slices.begin(), m_slices.end(), [label](const auto& entry) {
                return entry.second.labelIndex == label;
            });
            if (!used) {
                break;
            }
        }
        m_slices[id] = {st, hz, label};
        return id;
    }
    bool tune(int id, double hz, bool pinned)
    {
        auto i = m_slices.find(id);
        if (i == m_slices.end() || !valid(hz))
            return false;
        const int old = i->second.stream;
        if (occupants(old) == 1) {
            if (!pinned || !contains(old, hz))
                m_streams[old].centerHz = hz;
        } else if (!contains(old, hz)) {
            int st = -1;
            for (int j = 0; j < m_limit; ++j)
                if (contains(j, hz) && m_streams[j].rateKsps == m_streams[old].rateKsps) {
                    st = j;
                    break;
                }
            if (st < 0) {
                st = freeStream();
                if (st < 0)
                    return false;
                m_streams[st] = {true, hz, m_streams[old].rateKsps};
            }
            i->second.stream = st;
        }
        i->second.frequencyHz = hz;
        return true;
    }
    bool remove(int id)
    {
        auto i = m_slices.find(id);
        if (i == m_slices.end() || m_slices.size() == 1)
            return false;
        const int st = i->second.stream;
        m_slices.erase(i);
        if (!occupants(st))
            m_streams[st].active = false;
        return true;
    }
    bool setWindow(int st, double hz, int rate)
    {
        if (st < 0 || st >= m_limit || !m_streams[st].active || !valid(hz) ||
            (rate != 48 && rate != 96 && rate != 192 && rate != 384 && rate != 768 && rate != 1536))
            return false;
        for (const auto& [id, s] : m_slices)
            if (s.stream == st && std::abs(s.frequencyHz - hz) >= rate * 500.0)
                return false;
        m_streams[st].centerHz = hz;
        m_streams[st].rateKsps = rate;
        return true;
    }
    void setDefaultRate(int rate) { m_default = rate; }

  private:
    static bool valid(double hz) { return std::isfinite(hz) && hz > 0 && hz < 61'440'000; }
    int freeStream() const
    {
        for (int j = 0; j < m_limit; ++j)
            if (!m_streams[j].active)
                return j;
        return -1;
    }
    int m_limit = 1, m_sliceLimit = 5, m_next = 1, m_default = 48;
    std::array<Stream, 5> m_streams{};
    std::map<int, Slice> m_slices;
};
} // namespace AetherSDR::anan
