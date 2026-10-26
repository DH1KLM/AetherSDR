#pragma once

namespace AetherSDR::anan {
// Radio microphone wiring; this configuration never asserts MOX or supplies TX IQ.
struct MicOptions {
    bool ring = false;
    bool bias = false;
    bool ptt = false;
    bool xlr = false;
};
inline constexpr bool hasMicWiring(int board) { return board == 4 || board == 5 || board == 10; }
inline constexpr bool hasXlrMic(int board, int protocol) { return board == 10 && protocol == 2; }
}
