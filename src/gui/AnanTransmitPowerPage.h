#pragma once

class QWidget;
namespace AetherSDR {
class RadioModel;
QWidget* createAnanTransmitPowerPage(RadioModel* model, QWidget* parent);
QWidget* createAnanAdcPage(RadioModel* model, QWidget* parent);
}
