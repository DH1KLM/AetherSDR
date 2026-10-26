#pragma once

#include "core/RadioDiscovery.h"
#include "core/backends/IRadioBackend.h"
#include <memory>

namespace AetherSDR::anan {

// Empty retains the existing fork identity and all previously saved values.
class AnanImplementation {
public:
    static QString originalId() { return QStringLiteral("tropo1234"); }
    static QString label(bool original);
    static bool isSaturn(const RadioInfo& radio);
    static QList<RadioInfo> choices(const RadioInfo& radio);
    static QString preferred(const QString& serial);
    static bool setPreferred(const QString& serial, const QString& implementation);
    static RadioInfo selected(const RadioInfo& radio);
    static void populateParams(RadioConnectRequest& request, const QString& implementation);
    static QVariantMap originalOptions();
    static void setOriginalOption(const QString& name, const QVariant& value);
    static std::unique_ptr<IRadioBackend> makeBackend(const QString& implementation);
};
}
