#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "models/AsioSettingsModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("asio-settings-model"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) {
            ++failures;
        }
    };

    AsioSettingsModel unavailable(nullptr);
    check(!unavailable.available(), "no engine is reported unavailable");
    unavailable.applyConfig({});
    check(!AppSettings::instance().contains("AsioOutput"), "unavailable engine cannot persist settings");

    // Invalid configuration exercises the production dispatch and status route
    // without opening a driver, device, socket or radio session.
    auto audio = std::make_unique<AudioEngine>();
    AsioSettingsModel model(audio.get());
    QSignalSpy status(&model, &AsioSettingsModel::statusChanged);
    check(model.available(), "live engine is available");
    AsioOutputConfig config;
    config.sampleRate = 12345;
    model.applyConfig(config);
    config.sampleRate = 48000;
    check(status.isEmpty() && !AppSettings::instance().contains("AsioOutput"),
          "application is queued rather than executed in the caller");
    QCoreApplication::sendPostedEvents(audio.get(), QEvent::MetaCall);
    check(status.size() == 1 && status.first().first().toString() == QStringLiteral("ASIO settings could not be saved."),
          "engine receives the queued value snapshot and returns refusal through the model");
    check(!AppSettings::instance().contains("AsioOutput"), "invalid queued configuration is not persisted");

    config.sampleRate = 12345;
    model.applyConfig(config);
    audio.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!model.available() && status.size() == 1, "engine retirement cancels pending application and clears availability");
    model.applyConfig(config);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(status.size() == 1, "retired engine receives no further application");
    return failures == 0 ? 0 : 1;
}
