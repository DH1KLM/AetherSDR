#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanImplementation.h"
#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan_original/P2Protocol.h"
#include "core/RadioSettingsScope.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SaturnSettingsModel.h"
#include "core/backends/IRadioBackend.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <cstdio>
#include <optional>
#include <memory>

using namespace AetherSDR;

// Injected seam only: no transport or firmware peer.
class SaturnSettingsProbe final : public IRadioBackend {
public:
    bool connected = true;
    int frontendWrites = 0;
    QVariantMap lastOptions;
    RadioCapabilities capabilities() const override {
        RadioCapabilities caps;
        caps.canTransmit = false;
        caps.extensionNamespaces = {QStringLiteral("anan")};
        return caps;
    }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString& ns, const QString& verb, quint64, const QVariant& arg) override {
        if (ns == QLatin1String("anan") && verb == QLatin1String("frontend.set")) {
            ++frontendWrites;
            lastOptions = arg.toMap();
        }
    }
};

static int saturnSettingsTests()
{
    using anan::AnanImplementation;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) { ++failures; }
    };
    const QString previousFamilyChoice = AnanImplementation::preferred({});
    SaturnSettingsModel defaults(nullptr);
    check(defaults.setImplementation(AnanImplementation::originalId())
        && defaults.snapshot().originalSelected && defaults.snapshot().applicable,
          "disconnected Saturn model retains family-wide implementation selection");
    RadioModel model;
    RadioInfo radio;
    radio.family = QStringLiteral("anan");
    radio.serial = QStringLiteral("00:11:22:33:44:B0");
    radio.boardId = 10;
    radio.hpsdrProtocol = 2;
    radio.implementation = AnanImplementation::originalId();
    // Empty host is refused before the original backend opens its transport.
    QSignalSpy errors(&model, &RadioModel::connectionError);
    model.connectToRadio(radio);
    check(!errors.isEmpty() && !model.isConnected(), "invalid host seeds selected identity without a radio connection");
    SaturnSettingsModel settings(&model);
    check(settings.setImplementation(QString()) && !settings.snapshot().originalSelected
        && AnanImplementation::preferred({}) == AnanImplementation::originalId(),
          "per-radio implementation choice does not overwrite the family preference");
    check(settings.setImplementation(AnanImplementation::originalId()), "seed original radio preference");
    auto owned = std::make_unique<SaturnSettingsProbe>();
    SaturnSettingsProbe* probe = owned.get();
    model.setBackendForTest(std::move(owned), QStringLiteral("anan"));
    check(settings.snapshot().connected && settings.snapshot().applicable && settings.snapshot().originalSelected
        && !settings.setImplementation(QString())
        && AnanImplementation::preferred(radio.serial) == AnanImplementation::originalId(),
          "connected implementation selection is guarded in the model");
    settings.setOriginalOption(QStringLiteral("bandFiltersEnabled"), true);
    check(probe->frontendWrites == 1 && probe->lastOptions.value(QStringLiteral("bandFiltersEnabled")).toBool(),
          "original band-filter edit uses the existing live extension route");
    settings.setOriginalOption(QStringLiteral("speakerAmpMuted"), true);
    check(probe->frontendWrites == 2 && probe->lastOptions.value(QStringLiteral("speakerAmpMuted")).toBool(),
          "original speaker mute uses the existing live extension route");
    const int extendedRate = anan::AnanSettings::ddc0RateKsps();
    settings.setOriginalOption(QStringLiteral("ddc0RateKsps"), 96);
    check(probe->frontendWrites == 2
        && settings.snapshot().originalOptions.value(QStringLiteral("ddc0RateKsps")).toInt() == 96
        && anan::AnanSettings::ddc0RateKsps() == extendedRate,
          "next-connect settings persist only in original configuration without live dispatch");
    probe->connected = false;
    settings.setOriginalOption(QStringLiteral("bandFiltersEnabled"), false);
    check(probe->frontendWrites == 2, "disconnected original edits do not reach the backend");
    {
        RadioModel other;
        auto otherOwned = std::make_unique<SaturnSettingsProbe>();
        SaturnSettingsProbe* otherProbe = otherOwned.get();
        other.setBackendForTest(std::move(otherOwned), QStringLiteral("anan"));
        SaturnSettingsModel otherSettings(&other);
        otherSettings.setOriginalOption(QStringLiteral("speakerAmpMuted"), false);
        check(otherProbe->frontendWrites == 0, "an active non-original session receives no original frontend edits");
    }
    auto retired = std::make_unique<RadioModel>();
    SaturnSettingsModel retiredSettings(retired.get());
    retired.reset();
    check(!retiredSettings.isConnected() && retiredSettings.snapshot().applicable,
          "retired radio model leaves a safe disconnected settings presentation");
    check(defaults.setImplementation(previousFamilyChoice), "restore family implementation preference");
    return failures;
}

