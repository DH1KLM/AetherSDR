#pragma once
#include <QGroupBox>

namespace AetherSDR {
class AsioSettingsModel;
class AsioOutputSettingsWidget final : public QGroupBox {
public:
    explicit AsioOutputSettingsWidget(AsioSettingsModel* model, QWidget* parent = nullptr);
};
}
