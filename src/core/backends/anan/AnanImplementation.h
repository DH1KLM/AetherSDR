#pragma once

#include "core/RadioDiscovery.h"
#include "core/backends/IRadioBackend.h"
#include <QList>
#include <QString>
#include <memory>
#include <optional>

namespace AetherSDR::anan {

// Empty retains the existing fork identity and all previously saved values.
class AnanImplementation {
public:
    struct ManualProfileChoice {
        int profileId;
        QString caption;
    };
    struct ManualSelection {
        int profileId = -100; // Existing picker ID for the saved preference.
        int protocol = -1;   // Negative uses the saved preference; zero is automatic.
        std::optional<bool> overrideDetection;
    };
    enum class ManualSelectionStatus {
        Accepted, IgnoreProtocol, OriginalUnavailable, InvalidProfile,
        InvalidOverride, ProfileSaveFailed, ImplementationSaveFailed
    };
    struct ManualSelectionResult {
        ManualSelectionStatus status = ManualSelectionStatus::InvalidProfile;
        RadioInfo radio;
        bool resetChoices = false;
    };
    static QString originalId() { return QStringLiteral("tropo1234"); }
    static QString label(bool original);
    static QList<ManualProfileChoice> manualProfileChoices();
    static bool isSaturn(const RadioInfo& radio);
    static QList<RadioInfo> choices(const RadioInfo& radio);
    static QString preferred(const QString& serial);
    static bool setPreferred(const QString& serial, const QString& implementation);
    static RadioInfo selected(const RadioInfo& radio);
    static ManualSelectionResult selectForManualConnect(const RadioInfo& radio,
                                                        const ManualSelection& selection);
    static void populateParams(RadioConnectRequest& request, const RadioInfo& radio);
    static void populateParams(RadioConnectRequest& request, const QString& implementation);
    static QVariantMap originalOptions();
    static void setOriginalOption(const QString& name, const QVariant& value);
    static std::unique_ptr<IRadioBackend> makeBackend(const QString& implementation);
};
}