static int manualSelectionTests()
{
    using anan::AnanImplementation;
    using anan::AnanModel;
    using anan::AnanSettings;
    using Selection = AnanImplementation::ManualSelection;
    using Status = AnanImplementation::ManualSelectionStatus;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) { ++failures; }
    };
    RadioInfo radio;
    radio.family = QStringLiteral("anan");
    radio.serial = QStringLiteral("00:11:22:33:44:A0");
    radio.boardId = 10;
    radio.hpsdrProtocol = 2;
    radio.address = QHostAddress(QStringLiteral("192.0.2.4"));
    radio.port = 1024;
    radio.model = QStringLiteral("Physical Saturn");
    const RadioSettingsScope scope(QStringLiteral("anan"), radio.serial);
    const QString hardware = QStringLiteral("HardwareProfile");
    const QString implementation = QStringLiteral("Implementation");
    auto result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::Accepted && result.resetChoices && scope.featureExact(hardware).isEmpty(),
          "saved automatic selection connects without creating a hardware override");
    check(result.radio.address == radio.address && result.radio.serial == radio.serial
        && result.radio.boardId == radio.boardId && result.radio.hpsdrProtocol == radio.hpsdrProtocol
        && result.radio.model == radio.model && result.radio.port == radio.port,
          "manual profile resolution preserves physical discovery identity");
    check(AnanImplementation::setPreferred(radio.serial, AnanImplementation::originalId()), "seed original preference");
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::Accepted && result.radio.implementation == AnanImplementation::originalId(),
          "saved original preference resolves to the original backend");
    const Selection explicitG2{static_cast<int>(AnanModel::AnanG2), 0, false};
    result = AnanImplementation::selectForManualConnect(radio, explicitG2);
    check(result.status == Status::Accepted && result.radio.implementation.isEmpty()
        && AnanImplementation::preferred(radio.serial).isEmpty()
        && AnanSettings::radioProfile(radio.serial).model == AnanModel::AnanG2,
          "explicit extended profile replaces an original preference");
    check(AnanSettings::setRadioProfile(radio.serial, AnanModel::AnanG2, 1), "seed saved P1 preference");
    const QJsonObject beforeMismatch = scope.featureExact(hardware);
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::IgnoreProtocol && !result.resetChoices
        && scope.featureExact(hardware) == beforeMismatch,
          "saved protocol mismatch is ignored without consuming choices or saving");
    result = AnanImplementation::selectForManualConnect(radio, Selection{-100, 2, std::nullopt});
    check(result.status == Status::Accepted && AnanSettings::radioProfile(radio.serial).protocol == 2,
          "explicit protocol replaces the saved preference");
    const QJsonObject beforeOverride = scope.featureExact(hardware);
    const Selection mismatched{static_cast<int>(AnanModel::Anan200D), 0, false};
    result = AnanImplementation::selectForManualConnect(radio, mismatched);
    check(result.status == Status::InvalidProfile && !result.resetChoices
        && scope.featureExact(hardware) == beforeOverride,
          "board mismatch without override refuses before persistence");
    result = AnanImplementation::selectForManualConnect(radio,
        Selection{static_cast<int>(AnanModel::Anan200D), 0, true});
    check(result.status == Status::Accepted && AnanSettings::radioProfile(radio.serial).overrideDetection,
          "explicit supported board override is retained");
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::Accepted && AnanSettings::radioProfile(radio.serial).overrideDetection,
          "saved override is used when its control is unspecified");
    result = AnanImplementation::selectForManualConnect(radio, Selection{-1, 0, true});
    check(result.status == Status::InvalidOverride && !result.resetChoices,
          "automatic profile cannot acquire an explicit board override");
    result = AnanImplementation::selectForManualConnect(radio, Selection{-200, 1, std::nullopt});
    check(result.status == Status::IgnoreProtocol, "explicit wire protocol also constrains original selection");
    radio.hpsdrProtocol = 1;
    result = AnanImplementation::selectForManualConnect(radio, Selection{-200, 0, std::nullopt});
    check(result.status == Status::OriginalUnavailable, "original implementation refuses P1");
    radio.hpsdrProtocol = 2;
    radio.boardId = 20;
    result = AnanImplementation::selectForManualConnect(radio, Selection{-200, 0, std::nullopt});
    check(result.status == Status::OriginalUnavailable, "original implementation refuses non-Saturn boards");
    radio.boardId = 10;
    radio.serial = QStringLiteral("00:11:22:33:44:A1");
    const RadioSettingsScope futureScope(QStringLiteral("anan"), radio.serial);
    const QJsonObject future{{QStringLiteral("future"), true}};
    check(futureScope.setFeature(hardware, 99, future), "seed future hardware profile");
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::InvalidProfile && !result.resetChoices,
          "saved future hardware profile cannot resolve a selection");
    result = AnanImplementation::selectForManualConnect(radio, explicitG2);
    check(result.status == Status::ProfileSaveFailed && !result.resetChoices
        && futureScope.featureExact(hardware) == future && futureScope.featureExact(implementation).isEmpty(),
          "explicit selection cannot overwrite a future profile or save implementation after failure");
    check(futureScope.setFeature(hardware, 3, QJsonObject{{QStringLiteral("model"), 999},
        {QStringLiteral("protocol"), 0}, {QStringLiteral("overrideDetection"), false}}), "seed invalid saved model");
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::InvalidProfile, "invalid saved model is refused");
    radio.serial = QStringLiteral("00:11:22:33:44:A2");
    const RadioSettingsScope failedImplementation(QStringLiteral("anan"), radio.serial);
    check(failedImplementation.setFeature(implementation, 99, future), "seed future implementation preference");
    result = AnanImplementation::selectForManualConnect(radio, {});
    check(result.status == Status::ImplementationSaveFailed && !result.resetChoices
        && failedImplementation.featureExact(hardware).isEmpty(),
          "implementation failure without profile edit retains explicit controls");
    result = AnanImplementation::selectForManualConnect(radio, explicitG2);
    check(result.status == Status::ImplementationSaveFailed && result.resetChoices
        && AnanSettings::radioProfile(radio.serial).model == AnanModel::AnanG2
        && failedImplementation.featureExact(implementation) == future,
          "successful profile write consumes choices even if implementation persistence fails");
    radio.serial.clear();
    result = AnanImplementation::selectForManualConnect(radio, explicitG2);
    check(result.status == Status::ProfileSaveFailed && !result.resetChoices,
          "unknown identity cannot persist an explicit hardware profile");
    return failures;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-implementation"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) { ++failures; }
    };
    using anan::AnanImplementation;
    RadioInfo physical;
    physical.family = QStringLiteral("anan");
    physical.serial = QStringLiteral("00:11:22:33:44:55");
    physical.boardId = 10;
    physical.hpsdrProtocol = 2;
    physical.address = QHostAddress(QStringLiteral("192.0.2.1"));
    const QList<RadioInfo> choices = AnanImplementation::choices(physical);
    check(choices.size() == 2 && choices[0].selectionKey() != choices[1].selectionKey(), "two independently selectable Saturn choices");
    check(choices[0].serial == choices[1].serial && choices[0].address == choices[1].address,
          "physical MAC and IP retained");
    check(choices[0].settingsFamily() != choices[1].settingsFamily(), "settings scopes isolated");
    physical.boardId = 20;
    check(AnanImplementation::choices(physical).size() == 1, "G2E does not acquire a Saturn original choice");
    physical.boardId = 10;
    physical.hpsdrProtocol = 1;
    check(AnanImplementation::choices(physical).size() == 1, "P1 does not acquire a P2-only choice");
    check(AnanImplementation::setPreferred(physical.serial, AnanImplementation::originalId()), "selection persisted");
    physical.hpsdrProtocol = 2;
    check(AnanImplementation::selected(physical).implementation == AnanImplementation::originalId(), "automatic reconnect resolves saved implementation");
    check(!AnanImplementation::setPreferred(physical.serial, QStringLiteral("unknown")), "unknown selection refused");
    const QVariantMap originalBefore = AnanImplementation::originalOptions();
    anan::AnanSettings::setDdc0RateKsps(192);
    AnanImplementation::setOriginalOption(QStringLiteral("ddc0RateKsps"), 96);
    check(anan::AnanSettings::ddc0RateKsps() == 192 && AnanImplementation::originalOptions().value(QStringLiteral("ddc0RateKsps")).toInt() == 96,
          "front-end settings cannot overwrite the other variant");
    RadioConnectRequest request;
    request.params = {{QStringLiteral("anan.model"), 7}, {QStringLiteral("anan.overrideDetection"), true}, {QStringLiteral("other.keep"), 4}};
    AnanImplementation::populateParams(request, AnanImplementation::originalId());
    check(!request.params.contains(QStringLiteral("anan.model")) && !request.params.contains(QStringLiteral("anan.overrideDetection"))
        && request.params.value(QStringLiteral("anan.ddc0RateKsps")).toInt() == 96
        && request.params.value(QStringLiteral("other.keep")).toInt() == 4, "original connect ignores extended override and uses its own config");
    RadioInfo selected = physical;
    selected.implementation = AnanImplementation::originalId();
    RadioConnectRequest originalRequest;
    originalRequest.params = {{QStringLiteral("anan.model"), 7}, {QStringLiteral("other.keep"), 4}};
    AnanImplementation::populateParams(originalRequest, selected);
    check(originalRequest.params.value(QStringLiteral("anan.boardId"), -1).toInt() == 10
        && originalRequest.params.value(QStringLiteral("anan.protocol"), -1).toInt() == 2
        && originalRequest.params.value(QStringLiteral("anan.ddc0RateKsps")).toInt() == 96
        && !originalRequest.params.contains(QStringLiteral("anan.model"))
        && originalRequest.params.value(QStringLiteral("other.keep")).toInt() == 4,
          "original option reset retains discovered identity and unrelated parameters");
    selected.implementation.clear();
    selected.boardId = 0;
    selected.hpsdrProtocol = 1;
    RadioConnectRequest p1Request;
    p1Request.params.insert(QStringLiteral("anan.model"), 7);
    AnanImplementation::populateParams(p1Request, selected);
    check(p1Request.params.value(QStringLiteral("anan.boardId"), -1).toInt() == 0
        && p1Request.params.value(QStringLiteral("anan.protocol"), -1).toInt() == 1
        && p1Request.params.value(QStringLiteral("anan.model"), -1).toInt() == 7,
          "P1 board zero and extended profile options survive request preparation");
    selected.family = QStringLiteral("ANAN");
    selected.boardId = -1;
    selected.hpsdrProtocol = 0;
    RadioConnectRequest manualRequest;
    AnanImplementation::populateParams(manualRequest, selected);
    check(!manualRequest.params.contains(QStringLiteral("anan.boardId"))
        && manualRequest.params.value(QStringLiteral("anan.protocol"), -1).toInt() == 2,
          "case-insensitive family and address-only P2 default retain existing behavior");
    selected.family = QStringLiteral("icom");
    selected.implementation = AnanImplementation::originalId();
    RadioConnectRequest foreignRequest;
    foreignRequest.params.insert(QStringLiteral("icom.waitingForWake"), true);
    const QVariantMap foreignBefore = foreignRequest.params;
    AnanImplementation::populateParams(foreignRequest, selected);
    check(foreignRequest.params == foreignBefore, "non-ANAN wake requests remain untouched");
    auto extended = AnanImplementation::makeBackend({});
    auto original = AnanImplementation::makeBackend(AnanImplementation::originalId());
    check(extended && original && QByteArray(extended->metaObject()->className()) != original->metaObject()->className(),
          "two executable backend classes, not two labels for the same backend");
    check(original->capabilities().maxSlices == 1 && original->capabilities().maxPanadapters == 1
        && !original->capabilities().canTransmit && !extended->capabilities().canTransmit, "original receive capacity and both TX boundaries retained");
    check(!AnanImplementation::makeBackend(QStringLiteral("unknown")), "unknown backend never falls through to another implementation");
    QSignalSpy errors(original.get(), &IRadioBackend::connectionError);
    request.params.insert(QStringLiteral("anan.boardId"), 20);
    request.params.insert(QStringLiteral("anan.protocol"), 2);
    original->connectRadio(request);
    check(errors.size() == 1 && !original->isConnected(), "original rejects non-Saturn before opening transport");
    {
        RadioModel model;
        RadioInfo refused = physical;
        refused.boardId = 20;
        refused.implementation = AnanImplementation::originalId();
        QSignalSpy refusedErrors(&model, &RadioModel::connectionError);
        model.connectToRadio(refused);
        check(model.backend() && QByteArray(model.backend()->metaObject()->className()) == original->metaObject()->className()
            && model.settingsScope().family() == QLatin1String("anan-tropo1234") && !refusedErrors.isEmpty(),
            "production model constructs the original backend and its own scope, with no transport for a mismatched board");
        model.rebuildBackendForTest(QStringLiteral("anan"));
        check(QByteArray(model.backend()->metaObject()->className()) == extended->metaObject()->className()
            && model.settingsScope().family() == QLatin1String("anan"),
            "production backend teardown and rebuild return to the independent extended path");
    }
    const RadioSettingsScope a(QStringLiteral("anan"), physical.serial);
    const RadioSettingsScope b(QStringLiteral("anan-tropo1234"), physical.serial);
    check(a.setFeature(QStringLiteral("OperatingState"), 1, QJsonObject{{QStringLiteral("test"), 1}})
        && b.setFeature(QStringLiteral("OperatingState"), 1, QJsonObject{{QStringLiteral("test"), 2}})
        && a.featureExact(QStringLiteral("OperatingState")).value(QStringLiteral("test")).toInt() == 1,
          "original operating state cannot overwrite extended operating state");
    const auto ddc = anan_original::buildDdcSpecific();
    check(ddc[4] == 2, "original DDC packet still declares its two physical ADCs");
    AnanImplementation::setOriginalOption(QStringLiteral("ddc0RateKsps"), originalBefore.value(QStringLiteral("ddc0RateKsps")));
    failures += manualSelectionTests();
    failures += saturnSettingsTests();
    return failures == 0 ? 0 : 1;
}
