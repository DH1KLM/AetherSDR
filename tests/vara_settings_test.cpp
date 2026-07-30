// Tests for VaraSettings — the VARA HF modem's owned configuration.
//
// Two things are worth testing beyond simple round-tripping. First, Principle V
// requires the whole feature's configuration to live in ONE nested object under
// ONE root key, so the test asserts that directly rather than trusting the
// implementation to have done it. Second, several defaults exist for operational
// reasons that are easy to regress: enforceFilterWidth defaults ON because a
// bandwidth/filter mismatch does not error, it merely stops any link from ever
// forming; and an unrecognised bandwidth falls back to 2300 rather than being
// stored, for the same reason.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/VaraSettings.h"

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
    expect(!VaraSettings::enabled(), "disabled by default");
    expect(VaraSettings::host() == QStringLiteral("127.0.0.1"),
           "host defaults to loopback");
    expect(VaraSettings::commandPort() == VaraSettings::kDefaultCommandPort,
           "command port defaults to 8300");
    expect(VaraSettings::dataPort() == VaraSettings::kDefaultCommandPort + 1,
           "data port is the command port plus one, not a separate setting");
    expect(VaraSettings::bandwidth() == Vara::Bandwidth::Bw2300,
           "bandwidth defaults to 2300 Hz");
    expect(VaraSettings::compression() == Vara::Compression::Off,
           "compression defaults to off");
    expect(VaraSettings::enforceFilterWidth(),
           "filter-width enforcement defaults ON (a mismatch silently kills the link)");
    expect(!VaraSettings::launchModem(), "external-modem launching defaults off");
    expect(VaraSettings::callsign().isEmpty(), "callsign is empty until set");
}

void testRoundTrip()
{
    VaraSettings::setEnabled(true);
    VaraSettings::setHost(QStringLiteral("192.168.1.50"));
    VaraSettings::setCommandPort(8302);
    VaraSettings::setCallsign(QStringLiteral(" kk7gwy-9 "));
    VaraSettings::setBandwidth(Vara::Bandwidth::Bw500);
    VaraSettings::setCompression(Vara::Compression::Text);
    VaraSettings::setEnforceFilterWidth(false);
    VaraSettings::setLaunchModem(true);
    VaraSettings::setModemPath(QStringLiteral("/opt/vara/VARA.exe"));

    expect(VaraSettings::enabled(), "enabled round-trips");
    expect(VaraSettings::host() == QStringLiteral("192.168.1.50"),
           "host round-trips");
    expect(VaraSettings::commandPort() == 8302, "command port round-trips");
    expect(VaraSettings::dataPort() == 8303,
           "second-instance data port follows the command port");
    expect(VaraSettings::callsign() == QStringLiteral("KK7GWY-9"),
           "callsign is trimmed and upper-cased on the way in");
    expect(VaraSettings::bandwidth() == Vara::Bandwidth::Bw500,
           "bandwidth round-trips");
    expect(VaraSettings::bandwidthHz() == 500,
           "bandwidthHz reports the numeric width the modem expects");
    expect(VaraSettings::compression() == Vara::Compression::Text,
           "compression round-trips");
    expect(!VaraSettings::enforceFilterWidth(),
           "filter enforcement can be turned off");
    expect(VaraSettings::launchModem(), "launchModem round-trips");
    expect(VaraSettings::modemPath() == QStringLiteral("/opt/vara/VARA.exe"),
           "modem path round-trips");
}

void testValidation()
{
    VaraSettings::setCommandPort(80);
    expect(VaraSettings::commandPort() == VaraSettings::kDefaultCommandPort,
           "a privileged port is rejected: binding would fail like an absent modem");
    VaraSettings::setCommandPort(70000);
    expect(VaraSettings::commandPort() == VaraSettings::kDefaultCommandPort,
           "a port above 65535 is rejected");
    VaraSettings::setCommandPort(8300);

    VaraSettings::setHost(QStringLiteral("   "));
    expect(VaraSettings::host() == QStringLiteral("127.0.0.1"),
           "a blank host stores the default explicitly rather than an empty string");

    VaraSettings::setBandwidth(Vara::Bandwidth::Unknown);
    expect(VaraSettings::bandwidth() == Vara::Bandwidth::Bw2300,
           "an unknown bandwidth falls back to 2300 rather than being stored");
}

void testCorruptedValueDoesNotPropagate()
{
    // A hand-edited or truncated settings file must not produce a bandwidth the
    // modem will reject, because the failure mode is a link that never forms
    // with no error anywhere.
    auto& s = AppSettings::instance();
    QJsonObject o;
    o[QStringLiteral("bandwidthHz")] = QStringLiteral("1234");
    o[QStringLiteral("commandPort")] = QStringLiteral("not-a-number");
    s.setValue(QStringLiteral("Vara"),
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
    expect(VaraSettings::bandwidth() == Vara::Bandwidth::Bw2300,
           "a bandwidth the modem does not accept falls back to 2300");
    expect(VaraSettings::commandPort() == VaraSettings::kDefaultCommandPort,
           "a non-numeric port falls back to the default");
}

void testPrincipleVSingleRootKey()
{
    // Principle V: one nested object under one root key. If a later change adds
    // a flat "VaraSomething" key this fails, which is the point.
    auto& s = AppSettings::instance();
    VaraSettings::setEnabled(true);
    VaraSettings::setCallsign(QStringLiteral("KK7GWY"));
    VaraSettings::setBandwidth(Vara::Bandwidth::Bw2750);

    const QString blob = s.value(QStringLiteral("Vara"), QString{}).toString();
    expect(!blob.isEmpty(), "the configuration lives under the single root key");
    const QJsonObject o = QJsonDocument::fromJson(blob.toUtf8()).object();
    expect(o.contains(QStringLiteral("enabled"))
               && o.contains(QStringLiteral("callsign"))
               && o.contains(QStringLiteral("bandwidthHz")),
           "every field is nested inside that one object");

    for (const char* stray : {"VaraEnabled", "VaraHost", "VaraPort",
                              "VaraCallsign", "VaraBandwidth"}) {
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
    const QString blob = s.value(QStringLiteral("Vara"), QString{}).toString();
    const QJsonObject o = QJsonDocument::fromJson(blob.toUtf8()).object();
    bool any = false;
    for (const QString& k : o.keys()) {
        if (k.contains(QStringLiteral("encrypt"), Qt::CaseInsensitive)) any = true;
    }
    expect(!any, "no encryption setting is stored (47 CFR 97.113(a)(4))");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vara-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    testDefaults();
    testRoundTrip();
    testValidation();
    testCorruptedValueDoesNotPropagate();
    testPrincipleVSingleRootKey();
    testEncryptionIsNotConfigurable();

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
