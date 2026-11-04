#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace AetherSDR {

class RadioModel;

// Pull-based settings presentation; the setup page retains its existing reseed lifecycle.
class SaturnSettingsModel final : public QObject {
public:
    struct ImplementationChoice {
        QString id;
        QString caption;
    };
    struct Snapshot {
        bool connected = false;
        bool applicable = true;
        bool originalSelected = false;
        QVariantMap originalOptions;
    };

    explicit SaturnSettingsModel(RadioModel* radio, QObject* parent = nullptr);
    static QList<ImplementationChoice> implementationChoices();
    bool isConnected() const;
    Snapshot snapshot() const;
    bool setImplementation(const QString& id);
    void setOriginalOption(const QString& name, const QVariant& value);

private:
    QPointer<RadioModel> m_radio;
};

} // namespace AetherSDR
