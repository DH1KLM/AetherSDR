// Mercury engine: the command line it is launched with, and the settings that
// decide whether it can be started at all.
//
// Both are places where a mistake is SILENT. Wrong arguments give a modem that
// starts, opens the wrong sound device and never hears the radio; a wrong
// availability answer gives a Connect button that does nothing, or one that is
// greyed out for an operator who has Mercury running perfectly well elsewhere.

#include "core/AetherHfSettings.h"
#include "core/MercuryProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what);
    if (!condition)
        ++g_failures;
}

int indexOfArg(const QStringList& args, const char* flag)
{
    return args.indexOf(QString::fromLatin1(flag));
}

bool hasPair(const QStringList& args, const char* flag, const QString& value)
{
    const int i = indexOfArg(args, flag);
    return i >= 0 && i + 1 < args.size() && args.at(i + 1) == value;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AetherSDRTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MercuryEngineTest"));

    // ── The command line ────────────────────────────────────────────────
    {
        MercuryProcess::Options o;
        o.basePort = 8400;
        o.modeIndex = 3;
        o.soundSystem = QStringLiteral("pulse");
        o.captureDevice = QStringLiteral("plughw:1,0");
        o.playbackDevice = QStringLiteral("plughw:1,1");
        const QStringList a = MercuryProcess::buildArguments(o);

        expect(hasPair(a, "-p", QStringLiteral("8400")),
               "the ARQ base port is passed with -p");
        expect(hasPair(a, "-m", QStringLiteral("3")),
               "the payload mode index is passed with -m");
        expect(hasPair(a, "-x", QStringLiteral("pulse")),
               "the sound system is passed with -x");
        expect(hasPair(a, "-i", QStringLiteral("plughw:1,0")),
               "the capture device is passed with -i");
        expect(hasPair(a, "-o", QStringLiteral("plughw:1,1")),
               "the playback device is passed with -o");
        // Only the base port is passed: Mercury derives base+1 for data itself,
        // exactly as VARA does. Passing a data port would be a second source of
        // truth for the same fact.
        expect(!a.contains(QStringLiteral("8401")),
               "the data port is NOT passed — Mercury derives base+1 itself");
    }

    // ── Unset fields are omitted, not passed empty ──────────────────────
    {
        MercuryProcess::Options o;
        o.basePort = 8400;
        o.soundSystem = QStringLiteral("   ");   // whitespace only
        const QStringList a = MercuryProcess::buildArguments(o);
        // An empty -x is not "use the default" to Mercury; it is a sound system
        // named "", which fails. Leaving the flag out is what gets the default.
        expect(indexOfArg(a, "-x") < 0, "a blank sound system omits -x entirely");
        expect(indexOfArg(a, "-i") < 0, "an unset capture device omits -i");
        expect(indexOfArg(a, "-o") < 0, "an unset playback device omits -o");
        expect(indexOfArg(a, "-p") >= 0, "the base port is always passed");
    }

    // ── Refusing to start ───────────────────────────────────────────────
    {
        MercuryProcess p;
        MercuryProcess::Options o;
        expect(!p.start(QString(), o), "an empty binary path is refused");
        expect(!p.start(QStringLiteral("/nonexistent/mercury"), o),
               "a missing binary is refused rather than silently doing nothing");
        expect(!p.isRunning(), "nothing is running after a refused start");
    }

    // ── Availability ────────────────────────────────────────────────────
    {
        // Not launching it ourselves: a reachable host is all that is needed,
        // and there is always a default host, so the engine is usable.
        AetherHfSettings::setMercuryLaunch(false);
        expect(AetherHfSettings::mercuryAvailable(),
               "with launching off, a host alone makes the engine available");

        // Launching it ourselves: without a binary path there is nothing to run.
        AetherHfSettings::setMercuryLaunch(true);
        AetherHfSettings::setMercuryBinaryPath(QString());
        expect(!AetherHfSettings::mercuryAvailable(),
               "with launching on and no binary path, the engine is unavailable");

        AetherHfSettings::setMercuryBinaryPath(QStringLiteral("/opt/mercury/mercury"));
        expect(AetherHfSettings::mercuryAvailable(),
               "with launching on and a binary path, the engine is available");
    }

    // ── Settings round-trip and defaults ────────────────────────────────
    {
        // Mercury's own default base port is 8300, the same as VARA's. Sharing
        // it would collide the moment both run on one machine, which is exactly
        // what comparing the two engines requires.
        expect(AetherHfSettings::kDefaultMercuryCommandPort != 8300,
               "Mercury does not default to VARA's port");
        AetherHfSettings::setMercuryCommandPort(0);      // out of range
        expect(AetherHfSettings::mercuryCommandPort()
                   == AetherHfSettings::kDefaultMercuryCommandPort,
               "an out-of-range port falls back to the default");
        AetherHfSettings::setMercuryCommandPort(8500);
        expect(AetherHfSettings::mercuryCommandPort() == 8500,
               "a valid port round-trips");
        expect(AetherHfSettings::mercuryDataPort() == 8501,
               "the data port is the base port plus one");

        AetherHfSettings::setMercuryHost(QStringLiteral("   "));
        expect(AetherHfSettings::mercuryHost() == QStringLiteral("127.0.0.1"),
               "a blank host falls back to loopback rather than being stored");

        AetherHfSettings::setMercuryBandwidth(Vara::Bandwidth::Bw500);
        expect(AetherHfSettings::mercuryBandwidthHz() == 500,
               "the bandwidth round-trips");

        AetherHfSettings::setMercuryModeIndex(-5);
        expect(AetherHfSettings::mercuryModeIndex() == 1,
               "a negative mode index falls back to Mercury's default of 1");

        expect(!AetherHfSettings::defaultMercurySoundSystem().isEmpty(),
               "every platform has a default sound system");
        AetherHfSettings::setMercurySoundSystem(QString());
        expect(AetherHfSettings::mercurySoundSystem()
                   == AetherHfSettings::defaultMercurySoundSystem(),
               "an unset sound system reports the platform default");
    }

    // ── The active engine answers without the caller branching ──────────
    {
        AetherHfSettings::setMercuryCommandPort(8500);
        AetherHfSettings::setVaraCommandPort(8300);

        AetherHfSettings::setEngine(HfEngine::Vara);
        expect(AetherHfSettings::activeCommandPort() == 8300,
               "with VARA selected the active port is VARA's");
        AetherHfSettings::setEngine(HfEngine::Mercury);
        expect(AetherHfSettings::activeCommandPort() == 8500,
               "with Mercury selected the active port is Mercury's");
        expect(AetherHfSettings::activeDataPort() == 8501,
               "the active data port follows the active command port");

        // A bandwidth of zero used to mean "Mercury cannot report one". It can
        // now, and the filter comparison depends on the difference.
        AetherHfSettings::setMercuryBandwidth(Vara::Bandwidth::Bw2750);
        expect(AetherHfSettings::activeBandwidthHz() == 2750,
               "the active bandwidth is the selected engine's, not zero");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
