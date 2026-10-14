#pragma once

namespace AetherSDR::anan {
//DH1KLM: One discovery row per MAC. Prefer the operator's selected protocol
// while it answers, but permit a firmware/protocol change after two missed sweeps.
[[nodiscard]] constexpr bool acceptsDiscoveryProtocolUpdate(
    int currentProtocol, int incomingProtocol, int preferredProtocol,
    int missedPreferredSweeps) noexcept
{
    return currentProtocol != preferredProtocol || incomingProtocol == preferredProtocol
        || missedPreferredSweeps > 1;
}
} // namespace AetherSDR::anan
