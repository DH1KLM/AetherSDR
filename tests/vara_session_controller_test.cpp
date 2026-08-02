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

// Property 3: the armed state must not survive a session. A dropped link, an
// abort or a reconnect all have to come back inhibited, or an arm granted once
// silently carries into the next session.
void testArmDoesNotSurviveSession()
{
    Transmitter tx;
    SessionController sc;
    sc.setTransmitter(&tx);

    expect(!tx.isArmed(), "attaching a transmitter does not arm it");

    tx.armForSession();
    expect(tx.isArmed(), "operator armed the session");

    sc.abortSession();   // -> State::Disconnected
    expect(!tx.isArmed(),
           "aborting the session disarms the transmitter");

    tx.armForSession();
    sc.disconnectSession();
    expect(!tx.isArmed(),
           "a normal disconnect disarms the transmitter");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("=== Running vara_session_controller_test ===\n");
    testSessionStateTransitions();
    testArmDoesNotSurviveSession();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
