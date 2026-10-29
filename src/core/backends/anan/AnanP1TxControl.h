#pragma once
#include "P1Protocol.h"
#include "AnanP2TxControl.h"
#include <complex>
#include <cmath>

namespace AetherSDR::anan {
// Thetis networkproto1.c WriteMainLoop/sendProtocol1Samples: native P1 L,R,I,Q.
inline std::optional<p1::Packet> buildP1Transmit(const p1::ReceiveConfig& receive,
    const P2TxControl& tx, bool keyed, std::uint32_t sequence,
    std::uint8_t first, std::uint8_t second, std::span<const std::complex<float>> iq = {}) noexcept
{
    if (!validP2TxControl(tx) || iq.size() > p1::kSpeakerFrames) { return {}; }
    p1::ReceiveConfig config = receive;
    if (keyed) { config.antenna = tx.antenna; config.rxOcMask = tx.openCollector; config.rxOnlyInput = 0; }
    auto packet = p1::speakerAndControl(sequence,first == 2 ? 0 : first,second == 2 ? 0 : second,config);
    if (!packet) { return {}; }
    const int row = selectedFilterBand(tx.lowPass,tx.frequencyHz);
    constexpr std::array<int,7> lpfBits{3,2,1,0,6,5,4};
    for (int frame = 0; frame < 2; ++frame) {
        const int base = 8 + 512 * frame;
        const int address = frame ? second : first;
        if (address == 2) {
            (*packet)[base+3] = 2;
            const auto hz = static_cast<std::uint32_t>(tx.frequencyHz * tx.frequencyCorrection);
            for (int byte = 0; byte < 4; ++byte) { (*packet)[base+4+byte] = static_cast<std::uint8_t>(hz >> (24-8*byte)); }
        }
        if (address == 0x12 && keyed) {
            (*packet)[base+4] = static_cast<std::uint8_t>(tx.drive);
            (*packet)[base+6] = config.receiveFilterBits | (tx.disablePa ? 0x80 : 0);
            (*packet)[base+7] = static_cast<std::uint8_t>(1u << lpfBits[row]);
        }
        if (keyed) { (*packet)[base+3] |= 1; }
        for (int n = 0; keyed && n < 63; ++n) {
            const std::size_t index = static_cast<std::size_t>(frame * 63 + n);
            if (index >= iq.size()) { break; }
            const auto sample = iq[index];
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag()) || std::abs(sample) > 1.0f) { return {}; }
            const std::array<float,2> components{sample.real(),sample.imag()};
            for (int part = 0; part < 2; ++part) {
                const auto value = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(components[part] * 32767)));
                (*packet)[base+12+8*n+2*part] = static_cast<std::uint8_t>(value >> 8);
                (*packet)[base+13+8*n+2*part] = static_cast<std::uint8_t>(value);
            }
        }
    }
    return packet;
}
} // namespace AetherSDR::anan
