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
#include "core/backends/anan/AnanFilterOptions.h"
#include "core/backends/anan/AnanCalibrationOptions.h"
#include "core/RadioDiscovery.h"
#include "core/RadioSettingsScope.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>
#include <limits>
#include "core/backends/anan/AnanTransmitPower.h"

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
    {
        // Calibration belongs to one MAC/model/board/protocol and keeps ADC offsets independent.
        ReceiveCalibration calibration;
        calibration.frequencyFactor = 1.00001;
        calibration.meterOffsetDb[1] = 23.5;
        calibration.displayOffsetDb[1] = 42;
        calibration.levelCalibrated[1] = true;
        check(AnanSettings::setReceiveCalibration(QStringLiteral("CAL-MAC"),7,5,2,calibrationOptions(calibration)),
              "calibration saves atomically");
        const auto saved = parseCalibrationOptions(AnanSettings::receiveCalibration(QStringLiteral("CAL-MAC"),7,5,2));
        check(saved && saved->frequencyFactor == 1.00001 && saved->meterOffsetDb[0] == 0
              && saved->meterOffsetDb[1] == 23.5 && saved->displayOffsetDb[1] == 42
              && saved->levelCalibrated[1] && !saved->levelCalibrated[0], "calibration round trip separates ADCs");
        check(AnanSettings::receiveCalibration(QStringLiteral("OTHER-MAC"),7,5,2).isEmpty()
              && AnanSettings::receiveCalibration(QStringLiteral("CAL-MAC"),7,5,1).isEmpty()
              && AnanSettings::receiveCalibration(QStringLiteral("CAL-MAC"),8,5,2).isEmpty(), "calibration scope does not bleed");
        auto invalid = calibrationOptions(calibration);
        invalid.insert(QStringLiteral("frequencyFactor"), true);
        check(!parseCalibrationOptions(invalid), "boolean oscillator factors refused");
        invalid.insert(QStringLiteral("frequencyFactor"), 1.01);
        check(!AnanSettings::setReceiveCalibration(QStringLiteral("CAL-MAC"),7,5,2,invalid), "out-of-range calibration cannot overwrite");
        const RadioSettingsScope scope(QStringLiteral("anan"), QStringLiteral("CAL-FUTURE"));
        check(scope.setFeature(QStringLiteral("ReceiveCalibration"),99,QJsonObject{{QStringLiteral("future"),true}}), "seed future calibration");
        check(!AnanSettings::setReceiveCalibration(QStringLiteral("CAL-FUTURE"),7,5,2,calibrationOptions(calibration))
              && scope.featureExact(QStringLiteral("ReceiveCalibration")).value(QStringLiteral("future")).toBool(),
              "future calibration cannot be overwritten");
    }

    {
        // Manual user outputs persist per device/profile without borrowing band OC.
        FilterConfig config = defaultFilterConfig(false);
        config.userDigitalOutputs = 9;
        QVariantMap options = filterOptions(config);
        check(AnanSettings::setHardwareFilters(QStringLiteral("DIGITAL-MAC"), 7, 5, options),
              "manual user outputs save");
        const auto saved = parseFilterOptions(AnanSettings::hardwareFilters(QStringLiteral("DIGITAL-MAC"), 7, 5));
        check(saved && saved->userDigitalOutputs == 9 && !saved->ocEnabled,
              "manual user outputs round trip independently of disabled band OC");
        check(AnanSettings::hardwareFilters(QStringLiteral("DIGITAL-MAC-OTHER"), 7, 5).isEmpty()
              && AnanSettings::hardwareFilters(QStringLiteral("DIGITAL-MAC"), 8, 5).isEmpty(),
              "manual user outputs remain scoped to the device and model");
        for (const QVariant value : {QVariant(-1), QVariant(16), QVariant(1.5), QVariant(true), QVariant(QStringLiteral("3"))}) {
            options.insert(QStringLiteral("userDigitalOutputs"), value);
            check(!parseFilterOptions(options), "invalid user nibble is refused");
        }
        options.remove(QStringLiteral("userDigitalOutputs"));
        const auto legacy = parseFilterOptions(options);
        check(legacy && legacy->userDigitalOutputs == 0, "legacy documents default manual outputs off");
    }

    {
        auto config = defaultFilterConfig(false);
        config.antennaBands.emplace();
        config.antennaBands->enabled = true;
        config.antennaBands->rows[5] = {2, 5};
        const auto options = filterOptions(config);
        const QString mac = QStringLiteral("band-antenna-settings");
        check(AnanSettings::setHardwareFilters(mac, 7, 5, options), "save band antenna table atomically");
        const auto loaded = parseFilterOptions(AnanSettings::hardwareFilters(mac, 7, 5));
        check(loaded && loaded->antennaBands && loaded->antennaBands->enabled
              && loaded->antennaBands->rows[5].antenna == 2 && loaded->antennaBands->rows[5].rxOnly == 5,
              "independent ANT bank and Thetis external plus XVTR state survive storage");
        check(AnanSettings::hardwareFilters(QStringLiteral("other-band-antenna-radio"), 7, 5).isEmpty()
              && AnanSettings::hardwareFilters(mac, 8, 5).isEmpty()
              && AnanSettings::hardwareFilters(mac, 7, 4).isEmpty(), "band antennas cannot leak across MAC, model or board");
        auto invalid = options;
        auto table = invalid.value(QStringLiteral("antennaBands")).toMap();
        table.insert(QStringLiteral("enabled"), QStringLiteral("true"));
        invalid.insert(QStringLiteral("antennaBands"), table);
        check(!parseFilterOptions(invalid), "band antenna enable must be an actual boolean");
        for (const QVariant bad : {QVariant(-1), QVariant(6), QVariant(1.5), QVariant(true), QVariant(QStringLiteral("2"))}) {
            auto table = options.value(QStringLiteral("antennaBands")).toMap();
            auto rows = table.value(QStringLiteral("rows")).toList();
            auto row = rows[5].toMap();
            row.insert(QStringLiteral("rxOnly"), bad);
            rows[5] = row;
            table.insert(QStringLiteral("rows"), rows);
            invalid = options;
            invalid.insert(QStringLiteral("antennaBands"), table);
            check(!parseFilterOptions(invalid) && !AnanSettings::setHardwareFilters(mac, 7, 5, invalid),
                  "malformed band input cannot replace the persisted table");
        }
        table = options.value(QStringLiteral("antennaBands")).toMap();
        table.insert(QStringLiteral("rows"), QVariantList{});
        invalid = options;
        invalid.insert(QStringLiteral("antennaBands"), table);
        check(!parseFilterOptions(invalid), "incomplete band table is refused");
        const RadioSettingsScope scope(QStringLiteral("anan"), mac);
        check(scope.setFeature(QStringLiteral("HardwareFilters"), 99, QJsonObject{{QStringLiteral("future"), true}}), "seed future band schema");
        check(!AnanSettings::setHardwareFilters(mac, 7, 5, options)
              && scope.featureExact(QStringLiteral("HardwareFilters")).value(QStringLiteral("future")).toBool(),
              "band edits never overwrite a newer document");
    }

    // Logical hardware selection is exact-radio scoped, not inherited.
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

    // Two radios sharing a selected profile must retain different
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
    RadioInfo staleDiscovery = diy;
    staleDiscovery.effectiveModel.clear();
    staleDiscovery.connectionProfile = QStringLiteral("Profile: Automatic • Override OFF");
    check(staleDiscovery.displayIdentityForSession(&diy).displayModel() == QStringLiteral("ANAN-200D"),
          "active tab uses connected override when discovery cache predates directed connect");
    check(staleDiscovery.displayIdentityForSession(nullptr).displayModel() == staleDiscovery.model,
          "disconnected tabs use discovery metadata");
    check(diy.displayIdentityForSession(&staleDiscovery).effectiveModel.isEmpty(),
          "new override-OFF session wins over previously mapped discovery row");
    check(staleDiscovery.displayIdentityForSession(&genuine).serial == staleDiscovery.serial,
          "active profile never leaks into another radio tab");
    RadioInfo sameSerialOtherFamily = diy; sameSerialOtherFamily.family = QStringLiteral("flex");
    check(staleDiscovery.displayIdentityForSession(&sameSerialOtherFamily).effectiveModel.isEmpty(),
          "same serial in another family cannot override discovery display");
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

    {
        auto config = defaultFilterConfig(true);
        config.receiveAdc1 = config.receive;
        (*config.receiveAdc1)[1].highHz = 6499999;
        (*config.receiveAdc1)[2].lowHz = 6500000;
        auto options = filterOptions(config);
        const QString serial = QStringLiteral("ALEX2-MAC");
        check(AnanSettings::setHardwareFilters(serial, 7, 5, options), "save independent ADC1 limits");
        const auto restored = parseFilterOptions(AnanSettings::hardwareFilters(serial,7,5));
        check(restored && restored->receiveAdc1 && (*restored->receiveAdc1)[2].lowHz == 6500000
              && restored->receive[2].lowHz == 5500000,
              "ADC1 ranges persist without replacing ADC0 limits");
        auto legacy = options;
        legacy.remove(QStringLiteral("receiveAdc1"));
        const RadioSettingsScope scope(QStringLiteral("anan"),serial);
        check(scope.setFeature(QStringLiteral("HardwareFilters"), 3,
              QJsonObject{{QStringLiteral("7:5"),QJsonObject::fromVariantMap(legacy)}}), "seed old schema 3 ranges");
        const auto old = parseFilterOptions(AnanSettings::hardwareFilters(serial,7,5));
        check(old && !old->receiveAdc1 && receiveFilterBits(*old,6000000,true) == 0x10,
              "old schema inherits its saved ADC0 table for ADC1 without changing selection");
        check(AnanSettings::setHardwareFilters(serial,7,5,legacy), "upgrade old filter document atomically");
        int version = 0;
        scope.featureExact(QStringLiteral("HardwareFilters"), &version);
        check(version == 8, "hardware filters use schema 8 with independent digital outputs");
        auto rows = options.value(QStringLiteral("receiveAdc1")).toList();
        rows[2] = QVariantMap{{QStringLiteral("lowHz"), 1000000}, {QStringLiteral("highHz"), 6500000}};
        options.insert(QStringLiteral("receiveAdc1"),rows);
        check(!parseFilterOptions(options) && !AnanSettings::setHardwareFilters(serial,7,5,options),
              "overlapping ADC1 bands cannot overwrite saved configuration");
        options.insert(QStringLiteral("receiveAdc1"),QVariantList{});
        check(!parseFilterOptions(options), "present but incomplete ADC1 table is rejected");
    }
    {
        // A DIY override and another MAC must never share edited ranges.
        const QString serial = QStringLiteral("FILTER-TEST");
        auto config = defaultFilterConfig(false);
        config.receive[0].lowHz = 1900000;
        const auto options = filterOptions(config);
        check(AnanSettings::setHardwareFilters(serial, 7, 5, options), "save hardware filter document");
        const auto restored = parseFilterOptions(AnanSettings::hardwareFilters(serial, 7, 5));
        check(restored && restored->receive[0].lowHz == 1900000, "custom filter ranges round-trip");
        check(AnanSettings::hardwareFilters(serial, 9, 5).isEmpty()
            && AnanSettings::hardwareFilters(QStringLiteral("OTHER-MAC"), 7, 5).isEmpty(),
            "filter ranges isolated by MAC and effective model");
        check(!AnanSettings::setHardwareFilters(QString(), 7, 5, options), "unknown identity cannot write family defaults");
        auto invalid = options;
        auto rows = invalid.value(QStringLiteral("receive")).toList();
        rows[1] = QVariantMap{{QStringLiteral("lowHz"), 1000000}, {QStringLiteral("highHz"), 9499999}};
        invalid.insert(QStringLiteral("receive"), rows);
        check(!parseFilterOptions(invalid) && !AnanSettings::setHardwareFilters(serial, 7, 5, invalid),
            "overlapping ranges rejected before persistence");
        invalid = options;
        invalid.insert(QStringLiteral("bypassAdc0"), QStringLiteral("true"));
        check(!parseFilterOptions(invalid), "malformed bypass flag rejected");
        const RadioSettingsScope scope(QStringLiteral("anan"), serial);
        check(scope.setFeature(QStringLiteral("HardwareFilters"), 99, QJsonObject{{QStringLiteral("future"), true}}),
            "seed future hardware filter schema");
        check(!AnanSettings::setHardwareFilters(serial, 7, 5, options)
            && scope.featureExact(QStringLiteral("HardwareFilters")).value(QStringLiteral("future")).toBool(),
            "future schema refused without data loss");
    }
    {
        auto config = defaultFilterConfig(true);
        config.ocEnabled = true;
        config.ocReceive[1] = {3500000, 3800000, 11};
        const auto data = filterOptions(config);
        check(AnanSettings::setHardwareFilters(QStringLiteral("OC-MAC"), 7, 5, data), "save explicit RX OC configuration");
        const auto saved = parseFilterOptions(AnanSettings::hardwareFilters(QStringLiteral("OC-MAC"), 7, 5));
        check(saved && saved->ocEnabled && receiveOcMask(*saved, 3700000) == 11, "RX OC configuration round-trip");
        config.ocSwl[4] = {5060000,7199999,65};
        auto swlData = filterOptions(config);
        check(AnanSettings::setHardwareFilters(QStringLiteral("OC-MAC"), 7, 5, swlData), "save independent SWL pins");
        const auto swlSaved = parseFilterOptions(AnanSettings::hardwareFilters(QStringLiteral("OC-MAC"),7,5));
        check(swlSaved && receiveOcMask(*swlSaved,6100000) == 65
            && receiveOcMask(*swlSaved,3700000) == 11, "SWL and HF persist independently");
        swlData.remove(QStringLiteral("ocSwl"));
        const auto oldFilters = parseFilterOptions(swlData);
        check(oldFilters && receiveOcMask(*oldFilters,6100000) == 0,
              "older filter documents migrate with every SWL pin off");
        auto badSwl = filterOptions(config);
        auto swlRows = badSwl.value(QStringLiteral("ocSwl")).toList();
        swlRows[5] = QVariantMap{{QStringLiteral("lowHz"),6000000},{QStringLiteral("highHz"),9000000},
            {QStringLiteral("mask"),1}};
        badSwl.insert(QStringLiteral("ocSwl"),swlRows);
        check(!parseFilterOptions(badSwl), "overlapping active SWL ranges refused");
        swlRows.removeLast();
        badSwl.insert(QStringLiteral("ocSwl"),swlRows);
        check(!parseFilterOptions(badSwl), "incomplete SWL table refused");
        auto invalid = data;
        auto rows = invalid.value(QStringLiteral("ocReceive")).toList();
        rows[2] = QVariantMap{{QStringLiteral("lowHz"), 3600000}, {QStringLiteral("highHz"), 4000000}, {QStringLiteral("mask"), 5}};
        invalid.insert(QStringLiteral("ocReceive"), rows);
        check(!parseFilterOptions(invalid), "overlapping active OC bands rejected");
        invalid = data;
        invalid.insert(QStringLiteral("ocEnabled"), QStringLiteral("yes"));
        check(!parseFilterOptions(invalid), "OC enable must be a boolean");
        auto legacy = data;
        legacy.remove(QStringLiteral("ocEnabled")); legacy.remove(QStringLiteral("ocReceive"));
        const auto migrated = parseFilterOptions(legacy);
        check(migrated && !migrated->ocEnabled && receiveOcMask(*migrated, 3700000) == 0,
              "version 1 filter documents never enable OC outputs on upgrade");
    }
    {
        auto config = defaultFilterConfig(false);
        config.receiveBypass[2] = true;
        config.disable6mLnaOnRx = true;
        config.lpfBypassOnRx = true;
        check(AnanSettings::setHardwareFilters(QStringLiteral("RX-BYPASS-MAC"),7,5,filterOptions(config)),
              "save RX band bypass, LNA and LPF options");
        const auto saved = parseFilterOptions(AnanSettings::hardwareFilters(QStringLiteral("RX-BYPASS-MAC"),7,5));
        check(saved && saved->receiveBypass[2] && saved->disable6mLnaOnRx && saved->lpfBypassOnRx,
              "RX bypass options round-trip per device and model");
        auto legacy = filterOptions(config);
        legacy.remove(QStringLiteral("receiveBypass")); legacy.remove(QStringLiteral("disable6mLnaOnRx")); legacy.remove(QStringLiteral("lpfBypassOnRx"));
        const auto defaults = parseFilterOptions(legacy);
        check(defaults && !defaults->receiveBypass[2] && !defaults->disable6mLnaOnRx && !defaults->lpfBypassOnRx,
              "older hardware documents keep automatic receive filters on upgrade");
        auto malformed = filterOptions(config);
        malformed.insert(QStringLiteral("receiveBypass"), QVariantList{true});
        check(!parseFilterOptions(malformed), "truncated receive bypass array rejected");
        malformed = filterOptions(config); malformed.insert(QStringLiteral("disable6mLnaOnRx"),QStringLiteral("true"));
        check(!parseFilterOptions(malformed), "LNA bypass flag requires a boolean");
    }
    {
        AnanSettings::setRxAntenna(1); // Existing global value becomes a legacy fallback only.
        const QString a = QStringLiteral("ANTENNA-MAC-A"), b = QStringLiteral("ANTENNA-MAC-B");
        check(AnanSettings::setRadioRxAntenna(a,7,5,3), "save antenna for one MAC/model/board");
        check(AnanSettings::radioRxAntenna(a,7,5) == 3, "antenna choice round-trips");
        check(AnanSettings::radioRxAntenna(b,7,5) == 1
              && AnanSettings::radioRxAntenna(a,9,5) == 1
              && AnanSettings::radioRxAntenna(a,7,4) == 1,
              "another device, override model or physical board cannot inherit a live antenna change");
        check(AnanSettings::rxAntenna() == 1, "per-device antenna writes leave legacy defaults unchanged");
        check(!AnanSettings::setRadioRxAntenna(QString(),7,5,2)
              && !AnanSettings::setRadioRxAntenna(a,7,5,0)
              && !AnanSettings::setRadioRxAntenna(a,7,5,4), "unknown identity and invalid antenna refused");
        const RadioSettingsScope scope(QStringLiteral("anan"),a);
        check(scope.setFeature(QStringLiteral("ReceiveAntenna"),1,
              QJsonObject{{QStringLiteral("7:5"),1.5}}), "seed malformed antenna value");
        check(AnanSettings::radioRxAntenna(a,7,5) == 1, "fractional antenna cannot be applied");
        check(scope.setFeature(QStringLiteral("ReceiveAntenna"),99,
              QJsonObject{{QStringLiteral("future"),true}}), "seed future antenna schema");
        check(!AnanSettings::setRadioRxAntenna(a,7,5,2)
              && scope.featureExact(QStringLiteral("ReceiveAntenna")).value(QStringLiteral("future")).toBool(),
              "future antenna settings are not overwritten");
    }
    {
        const QString serial = QStringLiteral("RX-INPUT-MAC");
        auto config = defaultFilterConfig(true);
        config.rxOnlyInput = 2;
        check(AnanSettings::setHardwareFilters(serial,7,5,filterOptions(config)), "save RX-only input per device/model");
        const auto saved = parseFilterOptions(AnanSettings::hardwareFilters(serial,7,5));
        check(saved && saved->rxOnlyInput == 2 && AnanSettings::hardwareFilters(serial,9,5).isEmpty()
              && AnanSettings::hardwareFilters(QStringLiteral("OTHER-RX-INPUT-MAC"),7,5).isEmpty(),
              "RX input persists without crossing device/model scopes");
        auto legacy = filterOptions(config);
        legacy.remove(QStringLiteral("rxOnlyInput"));
        check(parseFilterOptions(legacy)->rxOnlyInput == 0, "old filter documents retain ANT bank input");
        for (const QVariant& invalid : {QVariant(-1), QVariant(4), QVariant(1.5), QVariant(true),
                QVariant(QStringLiteral("2")), QVariant(QStringLiteral("bad"))}) {
            auto data = filterOptions(config);
            data.insert(QStringLiteral("rxOnlyInput"),invalid);
            check(!parseFilterOptions(data) && !AnanSettings::setHardwareFilters(serial,7,5,data),
                  "malformed RX input cannot overwrite saved route");
        }
        check(parseFilterOptions(AnanSettings::hardwareFilters(serial,7,5))->rxOnlyInput == 2,
              "refused RX input leaves saved selection unchanged");
    }
    // The amplifier option does not replace device-scoped filter ownership.
    AnanSettings::reset();
    check(!AnanSettings::speakerAmpMuted(), "speaker amplifier defaults unmuted");
    AnanSettings::setDdc0RateKsps(96);
    AnanSettings::setSpeakerAmpMuted(true);
    check(AnanSettings::speakerAmpMuted() && AnanSettings::ddc0RateKsps() == 96,
          "amplifier mute persists without replacing connection defaults");
    AnanSettings::setSpeakerAmpMuted(false);
    check(!AnanSettings::speakerAmpMuted(), "amplifier mute round-trips off");

    // The rate list a setup page offers. It exists on this header because a
    // page above the radio seam may not include the protocol header that owns
    // the constant.
    const QList<int> rates = AnanSettings::validDdc0RatesKsps();
    check(rates.size() == 6, "six DDC0 rates, as the protocol defines");
    check(rates.contains(48) && rates.contains(1536),
          "including both ends of the range");

    {
        auto power = defaultTransmitPower(AnanModel::Anan7000);
        check(power.has_value() && !defaultTransmitPower(AnanModel::Automatic), "PA reference requires explicit supported model");
        check(previewTransmitPower(*power,0,100)->drive == 0, "zero band limit produces no drive");
        power->gainsDb[0] = 50;
        power->limitsWatts[0] = 100;
        const auto quarter = transmitPowerForPercent(*power,0,25);
        check(quarter && quarter->watts == 25 && quarter->drive == 36,
              "percent maps to configured band watts before logarithmic drive");
        check(!transmitPowerForPercent(*power,0,101) && !transmitPowerForPercent(*power,-1,25),
              "percent and band bounds cannot wrap");
        const auto preview = previewTransmitPower(*power,0,200);
        check(preview && preview->watts == 100 && preview->drive == 72
              && std::abs(preview->audioVolume - std::sqrt(0.05)/0.8) < 1e-12,
              "Thetis logarithmic gain, band cap and integer truncation agree");
        check(!previewTransmitPower(*power,-1,10) && !previewTransmitPower(*power,11,10)
              && !previewTransmitPower(*power,0,std::numeric_limits<double>::infinity()), "invalid preview input refused");
        power->gainsDb[0] = 100;
        check(previewTransmitPower(*power,0,100)->drive == 0, "100 dB never means full drive");
        const auto options = transmitPowerOptions(*power);
        check(AnanSettings::setTransmitPower(QStringLiteral("PA-MAC"),static_cast<int>(AnanModel::Anan7000),5,2,options), "PA profile saves");
        check(AnanSettings::transmitPower(QStringLiteral("PA-MAC"),static_cast<int>(AnanModel::Anan7000),5,2) == options, "PA profile round trip");
        const auto reloadedPower = parseTransmitPower(AnanSettings::transmitPower(QStringLiteral("PA-MAC"),
            static_cast<int>(AnanModel::Anan7000),5,2));
        check(reloadedPower && reloadedPower->limitsWatts[0] == 100 && reloadedPower->gainsDb[0] == 100,
              "stored JSON integral values remain usable for transmit admission");
        check(AnanSettings::transmitPower(QStringLiteral("OTHER"),static_cast<int>(AnanModel::Anan7000),5,2).isEmpty()
              && AnanSettings::transmitPower(QStringLiteral("PA-MAC"),static_cast<int>(AnanModel::Anan7000),5,1).isEmpty()
              && AnanSettings::transmitPower(QStringLiteral("PA-MAC"),static_cast<int>(AnanModel::Anan8000),5,2).isEmpty(), "PA scope isolates device model and protocol");
        auto malformed = options;
        auto gains = malformed.value(QStringLiteral("gainsDb")).toList(); gains[0] = true;
        malformed.insert(QStringLiteral("gainsDb"),gains);
        check(!parseTransmitPower(malformed)
              && !AnanSettings::setTransmitPower(QStringLiteral("PA-MAC"),static_cast<int>(AnanModel::Anan7000),5,2,malformed), "boolean gain cannot overwrite profile");
        const RadioSettingsScope scope(QStringLiteral("anan"),QStringLiteral("PA-FUTURE"));
        check(scope.setFeature(QStringLiteral("TransmitPower"),99,QJsonObject{{QStringLiteral("future"),true}}), "seed future PA schema");
        check(!AnanSettings::setTransmitPower(QStringLiteral("PA-FUTURE"),static_cast<int>(AnanModel::Anan7000),5,2,options), "future PA schema preserved");
    }
    if (g_failures == 0)
        std::printf("anan_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
