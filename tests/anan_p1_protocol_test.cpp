#include "core/backends/anan/P1Protocol.h"

#include <array>
#include <cassert>
#include <complex>
#include <cstdint>
#include <vector>

using namespace AetherSDR::anan::p1;

int main()
{
    //DH1KLM: Discovery request must use the standard EF FE 02 signature.
    const auto discovery = discoveryRequest();
    assert(discovery.size() == 63);
    assert(discovery[0] == 0xEF);
    assert(discovery[1] == 0xFE);
    assert(discovery[2] == 0x02);

    //DH1KLM: Verify a representative Orion discovery reply. The test uses the
    //DH1KLM: standard offset 0x13 for the receiver count.
    std::array<std::uint8_t, 64> reply{};
    reply[0] = 0xEF;
    reply[1] = 0xFE;
    reply[2] = 0x02;
    reply[3] = 0x00;
    reply[4] = 0x11;
    reply[5] = 0x22;
    reply[6] = 0x33;
    reply[7] = 0x44;
    reply[8] = 0x55;
    reply[9] = 0x66;
    reply[9] = 0x12;
    reply[10] = 0x05; // Orion
    reply[19] = 7;

    const auto parsed = parseDiscoveryReply(reply);
    assert(parsed.has_value());
    assert(parsed->boardId == 0x05);
    assert(parsed->numRx == 7);

    //DH1KLM: Check the standard Protocol 1 frequency/phase relationship.
    const auto phase = frequencyToPhaseWord(14'200'000);
    const auto recovered = phaseWordToFrequency(phase);
    assert(recovered >= 14'199'999 && recovered <= 14'200'001);

    //DH1KLM: RX1 is register 0x02 -> C0 0x04; RX2 is register 0x03 -> C0 0x06.
    const auto rx1 = ccRxFrequency(0, 14'200'000);
    const auto rx2 = ccRxFrequency(1, 14'200'000);
    assert(rx1[0] == 0x04);
    assert(rx2[0] == 0x06);

    //DH1KLM: TX uses register 0x01 -> C0 0x02.
    const auto tx = ccTxFrequency(14'200'000);
    assert(tx[0] == 0x02);

    //DH1KLM: MOX is a property of the C0 byte and must not alter the payload.
    const auto keyed = withMox(tx, true);
    const auto unkeyed = withMox(tx, false);
    assert((keyed[0] & 0x01) != 0);
    assert((unkeyed[0] & 0x01) == 0);
    for (std::size_t i = 1; i < keyed.size(); ++i)
        assert(keyed[i] == unkeyed[i]);

    //DH1KLM: EP2 control packet must contain the standard EF FE 01 02 header,
    //DH1KLM: a big-endian sequence number and two synchronized 512-byte frames.
    const auto packet = ep2ControlPacket(0x01020304u, tx, rx1);
    assert(packet[0] == 0xEF);
    assert(packet[1] == 0xFE);
    assert(packet[2] == 0x01);
    assert(packet[3] == 0x02);
    assert(packet[4] == 0x01);
    assert(packet[5] == 0x02);
    assert(packet[6] == 0x03);
    assert(packet[7] == 0x04);
    assert(packet[8] == 0x7F && packet[9] == 0x7F && packet[10] == 0x7F);
    assert(packet[520] == 0x7F && packet[521] == 0x7F && packet[522] == 0x7F);

    return 0;
}
