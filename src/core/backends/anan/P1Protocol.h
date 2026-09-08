#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR::anan::p1 {

//DH1KLM: OpenHPSDR Protocol 1 (Metis) wire primitives for the standard
//DH1KLM: non-Hermes-Lite boards. Stage 1 targets Orion / ANAN-200D.
//DH1KLM: This file deliberately contains no UDP/socket code. Transport belongs
//DH1KLM: to the future P1 client, while this layer remains deterministic and testable.

inline constexpr std::uint16_t kMetisPort = 1024;
inline constexpr std::size_t kUsbPacketSize = 1032;
inline constexpr std::size_t kFrameSize = 512;
inline constexpr std::size_t kFramePayload = 504;
inline constexpr std::uint8_t kSync = 0x7F;
inline constexpr std::uint32_t kDspClockHz = 122880000u;
inline constexpr int kFullScale24 = (1 << 23) - 1;

//DH1KLM: Metis packet types used by the standard OpenHPSDR Protocol 1 path.
enum class Endpoint : std::uint8_t {
    Ep2 = 0x02,
    Ep6 = 0x06
};

//DH1KLM: A 5-byte C&C register command: C0 register address followed by C1..C4.
using Cc = std::array<std::uint8_t, 5>;

//DH1KLM: Parsed discovery information. Board identity is intentionally kept as
//DH1KLM: the raw wire value; AnanHardware performs the board/model mapping.
struct DiscoveryReply {
    bool streaming = false;
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t gatewareVersion = 0;
    std::uint8_t boardId = 0;
    std::uint8_t numRx = 0;
};

//DH1KLM: Result of decoding one standard EP6 telemetry/C&C response frame.
struct Ep6Response {
    bool ack = false;
    std::uint8_t raddr = 0;
    bool dot = false;
    bool ptt = false;
    std::uint32_t data = 0;
};

//DH1KLM: Convert a frequency in Hz into the 32-bit NCO phase word used by
//DH1KLM: OpenHPSDR Protocol 1. The calculation is kept integer/deterministic.
std::uint32_t frequencyToPhaseWord(std::uint64_t frequencyHz) noexcept;

//DH1KLM: Convert a Protocol 1 phase word back into an approximate frequency.
std::uint64_t phaseWordToFrequency(std::uint32_t phaseWord) noexcept;

//DH1KLM: Construct the standard 63-byte OpenHPSDR discovery request.
std::array<std::uint8_t, 63> discoveryRequest() noexcept;

//DH1KLM: Parse the common Protocol 1 discovery response. Short packets are
//DH1KLM: accepted; fields not present remain at their zero defaults.
std::optional<DiscoveryReply> parseDiscoveryReply(
    std::span<const std::uint8_t> packet) noexcept;

//DH1KLM: Create a standard 64-byte Metis command packet.
std::array<std::uint8_t, 64> metisCommand(std::uint8_t command) noexcept;

//DH1KLM: Build one 1032-byte EP2 packet containing two C&C frames.
//DH1KLM: Payload is zero-filled because Stage 1 initially validates control and
//DH1KLM: framing independently from the later TX-IQ streaming implementation.
std::array<std::uint8_t, kUsbPacketSize> ep2ControlPacket(
    std::uint32_t sequence,
    const Cc& first,
    const Cc& second) noexcept;

//DH1KLM: Build a standard C&C configuration command for sample rate and RX count.
//DH1KLM: The register encoding follows the standard openHPSDR Protocol 1 layout,
//DH1KLM: not the Hermes-Lite-specific TX-gain interpretation.
Cc ccConfig(std::uint8_t sampleRateCode, int receiverCount) noexcept;

//DH1KLM: Build an RX NCO frequency command. Orion RX1..RX7 occupy consecutive
//DH1KLM: standard Protocol 1 frequency registers.
Cc ccRxFrequency(int receiverIndex, std::uint32_t frequencyHz) noexcept;

//DH1KLM: Build the TX NCO frequency command.
Cc ccTxFrequency(std::uint32_t frequencyHz) noexcept;

//DH1KLM: Build the standard ADC assignment command. For Stage 1 the ANAN-200D
//DH1KLM: starts with RX1 assigned to ADC0; additional routing will be added when
//DH1KLM: the receiver manager is connected to the ANAN capability registry.
Cc ccAdcAssign() noexcept;

//DH1KLM: Set or clear MOX on a C&C command. MOX is C0 bit 0 and is therefore
//DH1KLM: applied to the currently transmitted C&C bank rather than to a separate
//DH1KLM: register.
Cc withMox(Cc command, bool keyed) noexcept;

//DH1KLM: Decode the 32-bit big-endian payload of an EP6 C&C response.
std::optional<Ep6Response> parseEp6Response(
    std::span<const std::uint8_t> frame) noexcept;

//DH1KLM: Return the EP6 sequence number when the packet header is valid.
std::optional<std::uint32_t> ep6Sequence(
    std::span<const std::uint8_t> packet) noexcept;

//DH1KLM: Decode one receiver's 24-bit signed big-endian IQ samples from an EP6
//DH1KLM: packet. The returned count is the number of samples appended.
int decodeEp6Samples(
    std::span<const std::uint8_t> packet,
    std::vector<std::complex<float>>& output) noexcept;

//DH1KLM: Decode multiple standard Protocol 1 receiver streams. Each vector in
//DH1KLM: `outputs` corresponds to one receiver in the wire round.
int decodeEp6SamplesMulti(
    std::span<const std::uint8_t> packet,
    std::span<std::vector<std::complex<float>>> outputs) noexcept;

} // namespace AetherSDR::anan::p1
