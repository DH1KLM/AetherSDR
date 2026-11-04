#pragma once

class QWidget;
namespace AetherSDR {
class RadioModel;
QWidget* createAnanDiversityPage(RadioModel* model, QWidget* parent);
QWidget* createAnanTransmitPowerPage(RadioModel* model, QWidget* parent);
QWidget* createAnanAdcPage(RadioModel* model, QWidget* parent);
}
