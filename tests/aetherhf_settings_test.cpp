// Tests for AetherHfSettings — AetherHF's owned configuration.
//
// Beyond round-tripping, three properties are worth pinning down. Principle V
// requires the whole feature's configuration to live in ONE nested object under
// ONE root key, so the test asserts that directly rather than trusting the
// implementation. Engine-specific settings must live in their own sub-object,
// so adding an engine cannot collide with an existing one. And several defaults
// exist for operational reasons that are easy to regress: enforceFilterWidth
// defaults ON because a bandwidth/filter mismatch does not error, it merely
// stops any link from ever forming, and an unrecognised bandwidth falls back to
// 2300 rather than being stored, for the same reason.

#include "TestSettingsProfile.h"
#include "core/AetherHfSettings.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

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

void testDefaults()
{
    expect(!AetherHfSettings::enabled(), "disabled by default");
    expect(AetherHfSettings::engine() == HfEngine::Vara,
           "VARA is the default engine");
    expect(AetherHfSettings::varaHost() == QStringLiteral("127.0.0.1"),
           "VARA host defaults to loopback");
    expect(AetherHfSettings::varaCommandPort()
               == AetherHfSettings::kDefaultVaraCommandPort,
           "VARA command port defaults to 8300");
    expect(AetherHfSettings::varaDataPort()
               == AetherHfSettings::kDefaultVaraCommandPort + 1,
           "data port is the command port plus one, not a separate setting");
    expect(AetherHfSettings::varaBandwidth() == Vara::Bandwidth::Bw2300,
           "bandwidth defaults to 2300 Hz");
    expect(AetherHfSettings::varaCompression() == Vara::Compression::Off,
           "compression defaults to off");
    expect(AetherHfSettings::enforceFilterWidth(),
           "filter-width enforcement defaults ON (a mismatch silently kills the link)");
    expect(AetherHfSettings::callsign().isEmpty(), "callsign is empty until set");
    // Out of the box Mercury does not launch the binary itself, so a host is
    // all it needs and the default host is loopback. Availability is about
    // whether the engine CAN be started, not whether anything is listening.
    expect(AetherHfSettings::mercuryAvailable(),
           "Mercury is available by default: it only needs a host to reach");
}

void testEngineSelection()
{
    AetherHfSettings::setEngine(HfEngine::Mercury);
    expect(AetherHfSettings::engine() == HfEngine::Mercury,
           "the engine choice round-trips");
    expect(hfEngineToString(HfEngine::Mercury) == QStringLiteral("mercury")
               && hfEngineFromString(QStringLiteral("mercury")) == HfEngine::Mercury,
           "engine names map both ways");
    expect(hfEngineFromString(QStringLiteral("nonsense")) == HfEngine::Vara,
           "an unknown engine name falls back to VARA rather than a bad state");
    // Mercury negotiates the same three bandwidth tokens VARA does, so the
    // filter comparison has a real number to work with for either engine.
    expect(AetherHfSettings::activeBandwidthHz()
               == AetherHfSettings::mercuryBandwidthHz(),
           "the active bandwidth follows the selected engine, Mercury included");
    expect(AetherHfSettings::activeBandwidthHz() != 0,
           "Mercury reports a real bandwidth now that it is driven");
    AetherHfSettings::setEngine(HfEngine::Vara);
    expect(AetherHfSettings::activeBandwidthHz() == 2300,
           "with VARA selected the active bandwidth is VARA's");
}

void testRoundTrip()
{
    AetherHfSettings::setEnabled(true);
    AetherHfSettings::setCallsign(QStringLiteral(" kk7gwy-9 "));
    AetherHfSettings::setVaraHost(QStringLiteral("192.168.1.50"));
    AetherHfSettings::setVaraCommandPort(8302);
    AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Bw500);
    AetherHfSettings::setVaraCompression(Vara::Compression::Text);
    AetherHfSettings::setEnforceFilterWidth(false);
    AetherHfSettings::setVaraLaunchModem(true);
    AetherHfSettings::setVaraModemPath(QStringLiteral("/opt/vara/VARA.exe"));
    AetherHfSettings::setMercuryConfigPath(QStringLiteral("/etc/mercury.conf"));

    expect(AetherHfSettings::enabled(), "enabled round-trips");
    expect(AetherHfSettings::callsign() == QStringLiteral("KK7GWY-9"),
           "callsign is trimmed and upper-cased on the way in");
    expect(AetherHfSettings::varaHost() == QStringLiteral("192.168.1.50"),
           "VARA host round-trips");
    expect(AetherHfSettings::varaCommandPort() == 8302, "command port round-trips");
    expect(AetherHfSettings::varaDataPort() == 8303,
           "second-instance data port follows the command port");
    expect(AetherHfSettings::varaBandwidth() == Vara::Bandwidth::Bw500,
           "bandwidth round-trips");
    expect(AetherHfSettings::varaBandwidthHz() == 500,
           "bandwidthHz reports the numeric width the modem expects");
    expect(AetherHfSettings::varaCompression() == Vara::Compression::Text,
           "compression round-trips");
    expect(!AetherHfSettings::enforceFilterWidth(),
           "filter enforcement can be turned off");
    expect(AetherHfSettings::varaLaunchModem(), "launchModem round-trips");
    expect(AetherHfSettings::varaModemPath() == QStringLiteral("/opt/vara/VARA.exe"),
           "modem path round-trips");
    expect(AetherHfSettings::mercuryConfigPath() == QStringLiteral("/etc/mercury.conf"),
           "Mercury settings persist even though the engine is not integrated");
}

