#include "core/VaraSessionController.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR::Vara;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

void testSessionStateTransitions()
{
    Transmitter tx;
    Receiver rx;
    SessionController session;
    session.setTransmitter(&tx);
    session.setReceiver(&rx);

    expect(session.state() == SessionController::State::Disconnected, "Initial state is Disconnected");

    session.setMyCall("NF0T");
    session.connectTo("W1AW");
    expect(session.state() == SessionController::State::Calling, "State transitions to Calling on connectTo");

    session.abortSession();
    expect(session.state() == SessionController::State::Disconnected, "State transitions to Disconnected on abort");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("=== Running vara_session_controller_test ===\n");
    testSessionStateTransitions();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
