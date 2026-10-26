#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanImplementation.h"
#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan_original/P2Protocol.h"
#include "core/RadioSettingsScope.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <cstdio>

using namespace AetherSDR;
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
    return failures == 0 ? 0 : 1;
}
