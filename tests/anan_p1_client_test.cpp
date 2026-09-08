#include "core/backends/anan/P1Client.h"

#include <cassert>

using namespace AetherSDR::anan;

int main()
{
    //DH1KLM: Construction must not create or bind a UDP socket.
    P1Client client;
    assert(!client.isRunning());
    assert(!client.isLinkUp());
    assert(client.droppedPackets() == 0);

    //DH1KLM: The socket-free packet builder must already produce a standard
    //DH1KLM: Metis EP2 datagram, making the transport test independent of RF.
    const auto packet = client.buildNextControlPacket();
    assert(packet.size() == p1::kUsbPacketSize);
    assert(packet[0] == 0xEF);
    assert(packet[1] == 0xFE);
    assert(packet[2] == 0x01);
    assert(packet[3] == static_cast<std::uint8_t>(p1::Endpoint::Ep2));

    //DH1KLM: Two synchronized 512-byte frames must be present in the 1032-byte
    //DH1KLM: Metis datagram.
    assert(packet[8] == p1::kSync);
    assert(packet[9] == p1::kSync);
    assert(packet[10] == p1::kSync);
    assert(packet[520] == p1::kSync);
    assert(packet[521] == p1::kSync);
    assert(packet[522] == p1::kSync);

    return 0;
}
