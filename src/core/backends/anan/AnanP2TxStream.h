#pragma once

#include "AnanTxGate.h"
#include "P2Protocol.h"

#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <span>

namespace AetherSDR::anan {

// I/O-thread packetizer, with an injected monotonic clock and terminal writer.
// Due packets retain the sample clock within the bounded 20 ms queue window.
class AnanP2TxStream final {
public:
    static constexpr qint64 kPacketNs = 1'250'000; // 240 / 192000 seconds
    static constexpr int kMaximumPacketsPerTick = 16;
    static constexpr std::size_t kCapacity = kMaximumPacketsPerTick * kTxIqSamplesPerPacket;
    enum class Result { NotDue, Sent, Silence, Refused, WriteFailed };

    bool begin(const TxCoordinator::Operation& operation,
               const TxCoordinator::Context& context, qint64 nowNs, qint64 nowMs)
    {
        clear();
        if (nowNs < 0 || !operation.permitsDispatch(nowMs)
            || !context.permitsDispatch(nowMs)) {
            return false;
        }
        m_operation = operation;
        m_context = context;
        m_nextNs = nowNs;
        m_lastNs = nowNs;
        m_active = true;
        return true;
    }

    bool enqueue(std::span<const std::complex<float>> iq,
                 const TxCoordinator::Context& context, qint64 nowMs)
    {
        if (!m_active || !context.sameContext(m_context)
            || !context.permitsDispatch(nowMs) || !m_operation.permitsDispatch(nowMs)
            || iq.empty() || iq.size() > kCapacity - m_count) {
            clear();
            return false;
        }
        for (const std::complex<float>& sample : iq) {
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())
                || std::abs(sample) > 1.0f) {
                clear();
                return false;
            }
        }
        for (const std::complex<float>& sample : iq) {
            m_samples[(m_head + m_count++) % kCapacity] = sample;
        }
        return true;
    }

    bool beginCarrier(const TxCoordinator::Operation& operation, qint64 nowNs, qint64 nowMs)
    {
        clear();
        if (nowNs < 0 || !operation.permitsDispatch(nowMs)) { return false; }
        m_operation = operation; m_nextNs = nowNs; m_lastNs = nowNs; m_active = true;
        return true;
    }

    template<class Generator, class Writer>
    Result pumpCarrier(qint64 nowNs, qint64 nowMs, AnanTxGate& gate, Generator&& generator, Writer&& writer)
    {
        if (!m_active || nowNs < m_lastNs || nowNs > std::numeric_limits<qint64>::max() - kPacketNs
            || (nowNs >= m_nextNs && nowNs - m_nextNs >= kMaximumPacketsPerTick * kPacketNs)
            || !m_operation.permitsDispatch(nowMs)) { clear(); return Result::Refused; }
        m_lastNs = nowNs;
        if (nowNs < m_nextNs) { return Result::NotDue; }
        for (int due = 0; due < kMaximumPacketsPerTick && nowNs >= m_nextNs; ++due) {
            std::array<std::complex<float>,kTxIqSamplesPerPacket> samples{};
            for (auto& sample : samples) { sample = generator(); }
            const auto packet = buildTxIq(samples,m_sequence++);
            if (!packet || !gate.writeCarrier(m_operation,nowMs,[&] { return writer(*packet); })) {
                clear(); return Result::WriteFailed;
            }
            m_nextNs += kPacketNs;
        }
        return Result::Sent;
    }

    template<class Writer>
    Result pump(qint64 nowNs, qint64 nowMs, AnanTxGate& gate, Writer&& writer)
    {
        if (!m_active || nowNs < m_lastNs || nowNs > std::numeric_limits<qint64>::max() - kPacketNs
            || (nowNs >= m_nextNs && nowNs - m_nextNs >= kMaximumPacketsPerTick * kPacketNs)
            || !m_operation.permitsDispatch(nowMs) || !m_context.permitsDispatch(nowMs)) {
            clear();
            return Result::Refused;
        }
        m_lastNs = nowNs;
        if (nowNs < m_nextNs) { return Result::NotDue; }
        bool underflow = false;
        for (int due = 0; due < kMaximumPacketsPerTick && nowNs >= m_nextNs; ++due) {
            std::array<std::complex<float>, kTxIqSamplesPerPacket> samples{};
            const bool silence = m_count < samples.size();
            underflow = underflow || silence;
            if (!silence) {
                for (std::complex<float>& sample : samples) {
                    sample = m_samples[m_head];
                    m_head = (m_head + 1) % kCapacity;
                    --m_count;
                }
            }
            const std::optional<TxIqPacket> packet = buildTxIq(samples, m_sequence++);
            if (!packet || !gate.writeIq(m_operation, m_context, nowMs, [&] { return writer(*packet); })) {
                clear();
                return Result::WriteFailed;
            }
            m_nextNs += kPacketNs;
        }
        return underflow ? Result::Silence : Result::Sent;
    }

    void clear() noexcept
    {
        m_active = false;
        m_count = 0;
        m_head = 0;
        m_operation = {};
        m_context = {};
    }
    void transportClosed() noexcept { clear(); m_sequence = 0; }
    [[nodiscard]] std::size_t queuedSamples() const noexcept { return m_count; }

private:
    std::array<std::complex<float>, kCapacity> m_samples{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    std::uint32_t m_sequence = 0;
    bool m_active = false;
    qint64 m_nextNs = 0;
    qint64 m_lastNs = 0;
    TxCoordinator::Operation m_operation;
    TxCoordinator::Context m_context;
};
} // namespace AetherSDR::anan
