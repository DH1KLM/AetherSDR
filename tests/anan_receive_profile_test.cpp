//DH1KLM: Inject the real P2Client frame handler without creating sockets.
// Thetis ANAN-7000DLE normal RX1 uses physical DDC2, delivered as logical RX0.
#include "core/backends/anan/P2Client.h"

#include <QCoreApplication>

#include <array>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR::anan;

namespace AetherSDR::anan {
struct P2ClientTestAccess {
    static void prepare(P2Client& client, int selectedBoardId)
    {
        client.m_expectedBoardId = selectedBoardId;
        const auto profile = receiveProfile(selectedBoardId);
        client.m_numAdcs = profile ? profile->numAdcs : 2;
        client.m_activeDdcs = {{48, 0}};
        client.m_activeDdcCount = 1;
        client.m_running = true;
    }
    static void feed(P2Client& client, std::span<const std::uint8_t> bytes, quint16 port)
    {
        client.handleDatagram(bytes, port);
    }
    static bool validated(const P2Client& client) { return client.m_discoveryValidated; }
    static int physicalDdc(const P2Client& client) { return client.m_firstDdcIndex; }
    static bool selectLpf(const P2Client& client) { return client.m_selectReceiveLpf; }
    static void setAdcs(P2Client& client, int count) { client.m_numAdcs = count; }
    static void setDdcCount(P2Client& client, int count) { client.m_activeDdcCount = count; }
    static int adcs(const P2Client& client) { return client.m_numAdcs; }
    static ReceiveFilterKind filters(const P2Client& client) { return client.m_receiveFilterKind; }
    static bool swapsSpeaker(const P2Client& client) { return client.m_swapSpeakerChannels; }
    static void setDdc(P2Client& client, DdcConfig config) { client.m_activeDdcs = {config}; }
};
}

static int failures = 0;
static void check(bool condition, const char* claim)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", claim);
        ++failures;
    }
}

static std::array<std::uint8_t, 60> discovery(std::uint8_t boardId)
{
    std::array<std::uint8_t, 60> packet{};
    packet[4] = 2;
    packet[5] = 0x88;
    packet[10] = 0x6A;
    packet[11] = boardId;
    packet[12] = 43;
    packet[13] = 27;
    packet[20] = 7; // Deliberately not the profile's physical DDC count.
    packet[21] = 1;
    return packet;
}

static std::array<std::uint8_t, kDdcHeaderLen + kDdcSampleBytes> iq()
{
    std::array<std::uint8_t, kDdcHeaderLen + kDdcSampleBytes> packet{};
    packet[13] = 24;
    packet[15] = 1;
    packet[kDdcHeaderLen] = 0x40;
    return packet;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        int links = 0;
        int samples = 0;
        int logicalDdc = -1;
        int gapDdc = -1;
        QObject::connect(&client, &P2Client::linkUp, &client, [&]() { ++links; });
        QObject::connect(&client, &P2Client::ddc0IqReady, &client,
            [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
        QObject::connect(&client, &P2Client::ddcIqReady, &client,
            [&](int index, const std::vector<std::complex<float>>&) { logicalDdc = index; });
        QObject::connect(&client, &P2Client::ddcSequenceGap, &client,
            [&](int index) { gapDdc = index; });
        const auto frame = iq();
        P2ClientTestAccess::feed(client, frame, 1035);
        check(links == 0 && samples == 0, "stale DDC0 cannot link a session before profile validation");
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 2
              && P2ClientTestAccess::selectLpf(client),
              "Orion MkII selects its DDC2/LPF profile independent of reported receivers");
        P2ClientTestAccess::feed(client, frame, 1035);
        P2ClientTestAccess::feed(client, frame, 1038);
        check(links == 0 && samples == 0, "disabled physical DDC0 and DDC3 cannot establish single RX1");
        P2ClientTestAccess::feed(client, frame, 1037);
        check(links == 1 && samples == 1 && logicalDdc == 0,
              "physical DDC2 establishes link and reaches the existing logical RX0 DSP signal");
        P2ClientTestAccess::feed(client, frame, 1037);
        check(links == 1 && gapDdc == 0 && client.droppedPackets() == 1,
              "physical DDC2 sequence discontinuity is delivered as logical RX0");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 10);
        P2ClientTestAccess::feed(client, discovery(10), 1024);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 0
              && !P2ClientTestAccess::selectLpf(client), "Saturn retains its DDC0 packet profile");
    }
    for (const int selected : {10, 4}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, selected);
        int errors = 0;
        QObject::connect(&client, &P2Client::connectionError, &client,
            [&](const QString&) { ++errors; });
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning() && errors == 1,
              "Orion MkII cannot satisfy another selected physical board");
    }
    for (const std::uint8_t unsupported : {0, 6, 11, 99}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, -1);
        P2ClientTestAccess::feed(client, discovery(unsupported), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "recognized but unimplemented RX boards and unknown boards fail closed");
    }
    for (const bool invalidAdcs : {true, false}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        if (invalidAdcs) {
            P2ClientTestAccess::setAdcs(client, 1);
        } else {
            P2ClientTestAccess::setDdcCount(client, 3);
        }
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "Orion MkII rejects wrong ADC count and a DDC range exceeding the profile");
    }
    for (const DdcConfig invalid : {DdcConfig{48, 2}, DdcConfig{48, -1}, DdcConfig{123, 0}}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        P2ClientTestAccess::setDdc(client, invalid);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "invalid physical ADC indices and unsupported rates cannot start an Orion MkII session");
    }
    for (const int id : {1, 2, 3, 4, 20}) {
        for (const bool manual : {false, true}) {
            P2Client client;
            P2ClientTestAccess::prepare(client, manual ? -1 : id);
            int samples = 0;
            QObject::connect(&client, &P2Client::ddc0IqReady, &client,
                [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
            P2ClientTestAccess::feed(client, discovery(static_cast<std::uint8_t>(id)), 1024);
            const ReceiveProfile profile = *receiveProfile(id);
            check(P2ClientTestAccess::validated(client)
                  && P2ClientTestAccess::adcs(client) == profile.numAdcs
                  && P2ClientTestAccess::filters(client) == profile.filterKind
                  && P2ClientTestAccess::swapsSpeaker(client) == profile.swapSpeakerChannels,
                  "selected and address-only new-family sessions resolve physical RX properties");
            P2ClientTestAccess::feed(client, iq(), static_cast<quint16>(1035 + profile.firstDdcIndex));
            check(samples == 1, "new receive family delivers its physical RX1 stream to the existing DSP signal");
        }
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 2);
        P2ClientTestAccess::setDdcCount(client, 3);
        P2ClientTestAccess::feed(client, discovery(2), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "Hermes II rejects a third physical DDC even though the generic packet has room");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 1);
        P2ClientTestAccess::setDdc(client, {48, 1});
        P2ClientTestAccess::feed(client, discovery(1), 1024);
        check(!P2ClientTestAccess::validated(client), "Hermes cannot use nonexistent ADC1");
    }
    if (failures == 0) {
        std::puts("anan_receive_profile_test: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
