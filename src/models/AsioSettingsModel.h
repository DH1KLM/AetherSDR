#pragma once

#include "core/AsioOutput.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class AudioEngine;

// Client audio-device configuration; AudioEngine retains driver and stream ownership.
class AsioSettingsModel final : public QObject {
    Q_OBJECT

public:
    explicit AsioSettingsModel(AudioEngine* audio, QObject* parent = nullptr);
    AsioOutputConfig config() const;
    QList<AsioOutputDriver> drivers() const;
    bool available() const;
    void applyConfig(const AsioOutputConfig& config);

signals:
    void statusChanged(const QString& message);

private:
    QPointer<AudioEngine> m_audio;
};

} // namespace AetherSDR
