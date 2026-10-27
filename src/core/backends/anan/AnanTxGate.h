#pragma once

#include "core/TxCoordinator.h"

namespace AetherSDR::anan {

// Owned by the transport thread. Writers are synchronous, non-reentrant terminal
// writes; true means accepted locally, never proof of radio state or RF output.
class AnanTxGate final {
public:
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    [[nodiscard]] bool keyCommandAccepted() const noexcept { return m_keyed; }
    [[nodiscard]] bool stopPending() const noexcept { return m_stopPending; }

    template<class Writer>
    bool setEnabled(bool enabled, qint64 now, Writer&& writer)
    {
        m_enabled = enabled;
        if (!enabled) {
            return stop(now, writer);
        }
        return !m_stopPending;
    }

    template<class Writer>
    bool setKeying(bool key, const TxCoordinator::Operation& operation,
                   qint64 now, Writer&& writer)
    {
        if (!key) {
            if (!operation.sameAuthority(m_operation)) {
                return false;
            }
            return stop(now, writer);
        }
        if (!m_enabled || m_stopPending
            || (m_keyed && !operation.sameAuthority(m_operation))) {
            return false;
        }
        const TxCoordinator::Dispatch dispatch = operation.beginDispatch(now);
        if (!dispatch) {
            return false;
        }
        m_operation = operation;
        // A failed write may be ambiguous. Require an explicit successful off
        // before another key-on; never replay a failed key-on automatically.
        m_stopPending = true;
        m_keyed = false;
        if (!writer(true)) {
            return false;
        }
        m_keyed = true;
        m_stopPending = false;
        return true;
    }

    template<class Writer>
    bool poll(qint64 now, Writer&& writer)
    {
        if (m_stopPending || (m_keyed && !m_operation.permitsDispatch(now))) {
            return stop(now, writer);
        }
        return true;
    }

    template<class Writer>
    bool writeIq(const TxCoordinator::Operation& operation,
                 const TxCoordinator::Context& context,
                 qint64 now, Writer&& writer)
    {
        if (!m_enabled || !m_keyed || m_stopPending
            || !operation.sameAuthority(m_operation)) {
            return false;
        }
        const TxCoordinator::Dispatch keyDispatch = m_operation.beginDispatch(now);
        if (!keyDispatch) {
            return false;
        }
        const TxCoordinator::Dispatch mediaDispatch = context.beginDispatch(now);
        if (!mediaDispatch) {
            return false;
        }
        if (!writer()) {
            m_keyed = false;
            m_stopPending = true;
            return false;
        }
        return true;
    }

    // Only after transport teardown: this retires local command state and
    // deliberately leaves the next session disabled. It does not acknowledge RF idle.
    void transportClosed() noexcept
    {
        m_enabled = false;
        m_keyed = false;
        m_stopPending = false;
        m_operation = {};
    }

private:
    template<class Writer>
    bool stop(qint64 now, Writer&& writer)
    {
        if (!m_keyed && !m_stopPending) {
            return true;
        }
        m_keyed = false;
        m_stopPending = true;
        const TxCoordinator::Dispatch dispatch = m_operation.beginDispatch(now, false);
        if (!dispatch || !writer(false)) {
            return false;
        }
        m_stopPending = false;
        m_operation = {};
        return true;
    }

    bool m_enabled = false;
    bool m_keyed = false;
    bool m_stopPending = false;
    TxCoordinator::Operation m_operation;
};

} // namespace AetherSDR::anan
