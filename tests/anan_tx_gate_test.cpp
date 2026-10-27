#include "core/backends/anan/AnanTxGate.h"

#include <QCoreApplication>

#include <cstdio>
#include <vector>

using AetherSDR::TxCoordinator;
using AetherSDR::anan::AnanTxGate;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void admissionAndMedia()
{
    qint64 now = 0;
    TxCoordinator coordinator([](const auto&, auto) {}, [&] { return now; });
    const auto actor = coordinator.registerActor({true, 0});
    const auto operation = coordinator.acquire(actor, now).operation;
    const auto microphone = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(microphone);
    AnanTxGate gate;
    std::vector<bool> writes;
    const auto writer = [&](bool key) {
        check(coordinator.hasInFlightDispatches(), "key/off write holds dispatch lease");
        writes.push_back(key);
        return true;
    };
    check(!gate.setKeying(true, operation, now, writer) && writes.empty(),
          "disabled gate rejects even an admitted operation");
    check(gate.setEnabled(true, now, writer) && writes.empty(),
          "enabling cannot replay a previously refused key request");
    check(!gate.setKeying(true, {}, now, writer) && writes.empty(),
          "enabled gate does not manufacture authority");
    check(gate.setKeying(true, operation, now, writer) && gate.keyCommandAccepted(),
          "explicit admitted key reaches the terminal writer");
    int iqWrites = 0;
    const auto iqWriter = [&] {
        check(coordinator.hasInFlightDispatches(), "IQ write holds original key dispatch");
        ++iqWrites;
        return true;
    };
    check(!gate.writeIq(operation, {}, now, iqWriter) && iqWrites == 0,
          "keyed gate refuses IQ without original media provenance");
    check(gate.writeIq(operation, context, now, iqWriter) && iqWrites == 1,
          "valid media and admitted key permit one terminal IQ write");
    microphone.invalidate();
    check(!gate.writeIq(operation, context, now, iqWriter) && iqWrites == 1,
          "revoked media producer fences queued IQ");
    check(gate.setEnabled(false, now, writer) && !gate.keyCommandAccepted(),
          "closing the gate commands off before accepting more IQ");
    check(writes == std::vector<bool>({true, false}), "enable/disable writes one on and one off");
    check(gate.setEnabled(true, now, writer) && writes.size() == 2,
          "reopening still needs a new explicit key request");
    check(!gate.writeIq(operation, context, now, iqWriter), "reopening cannot resume IQ");
}

void failedWrites()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto operation = coordinator.acquire(actor, 0).operation;
    AnanTxGate gate;
    std::vector<bool> writes;
    bool accept = false;
    const auto writer = [&](bool key) { writes.push_back(key); return accept; };
    check(gate.setEnabled(true, 0, writer), "gate enables without writing");
    check(!gate.setKeying(true, operation, 0, writer) && gate.stopPending(),
          "ambiguous failed key-on requires off");
    check(!gate.setKeying(true, operation, 0, writer) && writes.size() == 1,
          "pending off blocks repeated key-on");
    check(!gate.poll(0, writer) && writes == std::vector<bool>({true, false}),
          "failed key is never automatically retried on");
    accept = true;
    check(gate.poll(0, writer) && !gate.stopPending() && !gate.keyCommandAccepted(),
          "successful off clears ambiguity but never rekeys");
    check(gate.setKeying(true, operation, 0, writer), "fresh explicit key can follow successful off");
    const auto microphone = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(microphone);
    check(!gate.writeIq(operation, context, 0, [] { return false; })
          && gate.stopPending() && !gate.keyCommandAccepted(),
          "failed terminal IQ write blocks further media pending off");
    int iqWrites = 0;
    check(!gate.writeIq(operation, context, 0, [&] { ++iqWrites; return true; })
          && iqWrites == 0, "failed IQ cannot silently resume");
    check(gate.poll(0, writer) && !gate.stopPending(), "IQ failure is followed only by off");
    check(gate.setKeying(true, operation, 0, writer), "fresh explicit key after IQ recovery");
    accept = false;
    check(!gate.setEnabled(false, 0, writer) && gate.stopPending() && !gate.enabled(),
          "failed disable closes admission and retains off obligation");
    accept = true;
    check(gate.poll(0, writer) && !gate.stopPending() && !gate.enabled(),
          "off retry cannot reopen disabled gate");
}

void cancellationAndReconnect()
{
    qint64 now = 0;
    int stops = 0;
    TxCoordinator coordinator([&](const auto&, auto) { ++stops; }, [&] { return now; });
    const auto actor = coordinator.registerActor({true, 100});
    const auto first = coordinator.acquire(actor, now).operation;
    AnanTxGate gate;
    std::vector<bool> writes;
    const auto writer = [&](bool key) { writes.push_back(key); return true; };
    gate.setEnabled(true, now, writer);
    check(gate.setKeying(true, first, now, writer), "bounded operation keys");
    now = 100;
    check(gate.poll(now, writer) && writes == std::vector<bool>({true, false}),
          "deadline expiry commands off without another operator request");
    check(!gate.setKeying(true, first, now, writer), "expired original cannot be replayed");
    coordinator.expire(now);
    check(stops == 1 && coordinator.recovering(),
          "terminal off does not claim qualified radio idle");
    check(coordinator.acknowledgeStopped(first), "fixture supplies independent stop evidence");
    const auto second = coordinator.acquire(actor, now).operation;
    check(gate.setKeying(true, second, now, writer), "new operation keys after qualified recovery");
    check(!gate.setKeying(false, first, now, writer) && gate.keyCommandAccepted(),
          "late old key-up cannot unkey the replacement operation");
    coordinator.reset();
    const std::size_t before = writes.size();
    check(!gate.poll(now, writer) && gate.stopPending() && writes.size() == before,
          "reset fences all old-session writes, including cleanup");
    gate.transportClosed();
    check(!gate.enabled() && !gate.stopPending() && !gate.keyCommandAccepted(),
          "closed transport retires local state and next session stays disabled");
    check(!gate.setKeying(true, second, now, writer), "reconnect cannot revive old authority");
}

void producerFence()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto original = coordinator.acquire(actor, 0).operation;
    bool firstPermit = true;
    const auto first = original.withKeyingPermit([&] { return firstPermit; });
    const auto second = original.withKeyingPermit([] { return true; });
    AnanTxGate gate;
    std::vector<bool> writes;
    const auto writer = [&](bool key) { writes.push_back(key); return true; };
    gate.setEnabled(true, 0, writer);
    check(gate.setKeying(true, first, 0, writer), "first restricted producer keys");
    check(!gate.setKeying(true, second, 0, writer) && writes.size() == 1,
          "same operation with different producer fence cannot replace active authority");
    check(!gate.setKeying(false, second, 0, writer), "different authority cannot consume another key");
    firstPermit = false;
    check(gate.poll(0, writer) && writes == std::vector<bool>({true, false}),
          "producer cancellation fences sustain and commands off");
    check(gate.setKeying(true, second, 0, writer), "new producer requires explicit fresh key");
    check(!gate.setKeying(false, first, 0, writer) && gate.keyCommandAccepted(),
          "old producer cleanup cannot consume replacement within same operation");
    coordinator.revoke(actor);
    check(gate.poll(0, writer) && !gate.keyCommandAccepted(), "actor revocation commands off");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    admissionAndMedia();
    failedWrites();
    cancellationAndReconnect();
    producerFence();
    return failures == 0 ? 0 : 1;
}
