#pragma once
#include <QGroupBox>

namespace AetherSDR {
class AudioEngine;
class AsioOutputSettingsWidget final : public QGroupBox {
public:
    explicit AsioOutputSettingsWidget(AudioEngine* audio, QWidget* parent = nullptr);
};
}
