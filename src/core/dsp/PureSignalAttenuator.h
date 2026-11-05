#pragma once

#include "PureSignal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AetherSDR {
// One observation per completed calculation. Uncommitted changes end with their over.
// Hardware updates require a native CALCC/IQC pause acknowledgement.
class PureSignalAttenuator final {
public:
    enum class Action { None, Pause, Attenuate, Resume };
    void beginOver(int attempts) noexcept { m_attempts = attempts; }
    void cancelOver() noexcept
    {
        if (m_state == State::RequestPause) { m_state = State::Monitor; }
        else if (m_state != State::Monitor) { m_state = State::CancelWait; }
    }
    [[nodiscard]] bool pending() const noexcept { return m_state != State::Monitor; }
    [[nodiscard]] int targetDb() const noexcept { return m_targetDb; }
    Action observe(const PureSignalSnapshot& snapshot, bool enabled, bool paused,
                   int currentDb, std::int64_t nowMs) noexcept
    {
        if (!enabled) { cancelOver(); }
        if (m_state == State::CancelWait) { return paused && nowMs >= m_settleUntil ? Action::Resume : Action::None; }
        if (m_state == State::WaitPause) { return paused ? Action::Attenuate : Action::None; }
        if (m_state == State::Settle) { return nowMs >= m_settleUntil ? Action::Resume : Action::None; }
        if (m_state == State::RequestPause) { return Action::Pause; }
        if (!enabled || snapshot.information[5] == m_attempts) { return Action::None; }
        m_attempts = snapshot.information[5];
        const int level = snapshot.information[4];
        if (snapshot.information[0] != 0 || level <= 0 || currentDb < 0 || currentDb > 31
            || (level <= 181 && (level > 128 || currentDb == 0))) { return Action::None; }
        // Thetis PSForm: target 152.293, adjustment thresholds 128/181, 0..31 dB.
        const int delta = level > 256 ? 31 : static_cast<int>(std::lround(20*std::log10(level/152.293)));
        m_targetDb = std::clamp(currentDb + delta, 0, 31);
        if (m_targetDb == currentDb) { return Action::None; }
        m_state = State::RequestPause;
        return Action::Pause;
    }
    void accepted(Action action, std::int64_t nowMs, int attempts) noexcept
    {
        if (action == Action::Pause && m_state == State::RequestPause) { m_state = State::WaitPause; m_settleUntil = 0; }
        else if (action == Action::Attenuate && m_state == State::WaitPause) {
            m_state = State::Settle; m_settleUntil = nowMs + 100;
        } else if (action == Action::Resume) { m_state = State::Monitor; m_attempts = attempts; }
    }
private:
    enum class State { Monitor, RequestPause, WaitPause, Settle, CancelWait };
    State m_state = State::Monitor;
    int m_attempts = 0;
    int m_targetDb = 31;
    std::int64_t m_settleUntil = 0;
};
} // namespace AetherSDR
