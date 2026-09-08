#include "P1Protocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::anan::p1 {

namespace {

inline std::uint32_t readBe32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 24)
         | (std::uint32_t(p[1]) << 16)
         | (std::uint32_t(p[2]) << 8)
         | std::uint32_t(p[3]);
}

inline void writeBe32(std::uint8_t* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::int32_t decode24be(const std::uint8_t* p) noexcept
{
    std::int32_t value = (std::int32_t(p[0]) << 16)
                       | (std::int32_t(p[1]) << 8)
                       | std::int32_t(p[2]);

    if (value & 0x00800000)
        value |= static_cast<std::int32_t>(0xFF000000u);

    return value;
}

inline bool validEp6Packet(std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < kUsbPacketSize)
        return false;

    return packet[0] == 0xEF
        && packet[1] == 0xFE
        && packet[2] == 0x01
        && packet[3] == static_cast<std::uint8_t>(Endpoint::Ep6);
}

inline bool validFrameSync(const std::uint8_t* frame) noexcept
{
    return frame[0] == kSync && frame[1] == kSync && frame[2] == kSync;
}

} // namespace

std::uint32_t frequencyToPhaseWord(std::uint64_t frequencyHz) noexcept
{
    //DH1KLM: Use 64-bit arithmetic so normal HF frequencies retain the full
    //DH1KLM: precision of the 32-bit phase accumulator without floating-point
    //DH1KLM: rounding differences between compilers.
    const std::uint64_t scaled =
        (frequencyHz * (std::uint64_t{1} << 32)) / kDspClockHz;

    return static_cast<std::uint32_t>(scaled & 0xFFFFFFFFu);
}

std::uint64_t phaseWordToFrequency(std::uint32_t phaseWord) noexcept
{
    return (static_cast<std::uint64_t>(phaseWord) * kDspClockHz)
         >> 32;
}

std::array<std::uint8_t, 63> discoveryRequest() noexcept
{
    std::array<std::uint8_t, 63> packet{};
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x02;
    //DH1KLM: A non-zero byte 4 prevents a Protocol 2 General_CC parser on UDP
    //DH1KLM: port 1024 from claiming the Protocol 1 discovery probe. Thetis/
    //DH1KLM: NereusSDR use 0xFF for this safety pad.
    packet[4] = 0xFF;
    return packet;
}

std::optional<DiscoveryReply> parseDiscoveryReply(
    std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < 11)
        return std::nullopt;

    if (packet[0] != 0xEF || packet[1] != 0xFE)
        return std::nullopt;

    if (packet[2] != 0x02 && packet[2] != 0x03)
        return std::nullopt;

    DiscoveryReply result;
    result.streaming = packet[2] == 0x03;

    for (std::size_t i = 0; i < result.mac.size(); ++i)
        result.mac[i] = packet[3 + i];

    result.gatewareVersion = packet[9];
    result.boardId = packet[10];

    //DH1KLM: Standard Protocol 1 discovery stores the hardware receiver count
    //DH1KLM: at offset 0x13 (decimal 19). Do not use offset 0x14: that byte is
    //DH1KLM: part of the following board/build information.
    if (packet.size() > 19)
        result.numRx = packet[19];

    return result;
}

std::array<std::uint8_t, 64> metisCommand(std::uint8_t command) noexcept
{
    std::array<std::uint8_t, 64> packet{};
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x04;
    packet[3] = command;
    return packet;
}

std::array<std::uint8_t, kUsbPacketSize> ep2ControlPacket(
    std::uint32_t sequence,
    const Cc& first,
    const Cc& second) noexcept
{
    std::array<std::uint8_t, kUsbPacketSize> packet{};

    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x01;
    packet[3] = static_cast<std::uint8_t>(Endpoint::Ep2);
    writeBe32(packet.data() + 4, sequence);

    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    const Cc* commands[2] = {&first, &second};

    for (int frame = 0; frame < 2; ++frame) {
        auto* destination = packet.data() + frameStarts[frame];

        destination[0] = kSync;
        destination[1] = kSync;
        destination[2] = kSync;

        for (std::size_t i = 0; i < 5; ++i)
            destination[3 + i] = (*commands[frame])[i];
    }

    return packet;
}

Cc ccConfig(std::uint8_t sampleRateCode, int receiverCount) noexcept
{
    //DH1KLM: Standard OpenHPSDR P1 encodes the sample-rate selection in C1 and
    //DH1KLM: the receiver count as (count-1) in C4[6:3]. The duplex bit is C4[2].
    const int clampedRx = std::clamp(receiverCount, 1, 12);

    return {
        0x00,
        static_cast<std::uint8_t>(sampleRateCode & 0x03),
        0x00,
        0x00,
        static_cast<std::uint8_t>(
            0x04 | ((clampedRx - 1) & 0x0F) << 3)
    };
}

