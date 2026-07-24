#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// HPSDR Protocol 1 ("Metis") wire primitives for the Hermes-Lite 2 backend.
//
// Direct C++ port of the live-validated prototypes/hl2/hpsdr.py spike (aetherd
// HL2 Phase 1a). Protocol facts (register map, EP2/EP6 framing, the
// CONFIG_MERCURY ADC-select bit, LNA gain register) are grounded clean-room in
// openHPSDR Protocol 1, the Hermes-Lite 2 wiki/gateware, and the pihpsdr
// reference client (Principle I; see THIRD_PARTY_LICENSES).
//
// This layer is intentionally socket-free and Qt-free so it unit-tests against
// captured/synthetic frames without hardware; MetisClient owns the UDP socket
// and RX thread and calls into these functions.
//
// RX-ONLY BY CONSTRUCTION: every C0 register-address byte here is even, so the
// MOX bit (C0 bit 0) is always 0 — these primitives cannot key the radio.

namespace AetherSDR::hl2 {

inline constexpr std::uint16_t kMetisPort = 1024;
inline constexpr int kFullScale = 1 << 23;   // 24-bit signed full scale (8388608)

// EP2 (host->radio) and EP6 (radio->host) are both 1032-byte USB-over-IP frames:
//   EF FE 01 <ep> | seq[4] | frame512 | frame512
// each 512-byte frame: 7F 7F 7F | C0 C1 C2 C3 C4 | 504 payload bytes
inline constexpr std::size_t kUsbPacketSize = 1032;
inline constexpr std::size_t kFrameSize = 512;
inline constexpr std::size_t kFramePayload = 504;     // 63 RX samples * 8 bytes
inline constexpr std::size_t kRxSampleBytes = 8;      // I[3] Q[3] mic[2], 24-bit BE
inline constexpr int kSamplesPerPacket = 126;         // 63 per frame * 2 frames

// C0 register-address bytes (address << 1, MOX=0). Odd values (TX NCO C0=0x02)
// are deliberately absent — this backend never encodes them.
inline constexpr std::uint8_t kC0Config = 0x00;   // addr 0x00: sample rate + #RX + ADC select
inline constexpr std::uint8_t kC0Rx1Freq = 0x04;  // addr 0x02: RX1 NCO frequency (Hz, 32-bit BE)
inline constexpr std::uint8_t kC0AdcGain = 0x14;  // addr 0x0a: AD9866 LNA gain

// Config-register (C0=0x00) bit flags.
inline constexpr std::uint8_t kConfigMercury = 0x40;  // C1 bit6: select the ADC as the DDC
                                                      // source. WITHOUT it the stream is flat
                                                      // ADC-floor noise (the non-obvious must-set).
inline constexpr std::uint8_t kConfigDuplex = 0x04;   // C4 bit2: pihpsdr sets this unconditionally

enum class SampleRate : std::uint8_t { R48k = 0, R96k = 1, R192k = 2, R384k = 3 };
int sampleRateHz(SampleRate rate) noexcept;

// A 5-byte Command & Control payload: C0 (register address) + C1..C4 (data).
using Cc = std::array<std::uint8_t, 5>;

// Config register: sample rate + receiver count, with CONFIG_MERCURY + duplex.
Cc ccConfig(SampleRate rate, int numRx = 1) noexcept;
// RX1 NCO frequency in Hz (32-bit big-endian across C1..C4).
Cc ccRx1Freq(std::uint32_t hz) noexcept;
// AD9866 LNA gain in dB, clamped to [-12, +48]; C4 = 0x40 | (dB + 12).
Cc ccRxGain(int db) noexcept;

// 64-byte Metis command: EF FE 04 <cmd>. cmd 0x01 = start IQ, 0x00 = stop.
std::array<std::uint8_t, 64> metisCommand(std::uint8_t cmd) noexcept;
// Bit 7 of the run/stop byte is the gateware's watchdog_disable flag
// (Hermes-Lite 2 gateware, rtl/dsopenhpsdr1.v — see THIRD_PARTY_LICENSES):
// 0 = watchdog ENABLED, 0x80 = disabled. We default to ENABLED, which is the
// anti-wedge mechanism: if this client dies without sending a stop, EP2 traffic
// ceases and the radio halts its own stream instead of streaming forever at a
// dead endpoint (after which it stops answering discovery until power-cycled).
inline constexpr std::uint8_t kRunWatchdogDisable = 0x80;

inline std::array<std::uint8_t, 64> metisStart(bool watchdogEnabled = true) noexcept
{
    return metisCommand(static_cast<std::uint8_t>(
        0x01 | (watchdogEnabled ? 0x00 : kRunWatchdogDisable)));
}
inline std::array<std::uint8_t, 64> metisStop(bool watchdogEnabled = true) noexcept
{
    return metisCommand(static_cast<std::uint8_t>(
        0x00 | (watchdogEnabled ? 0x00 : kRunWatchdogDisable)));
}

// 63-byte discovery request: EF FE 02 + 60 zero bytes (broadcast to :1024).
std::array<std::uint8_t, 63> discoveryRequest() noexcept;

struct DiscoveryReply {
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t gatewareVersion = 0;   // raw byte; HL2 gateware e.g. 0x4A -> 7.4
    std::uint8_t boardId = 0;           // 0x06 = Hermes-Lite / Hermes-Lite 2
    bool streaming = false;             // discovery status byte 0x03 = already sending IQ
    // Receiver count the board reports. Only present on full-length replies
    // (>= 21 bytes); 0 means "not reported" and callers should assume 1.
    std::uint8_t numRx = 0;
    [[nodiscard]] bool isHermesLite2() const noexcept { return boardId == 0x06; }
};
// Parse a >=60-byte Metis discovery reply (EF FE <st> MAC[6] gwver board ...).
std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> pkt) noexcept;

// Build a 1032-byte EP2 packet carrying two C&C registers (one per frame). The
// 504-byte TX payload is all-zero (RX-only).
std::array<std::uint8_t, kUsbPacketSize> ep2Packet(std::uint32_t seq, const Cc& a,
                                                   const Cc& b) noexcept;

// Cheap header read: the EP6 sequence number, or nullopt if not an EP6 packet.
// Used for drop counting without decoding samples.
std::optional<std::uint32_t> ep6Seq(std::span<const std::uint8_t> pkt) noexcept;

// Decode an EP6 packet's IQ samples (24-bit signed big-endian, normalized to
// [-1, 1)) and append them to `out`. Returns the count appended, or -1 if `pkt`
// is not a valid EP6 packet (wrong length/header). Does not remove the DC
// offset — that is the DSP layer's job.
int ep6Samples(std::span<const std::uint8_t> pkt,
               std::vector<std::complex<float>>& out) noexcept;

}  // namespace AetherSDR::hl2
