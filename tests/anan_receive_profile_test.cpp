//DH1KLM: Inject the real P2Client frame handler without creating sockets.
// Thetis ANAN-7000DLE normal RX1 uses physical DDC2, delivered as logical RX0.
#include "core/backends/anan/P2Client.h"

#include <QCoreApplication>

#include <array>
#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR::anan;

namespace AetherSDR::anan {
struct P2ClientTestAccess {
    static void prepare(P2Client& client, int selectedBoardId, AnanModel model = AnanModel::Automatic,
                        bool overrideDetection = false)
    {
        client.m_expectedBoardId = selectedBoardId;
        client.m_model = model;
        client.m_overrideDetection = overrideDetection;
        const auto profile = receiveProfile(selectedBoardId, model, overrideDetection);
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
    static auto refresh(P2Client& client, qint64 nowMs)
    {
        return client.takeGeneralRefresh(nowMs);
    }
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
    //DH1KLM: Discard the initial General as a lost UDP datagram. Continued IQ
    // must not prevent later General packets from restoring ALEX enable. The
    // scheduler used by the real keepalive is driven without sleeps or sockets.
    for (const int board : {1, 5, 10}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, board);
        check(!P2ClientTestAccess::refresh(client, 5000),
              "no General retry before discovery validation");
        P2ClientTestAccess::feed(client, discovery(board), 1024);
        const auto profile = *receiveProfile(board);
        P2ClientTestAccess::feed(client, iq(),
            static_cast<quint16>(1035 + profile.firstDdcIndex));
        check(!P2ClientTestAccess::refresh(client, 499), "General refresh is not sent every HP tick");
        const auto retry = P2ClientTestAccess::refresh(client, 500);
        check(retry && (*retry)[4] == 0 && (*retry)[37] == 8 && (*retry)[38] == 1
              && (*retry)[58] == profile.disablePa && (*retry)[59] == profile.alexEnableMask,
              "lost General is retried with the validated model's ALEX/PA configuration despite live IQ");
        check(!P2ClientTestAccess::refresh(client, 999), "General retry respects its own cadence");
        check(P2ClientTestAccess::refresh(client, 1000) == retry,
              "General refresh continues after the first retry");
        check(P2ClientTestAccess::refresh(client, 9000) == retry
              && !P2ClientTestAccess::refresh(client, 9000),
              "late timer delivery sends one refresh instead of a catch-up burst");
        client.stop();
        check(!P2ClientTestAccess::refresh(client, 10000), "disconnect cancels pending General refresh");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        const auto retry = P2ClientTestAccess::refresh(client, 500);
        check(retry && (*retry)[59] == 3 && (*retry)[58] == 1,
              "Anvelina/Orion MkII mapped to ANAN-200D refreshes the captured ALEX enable 03 with PA disabled");
        client.stop();
        P2ClientTestAccess::prepare(client, 10);
        check(!P2ClientTestAccess::refresh(client, 500),
              "a new unvalidated session cannot replay the previous device's General");
    }
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
    //DH1KLM: Exercise every source-backed logical model through production validation.
    for (const auto& entry : kAnanModels) {
        if (entry.model == AnanModel::Automatic || entry.model == AnanModel::Hpsdr
            || entry.model == AnanModel::HermesLite2) { continue; }
        P2Client client;
        P2ClientTestAccess::prepare(client, entry.nativeBoardId, entry.model);
        int blocks = 0;
        QObject::connect(&client, &P2Client::ddc0IqReady, [&](const auto&) { ++blocks; });
        P2ClientTestAccess::feed(client, discovery(entry.nativeBoardId), 1024);
        const auto profile = *receiveProfile(entry.nativeBoardId, entry.model);
        check(P2ClientTestAccess::validated(client), "logical model reaches P2 validated state");
        P2ClientTestAccess::feed(client, iq(), static_cast<quint16>(1035 + profile.firstDdcIndex));
        check(blocks == 1, "logical model uses its correct P2 RX port");
    }
    for (const int board : {1, 5}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, board, AnanModel::RedPitaya);
        P2ClientTestAccess::feed(client, discovery(board), 1024);
        check(P2ClientTestAccess::adcs(client) == 2 && P2ClientTestAccess::physicalDdc(client) == 2
            && P2ClientTestAccess::filters(client) == ReceiveFilterKind::LegacyHpf,
            "Red Pitaya P2 shared board IDs use dual ADC and legacy filters");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client), "Orion MkII cannot silently become ANAN-200D");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan8000);
        auto busy = discovery(5); busy[4] = 3;
        P2ClientTestAccess::feed(client, busy, 1024);
        check(!P2ClientTestAccess::validated(client), "selected logical model cannot take over a busy P2 radio");
    }
    {
        //DH1KLM: A DIY Orion MkII digital board can deliberately use the
        // ANAN-200D effective profile. Board identity remains the actual reply.
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        P2ClientTestAccess::feed(client, discovery(5), 0);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 2
              && P2ClientTestAccess::adcs(client) == 2
              && P2ClientTestAccess::filters(client) == ReceiveFilterKind::LegacyHpf,
              "explicit override applies ANAN-200D legacy filters on Orion MkII");
    }
    for (const bool busy : {false, true}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        auto packet = discovery(busy ? 5 : 4);
        if (busy) { packet[4] = 3; }
        P2ClientTestAccess::feed(client, packet, 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "override cannot bypass busy status or a different selected device identity");
    }
    for (const quint16 port : {0, 1024, 1025, 1037}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::RedPitaya);
        auto packet = discovery(5);
        std::fill(packet.begin() + 5, packet.begin() + 11, 0);
        packet[10] = 1; packet[12] = 2; packet[13] = 30;
        P2ClientTestAccess::feed(client, packet, port);
        check(P2ClientTestAccess::validated(client) == (port == 0 || port == 1024),
              "Patroclus Red Pitaya P2 validates only on a discovery endpoint, never status/IQ ports");
        if (port == 0 || port == 1024) {
            int samples = 0;
            QObject::connect(&client, &P2Client::ddc0IqReady,
                [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
            P2ClientTestAccess::feed(client, iq(), 1037);
            check(samples == 1 && P2ClientTestAccess::physicalDdc(client) == 2,
                  "accepted Patroclus P2 discovery routes physical DDC2 into logical RX0");
        }
    }
    if (failures == 0) {
        std::puts("anan_receive_profile_test: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
