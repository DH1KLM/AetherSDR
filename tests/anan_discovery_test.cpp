//DH1KLM aetherd ANAN P2 -- discovery broadcast targeting, and what discovery logs
//DH1KLM when it cannot find a radio (#6299).
//DH1KLM
//DH1KLM WHAT THIS IS FOR. A discovery datagram sent to 255.255.255.255 leaves by one
//DH1KLM interface: whichever the host's default route picks. On a machine with the
//DH1KLM radio on a second NIC on a distinct subnet, the default broadcast can miss
//DH1KLM it. AnanDiscovery::broadcastTargets() turns the
//DH1KLM host's own address entries into one directed broadcast per subnet.
//DH1KLM
//DH1KLM It is tested here rather than against a live host because the interesting
//DH1KLM network shapes (multi-homed, a point-to-point tunnel with no broadcast
//DH1KLM address, IPv6 alongside IPv4) are exactly the ones a developer machine and a
//DH1KLM CI runner do not have. QNetworkAddressEntry can be built by hand; a
//DH1KLM QNetworkInterface cannot, which is why the flag filtering sits in
//DH1KLM AnanDiscovery.cpp and only this half is covered.
//DH1KLM
//DH1KLM THE LOG LINES. AnanDiscoveryLog is the policy AnanDiscovery feeds from its
//DH1KLM sweeps: a failed bind, a sweep with no subnet to probe, a probe that could
//DH1KLM not be sent, and a run of unanswered sweeps, each logged once per
//DH1KLM occurrence. The lines are captured off the real "aether.discovery" category
//DH1KLM with the product's default filter rule installed, so a line this test sees
//DH1KLM reaches a support bundle.
//DH1KLM
//DH1KLM No sockets, no radio, nothing sent.

#include "core/backends/anan/AnanDiscovery.h"
#include "core/LogManager.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QList>
#include <QLoggingCategory>
#include <QNetworkAddressEntry>
#include <QStringList>

#include <cstdio>
#include <functional>

namespace AetherSDR::anan {
//DH1KLM AnanDiscovery's friend: whether it holds a socket, read without opening one.
class AnanDiscoveryTestAccess {
public:
    static bool hasSocket(const AnanDiscovery& d) { return d.m_socket != nullptr; }
};
}  //DH1KLM namespace AetherSDR::anan

using AetherSDR::anan::AnanDiscovery;
using AetherSDR::anan::AnanDiscoveryLog;
using AetherSDR::anan::AnanDiscoveryTestAccess;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

QStringList* g_lines = nullptr;

void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (!g_lines || !ctx.category
        || qstrcmp(ctx.category, "aether.discovery") != 0) {
        return;
    }
    *g_lines << QStringLiteral("%1|%2")
                    .arg(type == QtWarningMsg ? QStringLiteral("W")
                         : type == QtInfoMsg  ? QStringLiteral("I")
                                              : QStringLiteral("other"),
                         msg);
}

//DH1KLM Captures what one step logs, and nothing from the steps around it.
QStringList logged(const std::function<void()>& step)
{
    QStringList lines;
    g_lines = &lines;
    step();
    g_lines = nullptr;
    return lines;
}

const QHostAddress kGlobal(QHostAddress::Broadcast);
const QHostAddress kSubnetA(QStringLiteral("192.168.8.255"));
const QHostAddress kSubnetB(QStringLiteral("10.0.5.255"));
const QString kSilence = QStringLiteral("no supported OpenHPSDR radio answered");

//DH1KLM One sweep as AnanDiscovery reports it: its target list, a probe to each,
//DH1KLM a listed reply if `answered`, then the reply window closing.
void sentSweep(AnanDiscoveryLog& n, bool answered)
{
    n.targetsListed({kGlobal, kSubnetA});
    n.sendSucceeded(kGlobal);
    n.sendSucceeded(kSubnetA);
    if (answered) {
        n.replyListed();
    }
    n.sweepClosed();
}

//DH1KLM A sweep whose every probe failed: nothing was asked.
void failedSweep(AnanDiscoveryLog& n)
{
    n.targetsListed({kGlobal, kSubnetA});
    n.sendFailed(kGlobal, QStringLiteral("Network unreachable"));
    n.sendFailed(kSubnetA, QStringLiteral("Network unreachable"));
    n.sweepClosed();
}

