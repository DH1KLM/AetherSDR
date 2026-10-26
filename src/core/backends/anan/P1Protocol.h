#pragma once
#include "AnanMicHardware.h"

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

//DH1KLM: Independent, socket-free OpenHPSDR Protocol 1 receive codec.
// Wire facts and source revisions: docs/architecture/anan-p1-codec.md.
// No MOX, transmit frequency, transmit IQ, drive or CW encoder is exposed.
namespace AetherSDR::anan::p1 {

inline constexpr std::size_t kPacketBytes = 1032;
inline constexpr int kSpeakerFrames = 126;
using Control = std::array<std::uint8_t, 5>;
using Packet = std::array<std::uint8_t, kPacketBytes>;

struct ReceiveConfig {
    bool anvelinaExtraOc = false;
    int rateKsps = 48;
    int receivers = 1;
    int numAdcs = 1;
    int adcIndex = 0;
    std::uint32_t frequencyHz = 10'000'000;
    bool independentFrequencies = false;
    std::array<std::uint32_t, 7> receiveFrequencies{};
    int antenna = 1;
    int rxOnlyInput = 0;
    bool mkIIBpf = false;
    MicOptions mic;
    std::uint8_t rxOcMask = 0;
    std::uint8_t userDigitalOutputs = 0;
    double frequencyCorrection = 1.0;
    bool dither = false;
    bool random = false;
    int alexAttenuationDb = 0; // Classic ALEX only: 0, 10, 20 or 30 dB.
    int adc0AttenuationDb = 0;
    int adc1AttenuationDb = 0;
    // P1 filter-bank bits, not the P2 Alex-word bit positions.
    std::uint8_t receiveFilterBits = 0x20;
    std::uint8_t adc1ReceiveFilterBits = 0x20;
    std::uint8_t lowPassBits = 0x10;
};

[[nodiscard]] bool validConfig(const ReceiveConfig& config) noexcept;
// The supported RX-only C0 addresses are 00,04..10,12,14,16,1c,1e,24.
// Unsupported addresses and invalid configurations fail instead of aliasing
// another bank. Routing assigns every enabled slot to the selected ADC.
[[nodiscard]] std::optional<Control> receiveControl(
    std::uint8_t address, const ReceiveConfig& config) noexcept;

[[nodiscard]] std::array<std::uint8_t, 63> discoveryRequest() noexcept;
[[nodiscard]] std::array<std::uint8_t, 64> streamCommand(bool run) noexcept;

struct DiscoveryReply {
    bool streaming = false;
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t firmware = 0;
    std::uint8_t boardId = 0;
};
[[nodiscard]] std::optional<DiscoveryReply> parseDiscovery(
    std::span<const std::uint8_t> bytes) noexcept;
//DH1KLM: P1 wire IDs differ from the common Thetis/P2 registry: 4=Angelia,
// 5=Orion, 10=Orion MkII. Unknown IDs must not inherit a P2 interpretation.
[[nodiscard]] std::optional<int> normalizedBoardId(int wireBoardId) noexcept;

// Encodes controls internally: callers cannot insert arbitrary keying bytes.
// The speaker payload is L,R int16 BE; all transmit-IQ slots stay zero.
// Missing samples are silence; surplus samples are ignored.
[[nodiscard]] std::optional<Packet> speakerAndControl(
    std::uint32_t sequence, std::uint8_t firstAddress,
    std::uint8_t secondAddress, const ReceiveConfig& config,
    std::span<const std::int16_t> audio = {}, bool swapChannels = false) noexcept;

struct IqFrame {
    std::uint32_t sequence = 0;
    int receivers = 0;
    int roundsPerFrame = 0;
    std::array<Control, 2> status{};
    std::array<std::span<const std::uint8_t>, 2> payload{};
};
// Views borrow the input datagram. Both frame syncs and exact datagram length
// are validated before any IQ is exposed. Each round ends with a mic word.
[[nodiscard]] std::optional<IqFrame> parseIq(
    std::span<const std::uint8_t> bytes, int receivers) noexcept;
// Invalid slot / inconsistent geometry leaves the output unchanged.
[[nodiscard]] bool decodeIq(const IqFrame& frame, int slot,
                           std::vector<std::complex<float>>& out);

} // namespace AetherSDR::anan::p1
