#include "SaturnSettingsModel.h"

#include "RadioModel.h"
#include "core/backends/anan/AnanImplementation.h"

namespace AetherSDR {

SaturnSettingsModel::SaturnSettingsModel(RadioModel* radio, QObject* parent)
    : QObject(parent), m_radio(radio)
{
}

QList<SaturnSettingsModel::ImplementationChoice> SaturnSettingsModel::implementationChoices()
{
    return {{QString(), anan::AnanImplementation::label(false)},
            {anan::AnanImplementation::originalId(), anan::AnanImplementation::label(true)}};
}

bool SaturnSettingsModel::isConnected() const
{
    return m_radio && m_radio->isConnected();
}

SaturnSettingsModel::Snapshot SaturnSettingsModel::snapshot() const
{
    Snapshot state;
    state.connected = isConnected();
    const RadioInfo radio = m_radio ? m_radio->lastRadioInfo() : RadioInfo{};
    const bool saturn = anan::AnanImplementation::isSaturn(radio);
    state.applicable = !state.connected || saturn;
    const QString selected = state.connected ? radio.implementation
        : anan::AnanImplementation::preferred(saturn ? radio.serial : QString());
    state.originalSelected = selected == anan::AnanImplementation::originalId();
    state.originalOptions = anan::AnanImplementation::originalOptions();
    return state;
}

bool SaturnSettingsModel::setImplementation(const QString& id)
{
    if (isConnected()) {
        return false;
    }
    const RadioInfo radio = m_radio ? m_radio->lastRadioInfo() : RadioInfo{};
    return anan::AnanImplementation::setPreferred(
        anan::AnanImplementation::isSaturn(radio) ? radio.serial : QString(), id);
}

void SaturnSettingsModel::setOriginalOption(const QString& name, const QVariant& value)
{
    anan::AnanImplementation::setOriginalOption(name, value);
    if (isConnected() && m_radio->lastRadioInfo().implementation == anan::AnanImplementation::originalId()
        && (name == QLatin1String("bandFiltersEnabled") || name == QLatin1String("speakerAmpMuted"))) {
        m_radio->invokeBackendExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 0,
                                       QVariantMap{{name, value}});
    }
}

} // namespace AetherSDR