void bindFailureIsLoggedOncePerCondition()
{
    AnanDiscoveryLog n;
    const auto failedBind = [&n] {
        n.bindFailed(QStringLiteral("The address is not available"));
        n.sweepClosed();
    };

    check(logged(failedBind) == QStringList{QStringLiteral(
              "W|ANAN discovery: cannot bind a UDP socket: The address is not available")},
          "bind failure: the first sweep logs one warning naming the error");
    check(logged(failedBind).isEmpty() && logged(failedBind).isEmpty(),
          "bind failure: the sweeps that retry it log nothing");
    check(logged([&n] {
              for (int i = 0; i < 5; ++i) {
                  n.sweepClosed();
              }
          }).isEmpty(),
          "bind failure: sweeps no probe left log no silence line");

    logged([&n] { n.bindSucceeded(); });
    check(logged(failedBind).size() == 1,
          "bind failure: a failure after a successful bind is logged again");
}

void noSubnetIsLoggedOncePerCondition()
{
    AnanDiscoveryLog n;
    const auto globalOnly = [&n] { n.targetsListed({kGlobal}); };

    check(logged([&n] { n.targetsListed({kGlobal, kSubnetA}); }).isEmpty(),
          "no subnet: a sweep with a directed broadcast logs nothing");
    check(logged(globalOnly) == QStringList{QStringLiteral(
              "W|ANAN discovery: no IPv4 interface has a broadcast address; "
              "probing the global broadcast only")},
          "no subnet: the first global-only sweep logs one warning");
    check(logged(globalOnly).isEmpty() && logged(globalOnly).isEmpty(),
          "no subnet: later global-only sweeps log nothing while it persists");

    logged([&n] { n.targetsListed({kGlobal, kSubnetB}); });
    check(logged(globalOnly).size() == 1,
          "no subnet: logged again after a subnet came back and went");
}

void sendFailureIsLoggedOncePerCondition()
{
    AnanDiscoveryLog n;
    const auto failA = [&n] { n.sendFailed(kSubnetA, QStringLiteral("Network unreachable")); };

    check(logged(failA) == QStringList{QStringLiteral(
              "W|ANAN discovery: cannot send a probe to 192.168.8.255 (P2): Network unreachable")},
          "send failure: the first failure logs one warning naming target and error");
    check(logged(failA).isEmpty(),
          "send failure: the same target failing on later sweeps logs nothing");

    const QStringList other = logged([&n] { n.sendFailed(kSubnetB, QStringLiteral("x")); });
    check(other.size() == 1 && other.first().contains(QStringLiteral("to 10.0.5.255: x")),
          "send failure: another target is its own condition");

    check(logged([&n] { n.sendFailed(kGlobal, QStringLiteral("x")); })
              == QStringList{QStringLiteral(
                  "W|ANAN discovery: cannot send a probe to the global broadcast (P2): x")},
          "send failure: 255.255.255.255 is named, not left to the log's redaction");

    check(logged([&n] { n.sendSucceeded(kSubnetA); }).isEmpty() && logged(failA).size() == 1,
          "send failure: a failure after a probe left for that target is logged again");
}

void silenceIsLoggedOnceAfterThreshold()
{
    static_assert(AnanDiscoveryLog::kSilentSweepsBeforeNotice == 3,
                  "the strings below spell out the threshold");
    AnanDiscoveryLog n;

    check(logged([&n] {
              sentSweep(n, false);
              sentSweep(n, false);
          }).isEmpty(),
          "no reply: two silent sweeps log nothing");
    check(logged([&n] { sentSweep(n, false); }) == QStringList{QStringLiteral(
              "I|ANAN discovery: no supported OpenHPSDR radio answered 3 sweeps to 2 addresses")},
          "no reply: the third silent sweep logs one Info line with the target count");
    check(logged([&n] {
              for (int i = 0; i < 20; ++i) {
                  sentSweep(n, false);
              }
          }).isEmpty(),
          "no reply: continued silence logs nothing more");

    check(logged([&n] {
              sentSweep(n, true);
              sentSweep(n, false);
              sentSweep(n, false);
          }).isEmpty(),
          "no reply: a listed reply resets the count; two new silent sweeps log nothing");
    check(logged([&n] {
              n.targetsListed({kGlobal});
              n.sendSucceeded(kGlobal);
              n.sweepClosed();
          }) == QStringList{QStringLiteral(
              "W|ANAN discovery: no IPv4 interface has a broadcast address; "
              "probing the global broadcast only"),
              QStringLiteral("I|ANAN discovery: no supported OpenHPSDR radio answered 3 sweeps to 1 address")},
          "no reply: silence after a reply is logged again (singular address)");

    check(logged([&n] {
              sentSweep(n, false);
              sentSweep(n, true);
              sentSweep(n, false);
              sentSweep(n, true);
          }).isEmpty(),
          "no reply: an intermittent reply never reaches the threshold");
}

