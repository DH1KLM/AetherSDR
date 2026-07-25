// MetisClient used to be incapable of keying a radio, structurally: every C0
// register-address byte was even, so MOX (C0 bit 0) was always 0. Adding TX
// destroyed that invariant. This test is what replaces it.
//
// The claim under test is not "setMox returns false when refused" -- it is the
// only one that actually matters on the air: WITH THE GATE CLOSED, NO BYTE THAT
// WOULD GO OUT ON THE WIRE EVER HAS C0 BIT 0 SET. So it inspects the real EP2
// packet the client would send, from the same builder the socket path uses,
// rather than trusting a status flag.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Both sub-frames carry C0; keying either one keys the radio, so both are
// checked. Frame C0 sits at SYNC(3) into each 512-byte frame.
static bool anyFrameKeyed(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        if ((pkt[fs + 3] & kC0MoxBit) != 0)
            return true;
    }
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    MetisClient client;

    check(!client.transmitEnabled(), "transmit is DISABLED by default");
    check(!client.isKeyed(), "not keyed by default");

    // ---- gate closed ----
    client.setMox(true);
    check(!client.isKeyed(), "setMox(true) refused while the gate is closed");

    // Drain enough packets to cover the whole round robin (freq, gain, ADC
    // assign) plus the config bank, several times over, with a key request
    // standing the entire time. Any single keyed frame here is a real radio
    // keyed by accident.
    for (int i = 0; i < 64; ++i) {
        client.setMox(true);                       // keep asking, every frame
        const auto pkt = client.buildNextControlPacket();
        if (anyFrameKeyed(pkt)) {
            check(false, "a frame was keyed with the gate CLOSED");
            break;
        }
    }

    // Queuing TX frequency and drive must not key anything either -- those are
    // setup, and setup happening before the operator keys is the normal order.
    client.setTxFrequencyHz(14'200'000);
    client.setTxDriveLevel(200);
    for (int i = 0; i < 8; ++i) {
        if (anyFrameKeyed(client.buildNextControlPacket())) {
            check(false, "TX frequency/drive setup keyed a frame with the gate closed");
            break;
        }
    }

    // ---- gate open ----
    client.enableTransmit(true);
    check(client.transmitEnabled(), "transmit enabled after explicit opt-in");
    check(!client.isKeyed(), "opening the gate does not key by itself");

    // Still unkeyed until asked.
    for (int i = 0; i < 4; ++i) {
        if (anyFrameKeyed(client.buildNextControlPacket())) {
            check(false, "an open gate keyed a frame without setMox");
            break;
        }
    }

    client.setMox(true);
    check(client.isKeyed(), "setMox(true) honoured once the gate is open");
    {
        const auto pkt = client.buildNextControlPacket();
        const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
        // BOTH sub-frames must carry MOX: the radio keys off whichever bank is
        // in flight, so keying only one is an intermittent, cadence-dependent
        // half-key -- the worst possible failure mode to debug.
        for (const std::size_t fs : frameStarts) {
            check((pkt[fs + 3] & kC0MoxBit) != 0, "both sub-frames carry MOX when keyed");
            // The address must survive keying, or MOX would corrupt the bank.
            check((pkt[fs + 3] & ~kC0MoxBit) != 0 || fs == 8,
                  "register address intact alongside MOX");
        }
    }

    // ---- unkey, and revoking the gate ----
    client.setMox(false);
    check(!anyFrameKeyed(client.buildNextControlPacket()), "unkeys cleanly");

    // Revoking the gate while keyed must drop the key on the wire immediately,
    // not merely refuse the next request.
    client.setMox(true);
    check(anyFrameKeyed(client.buildNextControlPacket()), "keyed again");
    client.enableTransmit(false);
    for (int i = 0; i < 8; ++i) {
        if (anyFrameKeyed(client.buildNextControlPacket())) {
            check(false, "revoking the gate did not drop the key on the wire");
            break;
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
