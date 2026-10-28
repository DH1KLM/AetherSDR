#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/AnanUdpReceiveBuffer.h"

#include <QCoreApplication>
#include <QTimer>

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace AetherSDR::anan {
struct P2ClientTestAccess {
    static void prepare(P2Client& client, int board, AnanModel model, bool overrideModel = false)
    {
        client.m_running = true;
        client.m_expectedBoardId = board;
        client.m_model = model;
        client.m_overrideDetection = overrideModel;
        const auto profile = receiveProfile(board, model, overrideModel);
        client.m_numAdcs = profile ? profile->numAdcs : 2;
        client.m_activeDdcs = {{48,0}};
        client.m_activeDdcCount = 1;
        std::array<std::uint8_t, 60> reply{};
        reply[4] = 2; reply[11] = static_cast<std::uint8_t>(board);
        reply[5] = 0x02; reply[10] = 0x01;
        reply[12] = 43; reply[13] = 27; reply[20] = 7; reply[21] = 1;
        client.handleDatagram(reply, kRadioPort);
        client.m_linkUp = client.m_discoveryValidated;
    }
    static void inject(P2Client& client,
        std::function<qint64(std::span<const std::uint8_t>, quint16)> writer)
    { client.m_controlSendOverride = std::move(writer); client.m_transmitTimeOverride = [] { return qint64{0}; }; }
    static void tick(P2Client& client) { client.onTransmitTick(); }
    static void keepalive(P2Client& client) { client.m_nextGeneralRefreshMs = 0; client.onKeepaliveTick(); }
    static bool keyed(const P2Client& client) { return client.m_transmitGate.keyCommandAccepted(); }
    static bool enabled(const P2Client& client) { return client.m_transmitGate.enabled(); }
    static bool timerActive(const P2Client& client) { return client.m_transmitTimer && client.m_transmitTimer->isActive(); }
    static auto receive(const P2Client& client) { return client.receiveHighPriority(); }
    static std::size_t queued(const P2Client& client) { return client.m_transmitStream.queuedSamples(); }
    static void speaker(P2Client& client) {
        client.m_speakerAudioEnabled = true;
        client.enqueueSpeakerAudio(QByteArray(16,'\1'));
    }
    static bool speakerSilent(const P2Client& client) {
        return std::all_of(client.m_speakerPending.begin(),client.m_speakerPending.end(),[](auto value) { return value == 0; });
    }
};
}

namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
struct Write { quint16 port; std::vector<std::uint8_t> bytes; };
struct Fixture {
    std::vector<Write> writes;
    quint16 failPort = 0;
    int failKey = -1;
    int faults = 0;
    TxCoordinator coordinator{[](const auto&, auto) {}};
    P2Client client;
    Fixture(int board = 4, AnanModel model = AnanModel::Automatic, bool overrideModel = false)
    {
        P2ClientTestAccess::prepare(client, board, model, overrideModel);
        P2ClientTestAccess::inject(client, [this](auto bytes, quint16 port) {
            writes.push_back({port, {bytes.begin(), bytes.end()}});
            if (port == kTxIqPort || (port == kHighPriorityPort && (bytes[4] & 2))) {
                check(coordinator.hasInFlightDispatches(), "terminal TX write retains original dispatch lease");
            }
            const bool matchesKey = port == kHighPriorityPort && failKey >= 0
                && bool(bytes[4] & 2) == bool(failKey);
            return port == failPort || matchesKey ? qint64{-1} : static_cast<qint64>(bytes.size());
        });
        QObject::connect(&client, &P2Client::transmitFault, &client, [this](const auto&) { ++faults; });
    }
    const std::vector<std::uint8_t>& last(quint16 port) const
    {
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->port == port) { return it->bytes; }
        }
        static const std::vector<std::uint8_t> empty;
        return empty;
    }
    std::size_t count(quint16 port) const
    {
        std::size_t count = 0;
        for (const auto& write : writes) { count += write.port == port; }
        return count;
    }
    bool arm()
    {
        P2TxControl control;
        control.frequencyHz = 14'175'000; control.drive = 12;
        control.antenna = 2; control.disablePa = false;
        return client.configureTransmit(control) && client.enableTransmit(true);
    }
};
std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) { return 0; }
    return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset+1]} << 16)
        | (std::uint32_t{bytes[offset+2]} << 8) | bytes[offset+3];
}
void controlsAndAudio(int board, bool override200D = false)
{
    Fixture f(board, override200D || board == 4 ? AnanModel::Anan200D
        : board == 5 ? AnanModel::Anan7000 : board == 10 ? AnanModel::AnanG2 : AnanModel::Automatic,override200D);
    const auto actor = f.coordinator.registerActor({true,0});
    const auto operation = f.coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    f.client.setDdc0FrequencyHz(7'100'000);
    if (f.last(kHighPriorityPort).empty()) { check(false, "validated fixture emits RX control"); return; }
    check(f.last(kHighPriorityPort)[4] == 1, "RX setters remain receive-only before arming");
    check(!f.client.setTransmitKeying(true, operation), "default gate refuses keying");
    if (!f.arm()) { check(false, "validated board configures raw-drive TX"); return; }
    check(!f.client.setTransmitKeying(true, {}), "enabled transport still refuses missing authority");
    check(f.count(kTxIqPort) == 0 && f.last(kRadioPort)[58] == 0,
          "arming configures PA but never starts IQ or keys");
    check(f.last(kMicControlPort)[15] == 192 && f.last(kMicControlPort)[59] == 31,
          "DUC is 192 ksps with TX ADC attenuation");
    check(f.client.setTransmitKeying(true, operation), "explicit admitted key reaches real client send path");
    check(P2ClientTestAccess::timerActive(f.client), "authority polling starts even before the first IQ block");
    P2ClientTestAccess::speaker(f.client);
    check(P2ClientTestAccess::speakerSilent(f.client), "radio speaker stream is silent while this client keys TX");
    const auto keyed = f.last(kHighPriorityPort);
    check(keyed[4] == 3 && keyed[345] == 12 && word(keyed,329) == phaseWord(14'175'000),
          "TX PTT, raw drive and independent TX frequency encode correctly");
    check((word(keyed,1432) & 0xeff40000u) == ((1u<<27)|(1u<<25)|(1u<<20)|(1u<<18)),
          "Alex0 selects TX antenna, TX low-pass and PA relay");
    check((word(keyed,1428) & (1u<<27)) == 0 && (word(keyed,1428) & (1u<<18)),
          "Alex1 mirrors status but never Alex0 TR relay");
    check(bool(word(keyed,1428) & (1u<<8)) == (!override200D && (board == 5 || board == 10 || board == 20)),
          "MkII BPF2 grounding follows Thetis, independently of ADC count");
    f.client.setRxAntenna(3);
    f.client.setDdc0FrequencyHz(10'100'000);
    f.client.setStepAttenuationDb(0, 7);
    P2ClientTestAccess::keepalive(f.client);
    check(f.last(kHighPriorityPort)[4] == 3 && f.last(kHighPriorityPort)[345] == 12
          && (word(f.last(kHighPriorityPort),1432) & 0x07000000u) == (1u<<25),
          "RX controls and keepalive preserve the admitted TX envelope and antenna");
    check(f.client.updateTransmitDrive(72,operation) && f.last(kHighPriorityPort)[345] == 72,
          "drive update reaches keyed HP packet under original operation");
    const std::size_t writes = f.writes.size();
    check(!f.client.updateTransmitDrive(256,operation)
          && !f.client.updateTransmitDrive(12,TxCoordinator::Operation{}) && f.writes.size() == writes,
          "invalid drive or missing authority cannot write controls");
    const auto producer = f.coordinator.registerProducer(true);
    const auto context = f.coordinator.mediaContext(producer);
    std::array<std::complex<float>,1024> iq;
    iq.fill({0.25f,-0.25f});
    check(f.client.enqueueTransmitIq(iq, operation, context), "real client accepts authorized DSP output");
    P2ClientTestAccess::tick(f.client);
    check(f.count(kTxIqPort) == 1 && f.last(kTxIqPort).size() == 1444
          && P2ClientTestAccess::queued(f.client) == 784, "client sends one paced 240-sample IQ datagram");
    check(f.client.setTransmitKeying(false, operation), "explicit release unkeys");
    check(P2ClientTestAccess::queued(f.client) == 0 && !P2ClientTestAccess::timerActive(f.client),
          "unkey drops carry and stops the timer");
    const auto receive = P2ClientTestAccess::receive(f.client);
    bool restored = true;
    for (std::size_t i = 4; i < receive.size(); ++i) { restored &= receive[i] == f.last(kHighPriorityPort)[i]; }
    check(restored, "unkey restores the entire latest RX packet, including antenna/filter state");
    const auto before = f.count(kTxIqPort);
    P2ClientTestAccess::tick(f.client);
    check(f.count(kTxIqPort) == before, "late timer cannot drain released audio");
    check(f.client.enableTransmit(false) && f.last(kRadioPort)[58] == (board == 10 ? 0 : 1),
          "disabling restores the original receive PA configuration");
    f.client.stop();
    check(!f.client.setTransmitKeying(true, operation), "closed connection cannot replay original key");
}
void faultsAndRestrictions()
{
    check(!p2TransmitProfile(5, AnanModel::Automatic) && !p2TransmitProfile(10, AnanModel::Automatic),
          "ambiguous board families require an explicit transmit model");
    check(!p2TransmitProfile(5, AnanModel::AnvelinaPro3)
          && !p2TransmitProfile(10, AnanModel::AnanG21K)
          && !p2TransmitProfile(1, AnanModel::RedPitaya),
          "unimplemented transmit profiles remain closed");
    Fixture noOverride(5, AnanModel::Anan200D, false);
    check(!noOverride.arm(), "board-5 200D requires explicit override");
    const auto overrideProfile = p2TransmitProfile(5,AnanModel::Anan200D,true);
    check(overrideProfile && overrideProfile->filterKind == ReceiveFilterKind::LegacyHpf
          && !hasAlex2Filters(*overrideProfile), "200D override selects Orion rather than MkII ALEX");
    check(!p2TransmitProfile(5,AnanModel::Anan100D,true)
          && !p2TransmitProfile(10,AnanModel::Anan200D,true)
          && !p2TransmitProfile(5,AnanModel::Automatic,true), "other cross-board overrides stay closed");
    Fixture f;
    check(!f.client.enableTransmit(true), "TX cannot enable before configuration");
    P2TxControl invalid;
    invalid.frequencyHz = std::numeric_limits<double>::quiet_NaN();
    check(!f.client.configureTransmit(invalid), "NaN frequency rejected");
    invalid.frequencyHz = 7'100'000; invalid.drive = 256;
    check(!f.client.configureTransmit(invalid), "drive overflow rejected rather than wrapped");
    const auto actor = f.coordinator.registerActor({true,0});
    const auto operation = f.coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    if (!f.arm()) { check(false, "fixture armed before failure"); return; }
    f.failKey = 1;
    check(!f.client.setTransmitKeying(true, operation) && !P2ClientTestAccess::enabled(f.client)
          && f.last(kHighPriorityPort)[4] == 1 && f.faults == 1,
          "failed key-on is followed by off and cannot automatically replay");
    f.failKey = -1;
    check(f.arm() && f.client.setTransmitKeying(true, operation), "fresh explicit rearm after failure");
    const auto producer = f.coordinator.registerProducer(true);
    std::array<std::complex<float>,1024> iq{};
    check(f.client.enqueueTransmitIq(iq, operation, f.coordinator.mediaContext(producer)), "IQ queued before failure");
    f.failPort = kTxIqPort;
    P2ClientTestAccess::tick(f.client);
    check(!P2ClientTestAccess::enabled(f.client) && f.last(kHighPriorityPort)[4] == 1
          && P2ClientTestAccess::queued(f.client) == 0, "IQ write failure clears samples and unkeys");
    f.failPort = 0;
    check(f.arm() && f.client.setTransmitKeying(true, operation), "explicit rearm before cancellation");
    f.coordinator.revoke(actor);
    P2ClientTestAccess::tick(f.client);
    check(!P2ClientTestAccess::keyed(f.client) && f.last(kHighPriorityPort)[4] == 1,
          "revoked authority unkeys without waiting for microphone data");
    check(f.coordinator.recovering(), "accepted UDP key-off is not radio-idle acknowledgment");

    Fixture failedOff;
    const auto owner = failedOff.coordinator.registerActor({true,0});
    const auto grant = failedOff.coordinator.acquire(owner, TxCoordinator::monotonicMs()).operation;
    check(failedOff.arm() && failedOff.client.setTransmitKeying(true, grant), "fixture before failed off");
    failedOff.failKey = 0;
    check(!failedOff.client.setTransmitKeying(false, grant) && !failedOff.client.isRunning()
          && !P2ClientTestAccess::timerActive(failedOff.client),
          "failed off tears down transport instead of continuing TX keepalives");
    check(!failedOff.client.setTransmitKeying(true, grant), "failed stop cannot silently restart");
}