//DH1KLM The silence line means "asked and not answered". A sweep whose every send
//DH1KLM failed asked nothing; the send-failure warning is its whole report.
void sweepWithNoProbeSentIsNotSilence()
{
    AnanDiscoveryLog allFail;
    const QStringList failed = logged([&allFail] {
        for (int i = 0; i < 3; ++i) {
            failedSweep(allFail);
        }
    });
    check(failed.filter(QStringLiteral("cannot send a probe to 192.168.8.255")).size() == 1
              && failed.filter(QStringLiteral("cannot send a probe to the global broadcast")).size() == 1,
          "no probe sent: three all-failed sweeps log each target's send warning once");
    check(failed.filter(kSilence).isEmpty() && failed.size() == 2,
          "no probe sent: three all-failed sweeps log no silence line");
    check(logged([&allFail] {
              for (int i = 0; i < 20; ++i) {
                  failedSweep(allFail);
              }
          }).isEmpty(),
          "no probe sent: continued all-failed sweeps log nothing more");

    AnanDiscoveryLog mixed;
    const QStringList partial = logged([&mixed] {
        for (int i = 0; i < 3; ++i) {
            mixed.targetsListed({kGlobal, kSubnetA});
            mixed.sendFailed(kGlobal, QStringLiteral("Network unreachable"));
            mixed.sendSucceeded(kSubnetA);
            mixed.sweepClosed();
        }
    });
    check(partial.filter(kSilence).size() == 1 && partial.size() == 2,
          "no probe sent: one target sent, one failed -> the sweep counts as sent");
    check(partial.filter(kSilence).value(0).endsWith(
              QStringLiteral("answered 3 sweeps to 1 address")),
          "no probe sent: two listed, one sent -> the silence line counts the one sent");

    //DH1KLM The mark belongs to one sweep: a probe that left sweep 1 does not make
    //DH1KLM the failed sweeps after it count.
    AnanDiscoveryLog stale;
    check(logged([&stale] {
              sentSweep(stale, false);
              for (int i = 0; i < 5; ++i) {
                  failedSweep(stale);
              }
          }).filter(kSilence).isEmpty(),
          "no probe sent: one sent sweep then failed ones never reach the threshold");
    check(logged([&stale] {
              sentSweep(stale, false);
              sentSweep(stale, false);
          }).filter(kSilence).size() == 1,
          "no probe sent: failed sweeps do not clear the count; two more sent ones log it");

    //DH1KLM A sweep that was answered closes its own mark too.
    AnanDiscoveryLog answered;
    check(logged([&answered] {
              sentSweep(answered, true);
              failedSweep(answered);
              sentSweep(answered, false);
              sentSweep(answered, false);
          }).filter(kSilence).isEmpty(),
          "no probe sent: a probe that left an answered sweep does not count for the next");
}

void resetForgetsEverything()
{
    AnanDiscoveryLog n;
    const auto everything = [&n] {
        n.bindFailed(QStringLiteral("x"));
        n.targetsListed({kGlobal});
        n.sendFailed(kSubnetB, QStringLiteral("x"));
        for (int i = 0; i < 3; ++i) {
            sentSweep(n, false);
        }
    };
    logged(everything);
    n.reset();
    check(logged(everything).size() == 4,
          "reset (AnanDiscovery::stop): all four conditions are logged again");
}

