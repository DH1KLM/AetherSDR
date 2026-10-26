#include "core/backends/anan/P1Protocol.h"
#include "core/backends/anan/AnanReceiveCalibration.h"

#include <algorithm>

namespace AetherSDR::anan::p1 {
namespace {
bool validReceiverCount(int count) noexcept { return count >= 1 && count <= 7; }

void put32(std::uint8_t* destination, std::uint32_t value) noexcept
{
    for (int i = 0; i < 4; ++i)
        destination[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

float sample24(const std::uint8_t* bytes) noexcept
{
    const auto unsignedValue = (std::uint32_t{bytes[0]} << 16)
        | (std::uint32_t{bytes[1]} << 8) | bytes[2];
    const auto signedValue = static_cast<std::int32_t>(unsignedValue & 0x7fffff)
        - static_cast<std::int32_t>(unsignedValue & 0x800000);
    return static_cast<float>(signedValue) / 8388607.0f;
}
} // namespace

bool validConfig(const ReceiveConfig& c) noexcept
{
    const bool rate = c.rateKsps == 48 || c.rateKsps == 96
        || c.rateKsps == 192 || c.rateKsps == 384;
    if (c.independentFrequencies &&
        std::any_of(c.receiveFrequencies.begin(), c.receiveFrequencies.end(),
                    [](auto hz) { return hz > 61'440'000; }))
        return false;
    return rate && validFrequencyFactor(c.frequencyCorrection) && validReceiverCount(c.receivers)
        && (c.numAdcs == 1 || c.numAdcs == 2)
        && c.adcIndex >= 0 && c.adcIndex < c.numAdcs
        && c.rxOcMask <= 127
        && !c.mic.xlr
        && c.userDigitalOutputs <= 15
        && c.antenna >= 1 && c.antenna <= 3
        && c.rxOnlyInput >= 0 && c.rxOnlyInput <= 3
        && c.alexAttenuationDb >= 0 && c.alexAttenuationDb <= 30
        && c.alexAttenuationDb % 10 == 0
        && (!c.mkIIBpf || c.alexAttenuationDb == 0)
        && c.frequencyHz <= 61'440'000
        && c.adc0AttenuationDb >= 0 && c.adc0AttenuationDb <= 31
        && c.adc1AttenuationDb >= 0 && c.adc1AttenuationDb <= 31
        && (c.receiveFilterBits & 0x80) == 0 && (c.adc1ReceiveFilterBits & 0x80) == 0
        && (c.lowPassBits & 0x80) == 0;
}

std::optional<Control> receiveControl(std::uint8_t address,
                                     const ReceiveConfig& c) noexcept
{
    if (!validConfig(c))
        return std::nullopt;
    Control bytes{address, 0, 0, 0, 0};
    if (address >= 0x04 && address <= 0x10 && (address & 1) == 0) {
        const int slot = (address - 0x04) / 2;
        if (slot >= c.receivers)
            return std::nullopt;
        put32(bytes.data() + 1,
              static_cast<std::uint32_t>((c.independentFrequencies ? c.receiveFrequencies[slot] : c.frequencyHz)
                                         * c.frequencyCorrection));
        return bytes;
    }
    switch (address) {
    case 0x26:
        if (!c.anvelinaExtraOc) { return std::nullopt; }
        break; // Anvelina-only extra OC bank remains all-zero in RX.
    case 0x00:
        bytes[1] = c.rateKsps == 48 ? 0 : c.rateKsps == 96 ? 1
            : c.rateKsps == 192 ? 2 : 3;
        bytes[2] = static_cast<std::uint8_t>(c.rxOcMask << 1);
        bytes[3] = static_cast<std::uint8_t>(c.alexAttenuationDb / 10);
        bytes[3] |= (c.dither ? 0x08 : 0) | (c.random ? 0x10 : 0);
        // Thetis bank-0 C3[6:5] input, C3[7] RX_OUT, or C3[0]
        // MkII RX master selection. These relays never set MOX.
        bytes[3] |= static_cast<std::uint8_t>(c.rxOnlyInput << 5);
        if (c.rxOnlyInput != 0) {
            bytes[3] |= c.mkIIBpf && c.rxOnlyInput != 1 ? 0x01 : 0x80;
        }
        bytes[4] = static_cast<std::uint8_t>((c.antenna - 1) | 0x04
            | ((c.receivers - 1) << 3));
        break;
    case 0x12:
        // Manual receive filters, zero drive, and Disable HF PA.
        bytes[2] = 0x40;
        bytes[3] = c.receiveFilterBits | 0x80;
        bytes[4] = c.lowPassBits;
        break;
    case 0x14:
        // Thetis bank 10 C1[4:6]: ring, bias and inverted microphone-PTT permission.
        bytes[1] = (c.mic.ring ? 0x10 : 0) | (c.mic.bias ? 0x20 : 0) | (c.mic.ptt ? 0 : 0x40);
        // Thetis bank 11 C3 carries only the four independent user outputs.
        bytes[3] = c.userDigitalOutputs;
        bytes[4] = static_cast<std::uint8_t>(0x20 | c.adc0AttenuationDb);
        break;
    case 0x16:
        if (c.numAdcs == 2)
            bytes[1] = static_cast<std::uint8_t>(0x20 | c.adc1AttenuationDb);
        break;
    case 0x1c:
        for (int slot = 0; slot < c.receivers; ++slot)
            bytes[1 + slot / 4] |= static_cast<std::uint8_t>(c.adcIndex << (2 * (slot % 4)));
        break;
    case 0x1e:
        break; // CW engine disabled.
    case 0x24:
        if (c.numAdcs == 2)
            bytes[1] = c.adc1ReceiveFilterBits;
        break;
    default:
        return std::nullopt;
    }
    return bytes;
}

std::array<std::uint8_t, 63> discoveryRequest() noexcept
{
    std::array<std::uint8_t, 63> packet{};
    packet[0] = 0xef; packet[1] = 0xfe; packet[2] = 0x02;
    return packet;
}

std::array<std::uint8_t, 64> streamCommand(bool run) noexcept
{
    std::array<std::uint8_t, 64> packet{};
    packet[0] = 0xef; packet[1] = 0xfe; packet[2] = 0x04;
    packet[3] = run ? 0x01 : 0x00;
    return packet;
}

std::optional<DiscoveryReply> parseDiscovery(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < 11 || bytes.size() > 1032
        || bytes[0] != 0xef || bytes[1] != 0xfe
        || (bytes[2] != 0x02 && bytes[2] != 0x03))
        return std::nullopt;
    DiscoveryReply reply;
    std::copy_n(bytes.begin() + 3, 6, reply.mac.begin());
    if ((reply.mac[0] & 1) != 0
        || std::all_of(reply.mac.begin(), reply.mac.end(), [](auto b) { return b == 0; }))
        return std::nullopt;
    reply.streaming = bytes[2] == 0x03;
    reply.firmware = bytes[9];
    reply.boardId = bytes[10];
    return reply;
}

std::optional<int> normalizedBoardId(int wireBoardId) noexcept
{
    switch (wireBoardId) {
    case 0: case 1: case 2: case 6: case 20: return wireBoardId;
    case 4: return 3;
    case 5: return 4;
    case 10: return 5;
    default: return std::nullopt;
    }
}

std::optional<Packet> speakerAndControl(std::uint32_t sequence,
    std::uint8_t firstAddress, std::uint8_t secondAddress,
    const ReceiveConfig& config, std::span<const std::int16_t> audio,
    bool swapChannels) noexcept
{
    const auto first = receiveControl(firstAddress, config);
    const auto second = receiveControl(secondAddress, config);
    if (!first || !second)
        return std::nullopt;
    Packet packet{};
    packet[0] = 0xef; packet[1] = 0xfe; packet[2] = 0x01; packet[3] = 0x02;
    put32(packet.data() + 4, sequence);
    for (int frame = 0; frame < 2; ++frame) {
        const int base = 8 + frame * 512;
        std::fill_n(packet.begin() + base, 3, 0x7f);
        const auto& control = frame == 0 ? *first : *second;
        std::copy(control.begin(), control.end(), packet.begin() + base + 3);
        for (int round = 0; round < 63; ++round) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto index = static_cast<std::size_t>(2 * (frame * 63 + round)
                    + (swapChannels ? 1 - channel : channel));
                if (index >= audio.size())
                    continue;
                const auto sample = static_cast<std::uint16_t>(audio[index]);
                const int offset = base + 8 + round * 8 + channel * 2;
                packet[offset] = static_cast<std::uint8_t>(sample >> 8);
                packet[offset + 1] = static_cast<std::uint8_t>(sample);
            }
        }
    }
    return packet;
}

