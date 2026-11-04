#include "AsioSettingsModel.h"

#include "core/AudioEngine.h"

#include <QMetaObject>

namespace AetherSDR {

AsioSettingsModel::AsioSettingsModel(AudioEngine* audio, QObject* parent)
    : QObject(parent), m_audio(audio)
{
    if (audio) {
        connect(audio, &AudioEngine::asioOutputStatus, this, &AsioSettingsModel::statusChanged);
    }
}

AsioOutputConfig AsioSettingsModel::config() const
{
    return AsioOutputConfig::load();
}

QList<AsioOutputDriver> AsioSettingsModel::drivers() const
{
    return AsioOutput::drivers();
}

bool AsioSettingsModel::available() const
{
    return !m_audio.isNull();
}

void AsioSettingsModel::applyConfig(const AsioOutputConfig& config)
{
    const QPointer<AudioEngine> audio = m_audio;
    if (!audio) {
        return;
    }
    QMetaObject::invokeMethod(audio, [audio, config] {
        if (audio) {
            audio->setAsioOutputConfig(config);
        }
    }, Qt::QueuedConnection);
}

} // namespace AetherSDR
