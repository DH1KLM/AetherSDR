// ANAN-G2 — owned settings object ("Anan" root key, Principle V).
//
// No credential concern here (unlike icom_settings_test.cpp) -- this backend
// has nothing secret to leak. The load-bearing assertions are the round-trip
// and "never a bogus default" guards: an out-of-range/hand-edited rate must
// not silently commit to something AnanBackend would then have to snap anyway.
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/backends/anan/AnanSettings.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    // BEFORE QCoreApplication and before the first AppSettings touch — the
    // profile redirects the settings home, and Qt caches those paths.
    TestSettingsProfile profile(QStringLiteral("anan-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    //DH1KLM: Logical hardware selection is exact-radio scoped, not inherited.
    const QString rp = QStringLiteral("88:A2:9E:6D:9F:6A");
    const QString other = QStringLiteral("88:A2:9E:6D:9F:6B");
    check(!AnanSettings::redPitayaP1ForRadio(rp), "absent model override is standard");
    check(!AnanSettings::setRedPitayaP1ForRadio({}, true), "unknown identity cannot write family default");
    check(AnanSettings::setRedPitayaP1ForRadio(rp, true), "explicit Red Pitaya profile persists");
    check(AnanSettings::redPitayaP1ForRadio(rp) && !AnanSettings::redPitayaP1ForRadio(other),
          "Red Pitaya profile cannot affect a second radio");
    auto& settings = AppSettings::instance();
    check(settings.setRadioFeature(QStringLiteral("anan"), {}, QStringLiteral("HardwareProfile"), 1,
                                   QJsonObject{{QStringLiteral("redPitayaP1"), true}}), "test family default written");
    check(!AnanSettings::redPitayaP1ForRadio(other), "family-wide model override is never inherited");
    check(settings.setRadioFeature(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"), 1,
        QJsonObject{{QStringLiteral("redPitayaP1"), true}, {QStringLiteral("otherField"), 42}}), "test extra profile field");
    check(AnanSettings::setRedPitayaP1ForRadio(rp, false), "standard profile can be restored explicitly");
    check(settings.radioFeatureExact(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"))
        .value(QStringLiteral("otherField")).toInt() == 42, "profile setter preserves other fields");
    check(settings.setRadioFeature(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"), 2,
                                   QJsonObject{{QStringLiteral("redPitayaP1"), true}}), "test future schema");
    check(!AnanSettings::setRedPitayaP1ForRadio(rp, false), "newer profile schema is not downgraded");
    check(!AnanSettings::redPitayaP1ForRadio(rp), "unknown future schema cannot enable hardware override");

    // ---- defaults on an empty document -------------------------------------
    AnanSettings::reset();
    check(AnanSettings::ddc0RateKsps() == 48, "default DDC0 rate is 48 ksps");

    // ---- round trip ----------------------------------------------------------
    AnanSettings::setDdc0RateKsps(384);
    check(AnanSettings::ddc0RateKsps() == 384, "rate round-trips");

    // A hand-edited or truncated file must not command a nonsense rate —
    // AnanBackend::nearestDdc0RateKsps() is the actual snap-to-valid-rate
    // authority, so this class only needs to keep a non-positive value from
    // reaching it as a literal.
    AppSettings::instance().setValue(QStringLiteral("Anan"),
                                     QStringLiteral(R"({"ddc0RateKsps":0})"));
    check(AnanSettings::ddc0RateKsps() == 48, "a zero rate falls back to the default");

    // ---- ADC options -----------------------------------------------------
    AnanSettings::reset();
    check(AnanSettings::ditherEnabled() && AnanSettings::randomEnabled(),
          "dither/random default on");
    check(AnanSettings::ddc0AdcIndex() == 0, "default ADC selection is ADC0");
    check(AnanSettings::rxAntenna() == 1, "default RX antenna is ANT1");
    check(!AnanSettings::bypassAdc0Filters() && AnanSettings::bypassAdc1Filters(),
          "ADC0 Saturn BPF1 auto-selects by frequency; ADC1 bypass defaults on");

    AnanSettings::setDitherEnabled(false);
    AnanSettings::setRandomEnabled(false);
    AnanSettings::setDdc0AdcIndex(1);
    AnanSettings::setBypassAdc0Filters(false);
    AnanSettings::setBypassAdc1Filters(false);
    check(!AnanSettings::ditherEnabled() && !AnanSettings::randomEnabled(),
          "dither/random round-trip off");
    check(AnanSettings::ddc0AdcIndex() == 1, "ADC1/RX2 selection round-trips");
    AnanSettings::setRxAntenna(3);
    check(AnanSettings::rxAntenna() == 3, "RX antenna ANT3 selection round-trips");
    AnanSettings::setRxAntenna(9);
    check(AnanSettings::rxAntenna() == 3, "invalid RX antenna does not overwrite saved selection");
    check(!AnanSettings::bypassAdc0Filters() && !AnanSettings::bypassAdc1Filters(),
          "both bypass options round-trip off");

    // A hand-edited value outside {0,1} must not reach P2Client as a
    // literal ADC index it was never validated against.
    AppSettings::instance().setValue(QStringLiteral("Anan"),
                                     QStringLiteral(R"({"ddc0AdcIndex":7})"));
    check(AnanSettings::ddc0AdcIndex() == 0, "an out-of-range ADC index falls back to ADC0");

    if (g_failures == 0)
        std::printf("anan_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