std::optional<IqFrame> parseIq(std::span<const std::uint8_t> bytes, int receivers) noexcept
{
    if (!validReceiverCount(receivers) || bytes.size() != kPacketBytes
        || bytes[0] != 0xef || bytes[1] != 0xfe || bytes[2] != 0x01 || bytes[3] != 0x06)
        return std::nullopt;
    IqFrame result;
    result.receivers = receivers;
    result.roundsPerFrame = 504 / (6 * receivers + 2);
    for (int i = 4; i < 8; ++i)
        result.sequence = (result.sequence << 8) | bytes[i];
    for (int frame = 0; frame < 2; ++frame) {
        const auto base = static_cast<std::size_t>(8 + 512 * frame);
        if (bytes[base] != 0x7f || bytes[base + 1] != 0x7f || bytes[base + 2] != 0x7f)
            return std::nullopt;
        std::copy_n(bytes.begin() + base + 3, 5, result.status[frame].begin());
        result.payload[frame] = bytes.subspan(base + 8, 504);
    }
    return result;
}

bool decodeIq(const IqFrame& frame, int slot, std::vector<std::complex<float>>& out)
{
    if (!validReceiverCount(frame.receivers) || slot < 0 || slot >= frame.receivers
        || frame.roundsPerFrame != 504 / (6 * frame.receivers + 2)
        || frame.payload[0].size() != 504 || frame.payload[1].size() != 504)
        return false;
    out.reserve(out.size() + 2 * frame.roundsPerFrame);
    for (const auto payload : frame.payload) {
        for (int round = 0; round < frame.roundsPerFrame; ++round) {
            const auto* sample = payload.data() + round * (6 * frame.receivers + 2) + slot * 6;
            out.emplace_back(sample24(sample), sample24(sample + 3));
        }
    }
    return true;
}
} // namespace AetherSDR::anan::p1
