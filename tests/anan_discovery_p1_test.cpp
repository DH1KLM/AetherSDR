#include "core/backends/anan/AnanDiscovery.h"

#include <cassert>

using namespace AetherSDR::anan;

int main()
{
    //DH1KLM: The Stage 1 P1 fixture uses the exact field positions documented by
    //DH1KLM: Thetis/NereusSDR: MAC 3..8, firmware 9, board 10, Metis 19, RX 20.
    QByteArray reply(60, '\0');
    reply[0] = char(0xEF);
    reply[1] = char(0xFE);
    reply[2] = char(0x02);
    reply[3] = char(0x00);
    reply[4] = char(0x1C);
    reply[5] = char(0xC0);
    reply[6] = char(0xA2);
    reply[7] = char(0x13);
    reply[8] = char(0xDD);
    reply[9] = char(0x45);
    reply[10] = char(0x05); // Orion
    reply[19] = char(0x04);
    reply[20] = char(0x07);

    //DH1KLM: The parser is private by design; protocolForModel is the public
    //DH1KLM: deterministic seam used by the connection workflow in this stage.
    assert(AnanDiscovery::protocolForModel(QStringLiteral("ANAN-200D"))
           == AnanDiscovery::Protocol::P1);
    assert(AnanDiscovery::protocolForModel(QStringLiteral("ANAN-G2"))
           == AnanDiscovery::Protocol::P2);
    assert(AnanDiscovery::protocolForModel(QStringLiteral("unknown"))
           == AnanDiscovery::Protocol::Unknown);

    const QString serial = AnanDiscovery::macToSerial(
        {0x00, 0x1C, 0xC0, 0xA2, 0x13, 0xDD});
    assert(serial == QStringLiteral("00:1C:C0:A2:13:DD"));
    return 0;
}