void testEnginesDoNotCollide()
{
    // The point of per-engine sections: writing one must not disturb the other.
    AetherHfSettings::setVaraHost(QStringLiteral("10.0.0.9"));
    AetherHfSettings::setMercuryConfigPath(QStringLiteral("/tmp/m.conf"));
    expect(AetherHfSettings::varaHost() == QStringLiteral("10.0.0.9"),
           "writing a Mercury setting leaves VARA's untouched");
    AetherHfSettings::setVaraCommandPort(8310);
    expect(AetherHfSettings::mercuryConfigPath() == QStringLiteral("/tmp/m.conf"),
           "writing a VARA setting leaves Mercury's untouched");

    auto& s = AppSettings::instance();
    const QJsonObject root = QJsonDocument::fromJson(
        s.value(QStringLiteral("AetherHf"), QString{}).toString().toUtf8()).object();
    expect(root.value(QStringLiteral("vara")).isObject()
               && root.value(QStringLiteral("mercury")).isObject(),
           "each engine has its own nested section");
    expect(!root.contains(QStringLiteral("host")),
           "engine-specific keys do not leak into the common level");
}

void testValidation()
{
    AetherHfSettings::setVaraCommandPort(80);
    expect(AetherHfSettings::varaCommandPort()
               == AetherHfSettings::kDefaultVaraCommandPort,
           "a privileged port is rejected: binding would fail like an absent modem");
    AetherHfSettings::setVaraCommandPort(70000);
    expect(AetherHfSettings::varaCommandPort()
               == AetherHfSettings::kDefaultVaraCommandPort,
           "a port above 65535 is rejected");
    AetherHfSettings::setVaraCommandPort(8300);

    AetherHfSettings::setVaraHost(QStringLiteral("   "));
    expect(AetherHfSettings::varaHost() == QStringLiteral("127.0.0.1"),
           "a blank host stores the default explicitly rather than an empty string");

    AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Unknown);
    expect(AetherHfSettings::varaBandwidth() == Vara::Bandwidth::Bw2300,
           "an unknown bandwidth falls back to 2300 rather than being stored");
}

void testCorruptedValueDoesNotPropagate()
{
    // A hand-edited or truncated settings file must not produce a bandwidth the
    // modem will reject, because the failure mode is a link that never forms
    // with no error anywhere.
    auto& s = AppSettings::instance();
    QJsonObject vara;
    vara[QStringLiteral("bandwidthHz")] = QStringLiteral("1234");
    vara[QStringLiteral("commandPort")] = QStringLiteral("not-a-number");
    QJsonObject root;
    root[QStringLiteral("vara")] = vara;
    s.setValue(QStringLiteral("AetherHf"),
               QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    s.save();
    expect(AetherHfSettings::varaBandwidth() == Vara::Bandwidth::Bw2300,
           "a bandwidth the modem does not accept falls back to 2300");
    expect(AetherHfSettings::varaCommandPort()
               == AetherHfSettings::kDefaultVaraCommandPort,
           "a non-numeric port falls back to the default");
}

void testPrincipleVSingleRootKey()
{
    // Principle V: one nested object under one root key. If a later change adds
    // a flat "AetherHfSomething" key this fails, which is the point.
    auto& s = AppSettings::instance();
    // Set every common field explicitly: the previous test deliberately
    // overwrites the root object, and an unset field reads back as its default
    // rather than being stored, which is correct but means it would not appear
    // here.
    AetherHfSettings::setEnabled(true);
    AetherHfSettings::setEngine(HfEngine::Vara);
    AetherHfSettings::setCallsign(QStringLiteral("KK7GWY"));
    AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Bw2750);

    const QString blob = s.value(QStringLiteral("AetherHf"), QString{}).toString();
    expect(!blob.isEmpty(), "the configuration lives under the single root key");
    const QJsonObject o = QJsonDocument::fromJson(blob.toUtf8()).object();
    expect(o.contains(QStringLiteral("enabled"))
               && o.contains(QStringLiteral("callsign"))
               && o.contains(QStringLiteral("engine")),
           "common fields are nested inside that one object");

    for (const char* stray : {"AetherHfEnabled", "AetherHfEngine", "AetherHfCallsign",
                              "Vara", "VaraEnabled", "VaraHost"}) {
        expect(!s.contains(QLatin1String(stray)),
               "no flat sibling key is written alongside the nested object");
    }
}

void testEncryptionIsNotConfigurable()
{
    // Deliberate absence, asserted so nobody adds it casually: VARA can
    // negotiate an encrypted link, but §97.113(a)(4) prohibits obscuring the
    // meaning of amateur transmissions, so AetherSDR never asks for it.
    auto& s = AppSettings::instance();
    const QString blob = s.value(QStringLiteral("AetherHf"), QString{}).toString();
    bool any = blob.contains(QStringLiteral("encrypt"), Qt::CaseInsensitive);
    expect(!any, "no encryption setting is stored (47 CFR 97.113(a)(4))");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aetherhf-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    testDefaults();
    testEngineSelection();
    testRoundTrip();
    testEnginesDoNotCollide();
    testValidation();
    testCorruptedValueDoesNotPropagate();
    testPrincipleVSingleRootKey();
    testEncryptionIsNotConfigurable();

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