//DH1KLM A discovery that is not running opens no socket, so sends nothing. Never
//DH1KLM started is the same state stop() leaves: timer stopped, no socket.
void protocolFailuresAreIndependent()
{
    AnanDiscoveryLog log;
    const auto failP1 = [&log] { log.sendFailed(kSubnetA, QStringLiteral("failed"), 1); };
    const auto failP2 = [&log] { log.sendFailed(kSubnetA, QStringLiteral("failed"), 2); };
    const QStringList initial = logged([&] { failP1(); failP2(); });
    check(initial.size() == 2 && initial.first().contains(QStringLiteral("(P1)"))
              && initial.last().contains(QStringLiteral("(P2)")),
          "P1 and P2 failures at the same address have separate diagnostics");
    log.sendSucceeded(kSubnetA, 2);
    check(logged(failP1).isEmpty(), "P2 success does not re-arm P1's failure");
    check(logged(failP2).size() == 1, "P2 success re-arms only P2's failure");
    log.sendSucceeded(kSubnetA, 1);
    check(logged(failP1).size() == 1, "P1 success re-arms P1's failure");
    log.reset();
    check(logged(failP1).size() == 1, "reset clears P1 diagnostics too");
}

void bothProtocolsCountOneTargetAndAnyAcceptedReply()
{
    AnanDiscoveryLog log;
    const auto silent = [&log] {
        log.targetsListed({kGlobal, kSubnetA});
        log.sendSucceeded(kSubnetA, 1);
        log.sendSucceeded(kSubnetA, 2);
        log.sweepClosed();
    };
    const QStringList initial = logged([&] { silent(); silent(); silent(); });
    check(initial.size() == 1 && initial.first().endsWith(QStringLiteral("3 sweeps to 1 address")),
          "two protocols sent to one target count one address and one sweep");
    log.sendSucceeded(kSubnetA, 1);
    log.replyListed();
    log.sweepClosed();
    check(logged([&] { silent(); silent(); }).isEmpty(),
          "an accepted reply after a P1 probe resets the silence counter");
    check(logged(silent).size() == 1, "a fresh silence notice follows three new sent sweeps");
}

void sweepWhileStoppedSendsNothing()
{
    AnanDiscovery discovery;
    discovery.sweepNow();
    check(!AnanDiscoveryTestAccess::hasSocket(discovery),
          "sweepNow() on a discovery that is not running opens no socket");
}

}  //DH1KLM namespace

