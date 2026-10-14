#include "core/backends/anan/P1Protocol.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace AetherSDR::anan::p1;
namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
void controls()
{
    ReceiveConfig config;
    config.rateKsps = 192;
    config.receivers = 5;
    config.numAdcs = 2;
    config.adcIndex = 1;
    config.antenna = 3;
    config.dither = config.random = true;
    config.adc0AttenuationDb = 17;
    config.adc1AttenuationDb = 31;
    config.frequencyHz = 14'200'000;
    check(receiveControl(0, config) == Control{0, 2, 0, 0x18, 0x26}, "general bank bytes");
    check(receiveControl(4, config) == Control{4, 0, 0xd8, 0xac, 0xc0}, "P1 uses Hz, not phase words");
    check(receiveControl(0x1c, config) == Control{0x1c, 0x55, 1, 0, 0}, "five ADC routes");
    check(receiveControl(0x12, config) == Control{0x12, 0, 0x40, 0xa0, 0x10}, "filters, zero drive, PA disabled");
    check(receiveControl(0x14, config) == Control{0x14, 0x40, 0, 0, 0x31}, "ADC0 attenuation and mic PTT disable");
    check(receiveControl(0x16, config) == Control{0x16, 0x3f, 0, 0, 0}, "ADC1 attenuation");
    for (int address = 0; address < 256; ++address) {
        const auto control = receiveControl(static_cast<std::uint8_t>(address), config);
        if (control)
            check(((*control)[0] & 1) == 0, "all accepted banks keep MOX clear");
    }
    for (const int address : {1, 2, 3, 0x0e, 0x10, 0x18, 0x20, 0x22, 0x26, 0xff})
        check(!receiveControl(static_cast<std::uint8_t>(address), config), "unsupported/TX/inactive bank rejected");
    config.numAdcs = 1; config.adcIndex = 0;
    check(receiveControl(0x16, config) == Control{0x16, 0, 0, 0, 0}, "absent ADC1 has no controls");
    check(receiveControl(0x24, config) == Control{0x24, 0, 0, 0, 0}, "absent ADC1 has no filter");
    for (const int rate : {48, 96, 192, 384}) {
        config.rateKsps = rate;
        check(validConfig(config), "standard P1 rates accepted");
    }
    config.rateKsps = 768;
    check(!receiveControl(0, config), "P2-only rate rejected");
    config = {};
    config.receivers = 0; check(!validConfig(config), "zero receivers rejected");
    config.receivers = 8; check(!validConfig(config), "overflowing receiver field rejected");
    config = {}; config.adcIndex = 1; check(!validConfig(config), "absent ADC rejected");
    config = {}; config.adc0AttenuationDb = 32; check(!validConfig(config), "attenuation overflow rejected");
    config = {}; config.adc1AttenuationDb = -1; check(!validConfig(config), "negative attenuation rejected");
    config = {}; config.frequencyHz = std::numeric_limits<std::uint32_t>::max();
    check(!validConfig(config), "frequency beyond Nyquist rejected");
    config = {}; config.receiveFilterBits = 0x80; check(!validConfig(config), "PA bit cannot enter filter value");
}
void packets()
{
    const auto request = discoveryRequest();
    check(request.size() == 63 && request[0] == 0xef && request[1] == 0xfe
        && request[2] == 2, "discovery request header");
    auto reply = request;
    reply[3] = 0x88; reply[8] = 0x6a; reply[9] = 27; reply[10] = 5;
    check(parseDiscovery(reply)->boardId == 5 && parseDiscovery(reply)->firmware == 27,
          "discovery fields are P1 offsets");
    reply[2] = 3; check(parseDiscovery(reply)->streaming, "busy discovery");
    for (std::size_t length = 0; length < 11; ++length)
        check(!parseDiscovery(std::span(reply).first(length)), "truncated discovery rejected");
    reply[3] = 0x89; check(!parseDiscovery(reply), "multicast identity rejected");
    reply.fill(0); reply[0] = 0xef; reply[1] = 0xfe; reply[2] = 2;
    check(!parseDiscovery(reply), "zero identity rejected");
    for (bool run : {false, true}) {
        const auto command = streamCommand(run);
        check(command[2] == 4 && command[3] == static_cast<int>(run), "IQ-only stream command");
    }
    ReceiveConfig config;
    const std::array<std::int16_t, 3> audio{0x1234, -2, -32768};
    const auto packet = speakerAndControl(0x12345678, 0, 4, config, audio, true);
    check(packet.has_value(), "EP2 packet encoded");
    check((*packet)[4] == 0x12 && (*packet)[7] == 0x78, "sequence big endian");
    check((*packet)[16] == 0xff && (*packet)[17] == 0xfe
        && (*packet)[18] == 0x12 && (*packet)[19] == 0x34, "speaker channel swap and signed endian");
    check((*packet)[24] == 0 && (*packet)[25] == 0
        && (*packet)[26] == 0x80 && (*packet)[27] == 0, "odd input swaps with silence");
    for (int frame = 0; frame < 2; ++frame) {
        const int base = 8 + frame * 512;
        check((*packet)[base] == 0x7f && (*packet)[base + 2] == 0x7f, "frame sync");
        check(((*packet)[base + 3] & 1) == 0, "EP2 MOX clear");
        for (int round = 0; round < 63; ++round)
            for (int byte = 4; byte < 8; ++byte)
                check((*packet)[base + 8 + round * 8 + byte] == 0, "all transmit IQ is zero");
    }
    check(!speakerAndControl(0, 2, 4, config), "TX control cannot be wrapped in EP2");
}
void iq()
{
    for (int receivers = 1; receivers <= 7; ++receivers) {
        Packet packet{};
        packet[0] = 0xef; packet[1] = 0xfe; packet[2] = 1; packet[3] = 6;
        packet[4] = 0x89; packet[7] = 0xab;
        for (int frame = 0; frame < 2; ++frame) {
            const int base = 8 + 512 * frame;
            packet[base] = packet[base + 1] = packet[base + 2] = 0x7f;
            for (int round = 0; round < 504 / (6 * receivers + 2); ++round) {
                for (int slot = 0; slot < receivers; ++slot) {
                    const int offset = base + 8 + round * (6 * receivers + 2) + slot * 6;
                    packet[offset] = slot == receivers - 1 ? 0x80 : 0x40;
                    packet[offset + 3] = 0x7f;
                    packet[offset + 4] = packet[offset + 5] = 0xff;
                }
                const int mic = base + 8 + round * (6 * receivers + 2) + 6 * receivers;
                packet[mic] = packet[mic + 1] = 0xa5;
            }
        }
        const auto parsed = parseIq(packet, receivers);
        check(parsed && parsed->sequence == 0x890000ab, "EP6 header/sequence");
        std::vector<std::complex<float>> output;
        check(decodeIq(*parsed, receivers - 1, output), "selected physical slot decoded");
        check(output.size() == static_cast<std::size_t>(2 * (504 / (6 * receivers + 2))),
              "mic and per-frame padding never become IQ");
        for (auto value : output)
            check(value.real() < -0.999f && value.imag() == 1.0f, "signed 24-bit extremes");
        const auto size = output.size();
        check(!decodeIq(*parsed, receivers, output) && output.size() == size, "invalid slot leaves output intact");
        auto inconsistent = *parsed; inconsistent.roundsPerFrame = 999;
        check(!decodeIq(inconsistent, 0, output) && output.size() == size, "forged geometry rejected");
        for (std::size_t length = 0; length < packet.size(); ++length)
            check(!parseIq(std::span(packet).first(length), receivers), "every truncation rejected");
        for (const int offset : {0, 1, 2, 3, 8, 9, 10, 520, 521, 522}) {
            auto bad = packet; bad[offset] ^= 1;
            check(!parseIq(bad, receivers), "wrong endpoint/header/either sync rejected");
        }
        check(!parseIq(packet, 0) && !parseIq(packet, 8), "invalid receiver geometry rejected");
    }
}
} // namespace
int main()
{
    controls(); packets(); iq();
    std::cout << "ANAN P1 protocol checks passed\n";
}
