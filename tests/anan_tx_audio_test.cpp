#include "core/backends/anan/AnanClient.h"
#include "core/backends/anan/AnanPlannedResource.h"
#include <atomic>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace AetherSDR::anan {
struct AnanClientTestAccess {
    static P2Client& p2(AnanClient& client) { return *client.m_p2; }
    static void pump(AnanClient& client) { client.pumpTransmitAudio(); }
    static std::size_t pending(const AnanClient& client) { return client.m_transmitCount; }
    static void protocol(AnanClient& client, int value) { client.m_protocol = value; }
};
struct P2ClientTestAccess {
    static void prepare(P2Client& client, qint64& clock,
        std::function<qint64(std::span<const std::uint8_t>, quint16)> writer)
    {
        client.m_running = client.m_linkUp = client.m_discoveryValidated = true;
        client.m_model = AnanModel::Anan200D;
        client.m_expectedBoardId = 4;
        client.m_transmitProfile = receiveProfile(4);
        client.m_generalPacket = buildGeneral(3, true);
        client.m_receiveDisablePa = true;
        client.m_numAdcs = 2;
        client.m_activeDdcs = {{48,0}};
        client.m_controlSendOverride = std::move(writer);
        client.m_transmitTimeOverride = [&clock] { return clock; };
    }
    static void tick(P2Client& client) { client.onTransmitTick(); }
    static std::size_t queued(const P2Client& client) { return client.m_transmitStream.queuedSamples(); }
};
}
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
QByteArray pcm(std::size_t frames, std::size_t offset = 0)
{
    QByteArray bytes(static_cast<qsizetype>(frames * 4), Qt::Uninitialized);
    for (std::size_t i = 0; i < frames; ++i) {
        const qint16 sample = static_cast<qint16>(3000 * std::sin(2 * std::numbers::pi * 1000
            * static_cast<double>(i + offset) / AnanTxDsp::kInputRate));
        std::memcpy(bytes.data() + 4*i, &sample, 2);
        std::memcpy(bytes.data() + 4*i + 2, &sample, 2);
    }
    return bytes;
}
struct Fixture {
    qint64 clockNs = 0;
    std::size_t controlWrites = 0;
    std::vector<TxIqPacket> packets;
    TxCoordinator coordinator{[](const auto&, auto) {}};
    TxCoordinator::Actor actor = coordinator.registerActor({true,0});
    TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    TxCoordinator::Producer producer = coordinator.registerProducer(true);
    TxCoordinator::Context context = coordinator.mediaContext(producer);
    AnanClient client;
    Fixture()
    {
        P2ClientTestAccess::prepare(AnanClientTestAccess::p2(client), clockNs,
            [this](auto bytes, quint16 port) {
                ++controlWrites;
                if (port == kTxIqPort) {
                    check(coordinator.hasInFlightDispatches(), "PCM-to-IQ terminal write retains its grant");
                    TxIqPacket packet{};
                    std::copy(bytes.begin(), bytes.end(), packet.begin());
                    packets.push_back(packet);
                }
                return static_cast<qint64>(bytes.size());
            });
    }
    bool prepare(WdspChannel::Mode mode = WdspChannel::Mode::Usb)
    {
        auto dsp = std::make_unique<AnanTxDsp>();
        if (!dsp->configure(mode)) { return false; }
        P2TxControl control;
        control.frequencyHz = 7'100'000;
        return client.prepareTransmit(std::move(dsp), control);
    }
    bool keyed() { return AnanClientTestAccess::p2(client).transmitActive(); }
    void drain()
    {
        auto& transport = AnanClientTestAccess::p2(client);
        for (int i = 0; i < 16 && P2ClientTestAccess::queued(transport) >= kTxIqSamplesPerPacket; ++i) {
            P2ClientTestAccess::tick(transport);
            clockNs += AnanP2TxStream::kPacketNs;
        }
    }
    bool exchange()
    {
        QElapsedTimer timer;
        timer.start();
        while (AnanClientTestAccess::pending(client) >= AnanTxDsp::kInputSamples && timer.elapsed() < 1000) {
            AnanClientTestAccess::pump(client);
            drain();
            QThread::msleep(1);
        }
        return keyed() && AnanClientTestAccess::pending(client) < AnanTxDsp::kInputSamples;
    }
};
void pcmToWire()
{
    Fixture f;
    const auto block = pcm(128);
    check(!f.client.submitTransmitAudio(block,24000,TxAudioSource::Microphone,f.operation,f.context),
          "unkeyed PCM is not retained as a future microphone pre-roll");
    if (!f.prepare()) { check(false, "prebuilt TX DSP installs"); return; }
    check(f.client.transmitMode() == WdspChannel::Mode::Usb && !f.keyed() && f.packets.empty(),
          "preparation reports its exact mode and never keys");
    check(f.client.setTransmitKeying(true,f.operation), "voice envelope keys explicitly");
    for (std::size_t i = 0; i < 50; ++i) {
        check(f.client.submitTransmitAudio(pcm(128,i*128),24000,TxAudioSource::Microphone,f.operation,f.context),
              "stereo PCM accepts bounded microphone blocks");
        if (!f.exchange()) { check(false, "bounded nonblocking PCM-to-IQ exchange"); return; }
    }
    bool nonzero = false;
    for (const auto& packet : f.packets) {
        for (std::size_t i = 4; i < packet.size(); ++i) { nonzero |= packet[i] != 0; }
    }
    check(nonzero && f.packets.size() == (50 * AnanTxDsp::kOutputSamples / kTxIqSamplesPerPacket),
          "real USB TXA output reaches encoded P2 IQ without sample loss");
    check(f.client.submitTransmitAudio(pcm(64),24000,TxAudioSource::Microphone,f.operation,f.context),
          "partial DSP block is carried within the current over");
    check(f.client.setTransmitKeying(false,f.operation) && AnanClientTestAccess::pending(f.client) == 0,
          "PTT release discards partial PCM immediately");
    const auto before = f.packets.size();
    AnanClientTestAccess::pump(f.client);
    check(f.packets.size() == before, "late audio pump cannot drain released PCM");
    check(f.client.setTransmitKeying(true,f.operation), "fresh key reuses prepared plans");
    check(f.client.submitTransmitAudio(QByteArray(512,0),24000,TxAudioSource::Microphone,f.operation,f.context),
          "new silent over starts with a fresh media context");
    check(f.exchange(), "new silent over exchanges");
    bool silent = true;
    for (std::size_t p = before; p < f.packets.size(); ++p) {
        for (std::size_t i = 4; i < f.packets[p].size(); ++i) { silent &= f.packets[p][i] == 0; }
    }
    check(silent, "new over has no old PCM, filter or IQ tail");
    f.producer.invalidate();
    AnanClientTestAccess::pump(f.client);
    check(!f.keyed(), "expired media stops the voice envelope");
}
void plannedRetirement()
{
    QThread worker;
    QObject planner;
    QThread* caller = QThread::currentThread();
    planner.moveToThread(&worker);
    worker.start();
    std::atomic<QThread*> retiredOn{nullptr};
    struct Probe {
        std::atomic<QThread*>& retiredOn;
        ~Probe() { retiredOn.store(QThread::currentThread()); }
    };
    auto resource = retainPlannedResource(std::make_unique<Probe>(retiredOn),&planner);
    resource.reset();
    QMetaObject::invokeMethod(&planner,[] {},Qt::BlockingQueuedConnection);
    check(retiredOn.load() == &worker, "last consumer reference retires on planner");
    retiredOn.store(nullptr);
    {
        QObject droppedDelivery;
        auto pending = retainPlannedResource(std::make_unique<Probe>(retiredOn),&planner);
        QMetaObject::invokeMethod(&droppedDelivery,[pending = std::move(pending)] {},Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(&planner,[] {},Qt::BlockingQueuedConnection);
    check(retiredOn.load() == &worker, "discarded queued delivery also retires on planner");
    QMetaObject::invokeMethod(&planner,[&] { planner.moveToThread(caller); },Qt::BlockingQueuedConnection);
    worker.quit(); worker.wait();
}

void stagedDoesNotKey()
{
    Fixture f;
    const std::size_t before = f.controlWrites;
    auto dsp = std::make_unique<AnanTxDsp>();
    check(dsp->configure(WdspChannel::Mode::Usb), "staging fixture builds real DSP");
    check(f.client.stageTransmitDsp(retainPlannedResource(std::move(dsp),nullptr)), "local staging accepts valid DSP");
    check(!f.client.setTransmitKeying(true,f.operation) && !f.keyed(), "staged DSP alone refuses even authorized key request");
    f.client.discardStagedTransmit();
    check(!f.client.transmitMode(), "discard removes staged DSP");
    check(f.controlWrites == before, "staging, refused key and discard send no radio packets");
}
void refusalPaths()
{
    Fixture f;
    const auto block = pcm(128);
    check(f.prepare() && f.client.setTransmitKeying(true,f.operation), "format refusal fixture keys");
    check(!f.client.submitTransmitAudio(block,48000,TxAudioSource::Microphone,f.operation,f.context)
          && !f.keyed(), "wrong PCM rate unkeys rather than changing voice pitch");
    check(f.prepare() && f.client.setTransmitKeying(true,f.operation), "source refusal fixture keys");
    check(f.client.submitTransmitAudio(block,24000,TxAudioSource::Microphone,f.operation,f.context),
          "microphone source is captured");
    check(!f.client.submitTransmitAudio(block,24000,TxAudioSource::ClientLeveled,f.operation,f.context)
          && !f.keyed(), "source change cannot relabel pending PCM");
    check(f.prepare() && f.client.setTransmitKeying(true,f.operation), "overflow fixture keys");
    check(!f.client.submitTransmitAudio(pcm(2049),24000,TxAudioSource::Microphone,f.operation,f.context)
          && !f.keyed(), "PCM overflow stops instead of accumulating latency");
    AnanClientTestAccess::protocol(f.client,1);
    check(!f.client.setTransmitKeying(true,f.operation), "P1 never routes into the P2 transmitter");
    check(f.packets.empty(), "refused voice sources emitted no IQ packets");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    pcmToWire();
    refusalPaths();
    return failures == 0 ? 0 : 1;
}
