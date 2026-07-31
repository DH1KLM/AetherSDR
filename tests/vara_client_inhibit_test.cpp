// The VARA page is receive-only, and this is what makes that structural rather
// than conventional.
//
// A widget test that walked the page looking for transmit-shaped buttons was
// tried first and abandoned: it would have needed RadioModel and its whole
// dependency tree linked into a test, and it could only ever prove that no such
// button exists *today*. The inhibit is better — with it set, the client refuses
// the transmitting commands outright, so wiring up a button by mistake produces
// a refusal and a log line instead of a signal on the air.
//
// Constitution Principle VI: never transmit without operator intent.

#include "core/VaraClient.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

void testDefaultIsPermissive()
{
    // The client itself must stay usable for a future transmitting caller; the
    // inhibit is opt-in, applied by the receive-only page. If the default were
    // inhibited, a real TX path would fail confusingly instead of obviously.
    VaraClient c;
    expect(!c.isTransmitInhibited(),
           "a fresh client is not inhibited: the restriction belongs to the caller");
}

void testInhibitBlocksTransmittingCommands()
{
    VaraClient c;
    c.setTransmitInhibited(true);
    expect(c.isTransmitInhibited(), "the inhibit reports as set");

    QSignalSpy spy(&c, &VaraClient::transmitInhibited);

    const QString connect = c.connectTo(QStringLiteral("KK7GWY-9"),
                                        QStringLiteral("KK7GWY-8"));
    expect(connect.isEmpty(), "connectTo is refused while inhibited");

    const QString cq = c.sendCq(QStringLiteral("KK7GWY"), Vara::Bandwidth::Bw2300);
    expect(cq.isEmpty(), "sendCq is refused while inhibited");

    const qint64 written = c.write(QByteArrayLiteral("payload"));
    expect(written == -1, "payload writes are refused while inhibited");

    expect(spy.count() == 3,
           "each refusal is reported, so a caller can see why nothing happened");
    if (spy.count() == 3) {
        expect(spy.at(0).at(0).toString() == QStringLiteral("CONNECT")
                   && spy.at(1).at(0).toString() == QStringLiteral("CQFRAME")
                   && spy.at(2).at(0).toString() == QStringLiteral("DATA"),
               "the refused command is named in the signal");
    }
}

void testStoppingIsNeverBlocked()
{
    // An inhibit that prevented ENDING a transmission would be a worse safety
    // property than the one it enforces, so disconnectLink and abort are
    // deliberately exempt. With no socket open they return empty for that
    // reason rather than because of the inhibit — what matters here is that
    // they do not report a transmit-inhibited refusal.
    VaraClient c;
    c.setTransmitInhibited(true);
    QSignalSpy spy(&c, &VaraClient::transmitInhibited);
    c.disconnectLink();
    c.abort();
    expect(spy.count() == 0,
           "disconnect and abort are not blocked by the inhibit");
}

void testInhibitIsReversible()
{
    VaraClient c;
    c.setTransmitInhibited(true);
    c.setTransmitInhibited(false);
    expect(!c.isTransmitInhibited(), "the inhibit can be cleared");
    QSignalSpy spy(&c, &VaraClient::transmitInhibited);
    // With no command socket open the command still cannot reach a modem, so
    // this asserts only that the INHIBIT is no longer the thing stopping it.
    c.connectTo(QStringLiteral("KK7GWY-9"), QStringLiteral("KK7GWY-8"));
    expect(spy.count() == 0,
           "once cleared, commands are no longer refused by the inhibit");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDefaultIsPermissive();
    testInhibitBlocksTransmittingCommands();
    testStoppingIsNeverBlocked();
    testInhibitIsReversible();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