//DH1KLM One ordinary IPv4 entry: an address, its netmask, and the broadcast address
//DH1KLM the platform derives from them.
static QNetworkAddressEntry ipv4Entry(const char* ip, const char* mask, const char* broadcast)
{
    QNetworkAddressEntry e;
    e.setIp(QHostAddress(QLatin1String(ip)));
    e.setNetmask(QHostAddress(QLatin1String(mask)));
    e.setBroadcast(QHostAddress(QLatin1String(broadcast)));
    return e;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QHostAddress global(QHostAddress::Broadcast);   //DH1KLM 255.255.255.255

    //DH1KLM THE PROPERTY THAT MATTERS MOST: the global broadcast is always present and
    //DH1KLM always first. Everything else is added to what this already did, so a host
    //DH1KLM whose enumeration is empty, wrong, or unlike anything anticipated here is
    //DH1KLM no worse off than before the change. A regression that made the directed
    //DH1KLM addresses REPLACE it would break discovery on every single-NIC machine.
    {
        const auto none = AnanDiscovery::broadcastTargets({});
        check(none.size() == 1 && none.first() == global,
              "no interfaces at all still sweeps 255.255.255.255");
    }

    //DH1KLM The ordinary multi-homed case this exists for: two subnets, the radio on
    //DH1KLM whichever one is not the default route.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
            ipv4Entry("10.0.5.2", "255.255.255.0", "10.0.5.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 3, "two subnets -> the global broadcast plus one each");
        check(targets.first() == global, "the global broadcast stays first");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "the first subnet's directed broadcast is swept");
        check(targets.contains(QHostAddress(QLatin1String("10.0.5.255"))),
              "and so is the second's -- the whole point of the change");
    }

    //DH1KLM Qt synthesizes ip | ~netmask even for /32 point-to-point links.
    //DH1KLM That is this host's own address, not a broadcast destination.
    {
        QNetworkAddressEntry tunnel;
        tunnel.setIp(QHostAddress(QLatin1String("10.8.0.6")));
        tunnel.setNetmask(QHostAddress(QLatin1String("255.255.255.255")));
        tunnel.setBroadcast(QHostAddress(QLatin1String("10.8.0.6")));
        QList<QNetworkAddressEntry> entries{
            tunnel,
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2,
              "a /32 tunnel contributes no target even with Qt's synthesized broadcast");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "and does not stop the interface beside it being swept");
        for (const QHostAddress& t : targets)
            check(!t.isNull(), "no null address is ever returned as a target");
    }

    //DH1KLM /31 links have no broadcast domain either; a missing broadcast is also
    //DH1KLM rejected independently of the prefix length.
    {
        const QNetworkAddressEntry pointToPoint =
            ipv4Entry("10.8.0.6", "255.255.255.254", "10.8.0.7");
        check(AnanDiscovery::broadcastTargets({pointToPoint}) == QList<QHostAddress>{global},
              "a /31 link contributes no broadcast target");
        QNetworkAddressEntry noBroadcast;
        noBroadcast.setIp(QHostAddress(QLatin1String("10.8.0.6")));
        noBroadcast.setPrefixLength(24);
        check(AnanDiscovery::broadcastTargets({noBroadcast}) == QList<QHostAddress>{global},
              "an absent broadcast contributes no target");
    }

    //DH1KLM Loopback. Its subnet broadcast reaches this host only; the radio is not
    //DH1KLM there, and sweeping it would be noise on every machine.
    {
        QNetworkAddressEntry lo = ipv4Entry("127.0.0.1", "255.0.0.0", "127.255.255.255");
        const auto targets = AnanDiscovery::broadcastTargets({lo});
        check(targets.size() == 1 && targets.first() == global,
              "loopback contributes no directed broadcast");
    }

    //DH1KLM IPv6 has no broadcast concept at all, and protocol 2 discovery is IPv4
    //DH1KLM only. An IPv6 entry must be skipped rather than producing something that
    //DH1KLM cannot be sent to.
    {
        QNetworkAddressEntry v6;
        v6.setIp(QHostAddress(QLatin1String("fe80::1")));
        v6.setPrefixLength(64);
        QList<QNetworkAddressEntry> entries{
            v6,
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2, "an IPv6 entry contributes no target");
        check(targets.contains(QHostAddress(QLatin1String("192.168.1.255"))),
              "and the IPv4 entry beside it is still swept");
    }

    //DH1KLM Two addresses on the same subnet produce one destination. With the
    //DH1KLM wildcard socket, this does not guarantee a sweep of both interfaces.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("192.168.1.50", "255.255.255.0", "192.168.1.255"),
            ipv4Entry("192.168.1.77", "255.255.255.0", "192.168.1.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 2, "a repeated broadcast address is sent to once");
    }

    //DH1KLM And the global broadcast appearing as an interface's own broadcast address
    //DH1KLM must not add a second copy of it either.
    {
        QList<QNetworkAddressEntry> entries{
            ipv4Entry("10.1.2.3", "0.0.0.0", "255.255.255.255"),
        };
        const auto targets = AnanDiscovery::broadcastTargets(entries);
        check(targets.size() == 1 && targets.first() == global,
              "an entry whose broadcast IS 255.255.255.255 is not duplicated");
    }

    //DH1KLM LogManager::applyFilterRules() suppresses Debug on every aether.*
    //DH1KLM category by default; a warning has to survive that.
    QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
    const QtMessageHandler previous = qInstallMessageHandler(capture);

    //DH1KLM Control: the capture sees the category, and the filter rule is live.
    //DH1KLM Info is checked too: the silence line is the one Info line.
    const QStringList control = logged([] {
        qCDebug(AetherSDR::lcDiscovery) << "control debug";
        qCInfo(AetherSDR::lcDiscovery) << "control info";
        qCWarning(AetherSDR::lcDiscovery) << "control warning";
    });
    check(control.size() == 2 && control.at(0).startsWith(QStringLiteral("I|"))
              && control.at(1).startsWith(QStringLiteral("W|")),
          "control: aether.discovery info and warning are captured, its debug is filtered");

    bindFailureIsLoggedOncePerCondition();
    noSubnetIsLoggedOncePerCondition();
    sendFailureIsLoggedOncePerCondition();
    silenceIsLoggedOnceAfterThreshold();
    sweepWithNoProbeSentIsNotSilence();
    resetForgetsEverything();
    protocolFailuresAreIndependent();
    bothProtocolsCountOneTargetAndAnyAcceptedReply();
    sweepWhileStoppedSendsNothing();

    qInstallMessageHandler(previous);

    if (g_failures == 0)
        std::fprintf(stderr, "anan_discovery_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