void rekeyBetweenKeepaliveAndTimer()
{
    Fixture f;
    const auto actor = f.coordinator.registerActor({true,0});
    const auto root = f.coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    bool held = true;
    const auto first = root.withKeyingPermit([&] { return held; });
    const auto second = root.withKeyingPermit([] { return true; });
    const auto producer = f.coordinator.registerProducer(true);
    const auto context = f.coordinator.mediaContext(producer);
    std::array<std::complex<float>,1024> oldIq;
    oldIq.fill({0.25f,-0.25f});
    if (!f.arm() || !f.client.setTransmitKeying(true, first)) {
        check(false, "rekey fixture arms"); return;
    }
    check(f.client.enqueueTransmitIq(oldIq, first, context), "old over has queued IQ before release");
    held = false;
    P2ClientTestAccess::keepalive(f.client);
    check(!P2ClientTestAccess::keyed(f.client) && P2ClientTestAccess::queued(f.client) == 0,
          "keepalive release clears old IQ before another key can arrive");
    check(f.client.setTransmitKeying(true, second), "new explicit key between keepalive and timer");
    std::array<std::complex<float>,1024> newIq;
    newIq.fill({0.125f,0.125f});
    check(f.client.enqueueTransmitIq(newIq, second, context), "new over accepts its own IQ immediately");
    P2ClientTestAccess::tick(f.client);
    const auto expected = buildTxIq(std::span(newIq).first(kTxIqSamplesPerPacket), 0);
    check(expected && f.last(kTxIqPort) == std::vector<std::uint8_t>(expected->begin(), expected->end()),
          "rekey emits only the new over without an old IQ prefix");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        // Only our local socket's OS option is tested; no radio peer or datagrams.
        QUdpSocket socket;
        const bool bound = socket.bind(QHostAddress(QHostAddress::LocalHost),0);
        check(bound,"UDP receive-buffer test binds an ephemeral local socket");
        if (bound) {
            const auto granted = requestUdpReceiveBuffer(socket);
            check(kUdpReceiveBufferBytes == 1'024'000 && granted && *granted > 0,
                  "Thetis-sized UDP receive buffer reports the actual OS result");
            std::printf("UDP SO_RCVBUF requested=%d granted=%lld bytes\n",kUdpReceiveBufferBytes,
                static_cast<long long>(granted.value_or(-1)));
        }
    }
    for (int board : {1,2,3,4,5,10,20}) { controlsAndAudio(board); }
    controlsAndAudio(5,true);
    faultsAndRestrictions();
    rekeyBetweenKeepaliveAndTimer();
    return failures == 0 ? 0 : 1;
}
