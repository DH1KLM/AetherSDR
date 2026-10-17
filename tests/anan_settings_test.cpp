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
#include "core/RadioDiscovery.h"

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
    check(!AnanSettings::radioProfile(rp).valid, "malformed schema-2 model document is refused");
    check(AnanSettings::setRadioProfile(rp, AnanModel::AnvelinaPro3, 2), "explicit Anvelina P2 choice stored");
    const auto stored = AnanSettings::radioProfile(rp);
    check(stored.valid && stored.model == AnanModel::AnvelinaPro3 && stored.protocol == 2,
          "logical model and protocol round-trip independently");
    check(AnanSettings::radioProfile(other).model == AnanModel::Automatic,
          "model choice never leaks into a second MAC or inherits family-wide default");
    check(AnanSettings::setRedPitayaP1ForRadio(other, true), "legacy RP profile fixture");
    const auto legacy = AnanSettings::radioProfile(other);
    check(legacy.valid && legacy.model == AnanModel::RedPitaya && legacy.protocol == 1,
          "old tested Red Pitaya P1 setting retains model and protocol");
    check(!legacy.overrideDetection, "schema-1 RP preference never enables free board override");
    check(settings.setRadioFeature(QStringLiteral("anan"), other, QStringLiteral("HardwareProfile"), 2,
        QJsonObject{{QStringLiteral("model"), int(AnanModel::Anan7000)}, {QStringLiteral("protocol"), 2},
                    {QStringLiteral("overrideDetection"), true}}), "schema-2 fixture with unknown sibling field");
    check(AnanSettings::radioProfile(other).valid && !AnanSettings::radioProfile(other).overrideDetection,
          "schema-2 defaults override off even if an unowned field is present");
    check(AnanSettings::setRadioProfile(rp, AnanModel::Anan200D, 2, true), "explicit board override stored");
    const auto overridden = AnanSettings::radioProfile(rp);
    check(overridden.valid && overridden.overrideDetection && overridden.model == AnanModel::Anan200D,
          "override and selected model round-trip together");
    check(!AnanSettings::radioProfile(other).overrideDetection, "override never leaks to second MAC");
    check(!AnanSettings::setRadioProfile(rp, AnanModel::Automatic, 2, true), "automatic cannot enable override");
    check(!AnanSettings::setRadioProfile(rp, AnanModel::HermesLite2, 2, true), "HL2 cannot be a board override");
    check(AnanSettings::setRadioProfile(rp, AnanModel::Anan200D, 2, false)
          && !AnanSettings::radioProfile(rp).overrideDetection, "override can be explicitly disabled");
    check(settings.setRadioFeature(QStringLiteral("anan"), other, QStringLiteral("HardwareProfile"), 3,
        QJsonObject{{QStringLiteral("model"), int(AnanModel::Anan200D)}, {QStringLiteral("protocol"), 2},
                    {QStringLiteral("overrideDetection"), QStringLiteral("true")}}), "malformed override fixture");
    check(!AnanSettings::radioProfile(other).valid, "non-boolean override field fails closed");

    //DH1KLM: Two radios sharing a selected profile must retain different
    // reported identities. Forget removes only the selected MAC's mapping.
    RadioInfo diy;
    diy.family = QStringLiteral("anan"); diy.serial = rp; diy.boardId = 5;
    diy.hpsdrProtocol = 2; diy.address = QHostAddress(QStringLiteral("192.0.2.10"));
    diy.nickname = QStringLiteral("DIY Red Pitaya");
    RadioInfo genuine = diy; genuine.serial = other; genuine.boardId = 4;
    genuine.address = QHostAddress(QStringLiteral("192.0.2.11"));
    check(AnanSettings::setRadioProfile(rp, AnanModel::Anan200D, 2, true)
          && AnanSettings::setRadioProfile(other, AnanModel::Anan200D, 2, false), "same-profile device fixtures");
    auto mappedDoc = settings.radioFeatureExact(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"));
    mappedDoc.insert(QStringLiteral("otherField"), 42);
    check(settings.setRadioFeature(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"), 3, mappedDoc),
          "unrelated sibling field fixture");
    check(settings.setRadioFeature(QStringLiteral("anan"), rp, QStringLiteral("Identity"), 1,
        QJsonObject{{QStringLiteral("nickname"), QStringLiteral("DIY Red Pitaya")}}), "nickname fixture");
    AnanSettings::applyRadioProfileDisplay(diy);
    AnanSettings::applyRadioProfileDisplay(genuine);
    check(diy.displayModel() == QStringLiteral("ANAN-200D")
          && diy.model.contains(QStringLiteral("Orion MkII")),
          "override presents ANAN-200D to operating UI while retaining discovery identity");
    check(genuine.effectiveModel.isEmpty() && genuine.displayModel() == genuine.model,
          "override OFF retains normal model presentation");
    RadioInfo fallbackName = diy;
    fallbackName.nickname = fallbackName.model;
    check(fallbackName.displayNickname() == QStringLiteral("ANAN-200D"),
          "discovery nickname fallback cannot conceal effective operating model");
    fallbackName.nickname.clear();
    check(fallbackName.displayNickname() == QStringLiteral("ANAN-200D"), "empty nickname uses effective model");
    check(diy.displayNickname() == QStringLiteral("DIY Red Pitaya"), "custom station nickname remains independent");
    for (const auto* family : {"flex", "icom", "hl2"}) {
        RadioInfo untouched;
        untouched.family = QString::fromLatin1(family); untouched.serial = rp;
        untouched.model = QStringLiteral("Original model"); untouched.nickname = QStringLiteral("Original station");
        AnanSettings::applyRadioProfileDisplay(untouched);
        check(untouched.effectiveModel.isEmpty() && untouched.connectionProfile.isEmpty()
              && untouched.displayModel() == untouched.model && untouched.displayNickname() == untouched.nickname,
              "Flex/Icom/HL2 presentation unaffected by an ANAN mapping for the same serial");
        untouched.nickname.clear();
        check(untouched.displayNickname().isEmpty(), "other families retain their empty nickname behavior");
    }
    check(diy.model.contains(QStringLiteral("Orion MkII"))
          && genuine.model.contains(QStringLiteral("ANAN-200D")) && diy.model != genuine.model,
          "selected ANAN-200D profile never replaces reported board identity");
    check(diy.connectionProfile.contains(QStringLiteral("ANAN-200D"))
          && diy.connectionProfile.contains(QStringLiteral("Override ON"))
          && genuine.connectionProfile.contains(QStringLiteral("Override OFF")),
          "profile and override status remain explicit and distinct");
    check(diy.nickname == QStringLiteral("DIY Red Pitaya"), "presentation retains independent nickname");
    const auto snapshot = diy.connectionProfile;
    check(AnanSettings::hasRadioProfileMapping(diy) && AnanSettings::hasRadioProfileMapping(genuine),
          "explicit model choices are forgettable even without override");
    check(AnanSettings::forgetRadioProfileMapping(diy), "forget selected mapping");
    const auto forgotten = AnanSettings::radioProfile(rp);
    check(forgotten.valid && forgotten.model == AnanModel::Automatic && !forgotten.overrideDetection
          && forgotten.protocol == 2, "forget restores automatic model and retains protocol");
    check(!AnanSettings::hasRadioProfileMapping(diy) && AnanSettings::hasRadioProfileMapping(genuine),
          "forget affects only selected radio and disables repeat forget");
    check(settings.radioFeatureExact(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"))
              .value(QStringLiteral("otherField")).toInt() == 42
          && settings.radioFeatureExact(QStringLiteral("anan"), rp, QStringLiteral("Identity"))
              .value(QStringLiteral("nickname")).toString() == QStringLiteral("DIY Red Pitaya"),
          "forget preserves sibling fields and separate features");
    check(diy.connectionProfile == snapshot, "connect-time presentation snapshot remains immutable");
    AnanSettings::applyRadioProfileDisplay(diy);
    check(diy.connectionProfile.contains(QStringLiteral("Automatic"))
          && diy.connectionProfile.contains(QStringLiteral("Override OFF"))
          && diy.model.contains(QStringLiteral("Orion MkII")) && diy.effectiveModel.isEmpty()
          && diy.displayModel() == diy.model, "rediscovery renders physical identity after forget");
    RadioInfo foreign = diy; foreign.family = QStringLiteral("flex");
    check(!AnanSettings::hasRadioProfileMapping(foreign) && !AnanSettings::forgetRadioProfileMapping(foreign),
          "forget refuses another radio family");
    foreign = diy; foreign.serial.clear();
    check(!AnanSettings::forgetRadioProfileMapping(foreign), "forget never writes a family-wide default");
    check(AnanSettings::setRedPitayaP1ForRadio(QStringLiteral("legacy-forget"), true), "legacy forget fixture");
    foreign = diy; foreign.serial = QStringLiteral("legacy-forget");
    check(AnanSettings::forgetRadioProfileMapping(foreign)
          && AnanSettings::radioProfile(foreign.serial).protocol == 1
          && !AnanSettings::redPitayaP1ForRadio(foreign.serial), "legacy forget preserves P1 preference only");
    check(!AnanSettings::setRadioProfile({}, AnanModel::Anan8000, 2), "model cannot write unknown identity");
    check(!AnanSettings::setRadioProfile(rp, static_cast<AnanModel>(99), 2), "invalid logical model rejected");
    check(!AnanSettings::setRadioProfile(rp, AnanModel::Anan8000, 3), "invalid wire preference rejected");
    check(settings.setRadioFeature(QStringLiteral("anan"), rp, QStringLiteral("HardwareProfile"), 4,
                                   QJsonObject{{QStringLiteral("model"), 10}}), "future model schema fixture");
    check(!AnanSettings::setRadioProfile(rp, AnanModel::Anan8000, 2), "future model schema not downgraded");
    check(!AnanSettings::radioProfile(rp).valid, "future model schema cannot silently choose a board profile");
    check(!AnanSettings::forgetRadioProfileMapping(diy), "forget refuses newer schema without downgrading it");

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