Cc ccRxFrequency(int receiverIndex, std::uint32_t frequencyHz) noexcept
{
    //DH1KLM: Standard Orion/Hermes-family P1 maps RX1..RX7 to registers
    //DH1KLM: 0x02..0x08. C0 contains the register address shifted left once.
    const int index = std::clamp(receiverIndex, 0, 6);
    const std::uint8_t c0 =
        static_cast<std::uint8_t>(0x04 + (index << 1));

    return {
        c0,
        static_cast<std::uint8_t>((frequencyHz >> 24) & 0xFF),
        static_cast<std::uint8_t>((frequencyHz >> 16) & 0xFF),
        static_cast<std::uint8_t>((frequencyHz >> 8) & 0xFF),
        static_cast<std::uint8_t>(frequencyHz & 0xFF)
    };
}

Cc ccTxFrequency(std::uint32_t frequencyHz) noexcept
{
    return {
        0x02,
        static_cast<std::uint8_t>((frequencyHz >> 24) & 0xFF),
        static_cast<std::uint8_t>((frequencyHz >> 16) & 0xFF),
        static_cast<std::uint8_t>((frequencyHz >> 8) & 0xFF),
        static_cast<std::uint8_t>(frequencyHz & 0xFF)
    };
}

Cc ccAdcAssign() noexcept
{
    //DH1KLM: The standard ADC-assignment command is intentionally kept separate
    //DH1KLM: from the HL2 TX-gain interpretation of the same register address.
    return {0x1C, 0x00, 0x00, 0x00, 0x00};
}

Cc withMox(Cc command, bool keyed) noexcept
{
    if (keyed)
        command[0] |= 0x01;
    else
        command[0] &= static_cast<std::uint8_t>(~0x01u);

    return command;
}

std::optional<Ep6Response> parseEp6Response(
    std::span<const std::uint8_t> frame) noexcept
{
    if (frame.size() < 8)
        return std::nullopt;

    if (!validFrameSync(frame.data()))
        return std::nullopt;

    Ep6Response result;
    const std::uint8_t c0 = frame[3];

    result.ack = (c0 & 0x80) != 0;

    if (result.ack) {
        result.raddr = static_cast<std::uint8_t>((c0 >> 1) & 0x3F);
    } else {
        result.raddr = static_cast<std::uint8_t>((c0 >> 3) & 0x0F);
        result.dot = (c0 & 0x04) != 0;
    }

    result.ptt = (c0 & 0x01) != 0;
    result.data = readBe32(frame.data() + 4);

    return result;
}

std::optional<std::uint32_t> ep6Sequence(
    std::span<const std::uint8_t> packet) noexcept
{
    if (!validEp6Packet(packet))
        return std::nullopt;

    return readBe32(packet.data() + 4);
}

int decodeEp6Samples(
    std::span<const std::uint8_t> packet,
    std::vector<std::complex<float>>& output) noexcept
{
    std::array<std::vector<std::complex<float>>, 1> receivers;
    const int result = decodeEp6SamplesMulti(packet, receivers);

    if (result > 0)
        output.insert(output.end(), receivers[0].begin(), receivers[0].end());

    return result;
}

int decodeEp6SamplesMulti(
    std::span<const std::uint8_t> packet,
    std::span<std::vector<std::complex<float>>> outputs) noexcept
{
    if (!validEp6Packet(packet))
        return -1;

    if (outputs.empty() || outputs.size() > 7)
        return -1;

    const std::size_t roundBytes = outputs.size() * 6 + 2;
    if (roundBytes > kFramePayload)
        return -1;

    const float inverseFullScale =
        1.0f / static_cast<float>(kFullScale24);

    int rounds = 0;
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};

    for (const std::size_t frameStart : frameStarts) {
        const auto* frame = packet.data() + frameStart;

        if (!validFrameSync(frame))
            continue;

        const auto* payload = frame + 8;

        //DH1KLM: Standard P1 emits complete receiver rounds only. Remaining
        //DH1KLM: bytes in a 512-byte frame are padding and must not be decoded.
        for (std::size_t offset = 0;
             offset + roundBytes <= kFramePayload;
             offset += roundBytes) {

            for (std::size_t rx = 0; rx < outputs.size(); ++rx) {
                const auto* sample =
                    payload + offset + rx * 6;

                const float i =
                    static_cast<float>(decode24be(sample))
                    * inverseFullScale;

                const float q =
                    static_cast<float>(decode24be(sample + 3))
                    * inverseFullScale;

                outputs[rx].emplace_back(i, q);
            }

            ++rounds;
        }
    }

    return rounds;
}

} // namespace AetherSDR::anan::p1
