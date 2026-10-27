#include "core/backends/anan/AnanP2TxStream.h"

#include <QCoreApplication>

#include <array>
#include <cstdio>
#include <limits>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto operation = coordinator.acquire(actor, 0).operation;
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    const auto keyWriter = [](bool) { return true; };
    AnanTxGate gate;
    gate.setEnabled(true, 0, keyWriter);
    check(gate.setKeying(true, operation, 0, keyWriter), "fixture key accepted");
    AnanP2TxStream stream;
    check(stream.begin(operation, context, 0, 0), "stream begins with original authority");
    std::array<std::complex<float>, 1024> block;
    block.fill({0.25f, -0.25f});
    check(stream.enqueue(block, context, 0), "one DSP block accepted");
    std::vector<TxIqPacket> writes;
    const auto writer = [&](const TxIqPacket& packet) { writes.push_back(packet); return true; };
    check(stream.pump(0, 0, gate, writer) == AnanP2TxStream::Result::Sent,
          "first full frame sent");
    check(stream.pump(0, 0, gate, writer) == AnanP2TxStream::Result::NotDue
          && writes.size() == 1, "repeated callback cannot emit a packet burst");
    for (int i = 1; i < 4; ++i) {
        check(stream.pump(i * AnanP2TxStream::kPacketNs, 0, gate, writer)
              == AnanP2TxStream::Result::Sent, "sample clock drains one 240-sample packet");
    }
    check(stream.queuedSamples() == 64, "DSP block remainder is conserved");
    check(stream.pump(4 * AnanP2TxStream::kPacketNs, 0, gate, writer)
          == AnanP2TxStream::Result::Silence && stream.queuedSamples() == 64,
          "underflow emits silence without dropping the partial frame");
    bool silent = true;
    for (std::size_t i = 4; i < writes.back().size(); ++i) { silent &= writes.back()[i] == 0; }
    check(silent && writes.back()[3] == 4, "silence carries the next sequence");
    check(stream.enqueue(std::span(block).first(176), context, 0), "partial frame completes");
    check(stream.pump(20'000'000, 0, gate, writer) == AnanP2TxStream::Result::Sent,
          "late callback sends only one packet");
    check(stream.pump(20'000'000, 0, gate, writer) == AnanP2TxStream::Result::NotDue,
          "late callback rebases rather than replaying historical deadlines");
    stream.clear();
    check(stream.queuedSamples() == 0 && stream.begin(operation, context, 0, 0),
          "new over clears all sample carry");
    std::array<std::complex<float>, AnanP2TxStream::kCapacity> full{};
    check(stream.enqueue(full, context, 0), "bounded queue accepts its capacity");
    check(!stream.enqueue(std::span(block).first(1), context, 0) && stream.queuedSamples() == 0,
          "queue overflow retires all pending audio instead of growing latency");
    check(stream.begin(operation, context, 0, 0), "fresh stream after overflow");
    const auto other = coordinator.registerProducer(true);
    check(!stream.enqueue(block, coordinator.mediaContext(other), 0),
          "different producer cannot relabel pending audio");
    check(stream.begin(operation, context, 0, 0), "fresh stream before cancellation");
    check(stream.enqueue(block, context, 0), "queue populated before cancellation");
    producer.invalidate();
    const std::size_t before = writes.size();
    check(stream.pump(0, 0, gate, writer) == AnanP2TxStream::Result::Refused
          && writes.size() == before && stream.queuedSamples() == 0,
          "cancelled media cannot reach terminal transport");
    const auto replacement = coordinator.mediaContext(other);
    check(stream.begin(operation, replacement, 10, 0), "new producer starts fresh stream");
    check(stream.pump(9, 0, gate, writer) == AnanP2TxStream::Result::Refused,
          "clock rollback fails closed");
    check(stream.begin(operation, replacement, 0, 0), "fresh stream before prolonged stall");
    check(stream.enqueue(block, replacement, 0), "queue before prolonged stall");
    check(stream.pump(16 * AnanP2TxStream::kPacketNs, 0, gate, writer)
          == AnanP2TxStream::Result::Refused && stream.queuedSamples() == 0,
          "stall beyond queue duration drops historical IQ");
    stream.transportClosed();
    check(stream.begin(operation, replacement, 0, 0), "fresh sample clock");
    std::array<std::complex<float>, kTxIqSamplesPerPacket> packetSamples{};
    std::size_t packets = 0;
    const auto orderedWriter = [&](const TxIqPacket& packet) {
        const auto expected = buildTxIq(packetSamples, static_cast<std::uint32_t>(packets));
        check(expected && *expected == packet, "ring wrap retains every IQ sample and sequence");
        ++packets;
        return true;
    };
    for (int i = 0; i < 800; ++i) {
        for (std::size_t j = 0; j < packetSamples.size(); ++j) {
            packetSamples[j] = {static_cast<float>((i + j) % 100) / 200.0f, -0.125f};
        }
        check(stream.enqueue(packetSamples, replacement, i), "sample clock accepts one frame");
        check(stream.pump(i * AnanP2TxStream::kPacketNs, i, gate, orderedWriter)
              == AnanP2TxStream::Result::Sent, "sample clock emits exactly one due frame");
    }
    check(packets * kTxIqSamplesPerPacket == 192000 && stream.queuedSamples() == 0,
          "one second conserves exactly 192000 samples across ring wraps");
    check(stream.begin(operation, replacement, 0, 0), "fresh stream before failed write");
    check(stream.pump(0, 0, gate, [](const TxIqPacket&) { return false; })
          == AnanP2TxStream::Result::WriteFailed && gate.stopPending(),
          "failed packet closes the IQ gate and requires off");
    stream.transportClosed();
    gate.transportClosed();
    check(stream.pump(0, 0, gate, writer) == AnanP2TxStream::Result::Refused,
          "closed transport cannot restart old queue");
    return failures == 0 ? 0 : 1;
}
