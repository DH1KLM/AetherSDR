#pragma once

#include "P2Protocol.h"
#include "AnanReceiveCalibration.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace AetherSDR::anan {
using HighPriorityPacket = std::array<std::uint8_t, 1444>;

struct P2TxControl {
    double frequencyHz = 0;
    int drive = 0; // Raw 8-bit drive, not watts or a calibrated power percentage.
    int antenna = 1;
    bool disablePa = true;
    double frequencyCorrection = 1.0;
    std::uint8_t openCollector = 0;
    std::array<FilterBand, 7> lowPass = defaultFilterConfig(false).lowPass;
};

inline std::optional<ReceiveProfile> p2TransmitProfile(int board, AnanModel model,
                                                     bool overrideDetection = false) noexcept
{
    // An explicitly selected 200D uses Thetis Orion/legacy ALEX, including
    // the operator's board-5 override. Discovery alone never selects this path.
    if (overrideDetection && model == AnanModel::Anan200D && board == 5) {
        return receiveProfile(4);
    }
    if (!modelMatchesBoard(model, board) || model == AnanModel::RedPitaya
        || model == AnanModel::AnvelinaPro3 || model == AnanModel::AnanG21K
        || (model == AnanModel::Automatic && (board == 5 || board == 10))) {
        return std::nullopt;
    }
    return receiveProfile(board);
}

inline bool validP2TxControl(const P2TxControl& control) noexcept
{
    return std::isfinite(control.frequencyHz) && control.frequencyHz > 0
        && control.frequencyHz <= kDspClockHz / 2.0
        && control.drive >= 0 && control.drive <= 255
        && control.antenna >= 1 && control.antenna <= 3
        && (control.openCollector & 0x80) == 0
        && validFrequencyFactor(control.frequencyCorrection)
        && control.frequencyHz * control.frequencyCorrection <= kDspClockHz / 2.0
        && validFilterBands(control.lowPass)
        && selectedFilterBand(control.lowPass, control.frequencyHz) >= 0;
}

// Thetis network.c CmdHighPriority and netInterface.c SetAntBits,
// SetTRXrelay/SetAlexLPFBits. The receive builder remains RX-only.
inline std::optional<HighPriorityPacket> buildP2TransmitControl(
    const HighPriorityPacket& receive, const P2TxControl& control,
    const ReceiveProfile& profile, bool keyed, std::uint32_t sequence) noexcept
{
    if ((receive[4] & ~1u) != 0 || (keyed && (!receive[4] || !validP2TxControl(control)))) {
        return std::nullopt;
    }
    HighPriorityPacket packet = receive;
    for (int i = 0; i < 4; ++i) {
        packet[i] = static_cast<std::uint8_t>(sequence >> (24 - 8 * i));
    }
    if (!keyed) { return packet; }
    packet[4] |= 2;
    const std::uint32_t frequency = phaseWord(control.frequencyHz * control.frequencyCorrection);
    for (int i = 0; i < 4; ++i) {
        packet[329+i] = static_cast<std::uint8_t>(frequency >> (24 - 8 * i));
    }
    packet[345] = static_cast<std::uint8_t>(control.drive);
    packet[1401] = static_cast<std::uint8_t>(control.openCollector << 1);
    const auto readWord = [&](int offset) {
        std::uint32_t word = 0;
        for (int i = 0; i < 4; ++i) { word = (word << 8) | packet[offset+i]; }
        return word;
    };
    const auto writeWord = [&](int offset, std::uint32_t word) {
        for (int i = 0; i < 4; ++i) {
            packet[offset+i] = static_cast<std::uint8_t>(word >> (24 - 8 * i));
        }
    };
    constexpr std::uint32_t kLpfMask = 0xe0f00000u;
    constexpr std::uint32_t kAntennaMask = 0x07000000u;
    constexpr std::array<int, 7> lpfBits{23,22,21,20,31,30,29};
    const int row = selectedFilterBand(control.lowPass, control.frequencyHz);
    const std::uint32_t lpf = std::uint32_t{1} << lpfBits[row];
    const std::uint32_t antenna = std::uint32_t{1} << (23 + control.antenna);
    std::uint32_t alex0 = readWord(1432);
    alex0 = (alex0 & ~(kLpfMask | kAntennaMask)) | lpf | antenna;
    if (!control.disablePa) { alex0 |= (1u << 27) | (1u << 18); }
    // MkII RX MASTER becomes RX BYPASS OUT while transmitting. Preserve
    // classic ALEX's bit 14, where it is the physical 10 dB attenuator.
    if (profile.filterKind == ReceiveFilterKind::MkIIBpf && (alex0 & (1u << 14))) {
        alex0 = (alex0 & ~(1u << 14)) | (1u << 11);
    }
    writeWord(1432, alex0);
    std::uint32_t alex1 = readWord(1428);
    alex1 = (alex1 & ~(kLpfMask | kAntennaMask | (1u << 27) | (1u << 18))) | lpf | antenna;
    if (!control.disablePa) { alex1 |= 1u << 18; }
    if (profile.filterKind == ReceiveFilterKind::MkIIBpf) { alex1 |= 1u << 8; }
    writeWord(1428, alex1);
    return packet;
}
} // namespace AetherSDR::anan
