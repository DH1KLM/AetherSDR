#include "AsioOutputSettingsWidget.h"
#include "core/AudioEngine.h"
#include "core/AsioOutput.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>

namespace AetherSDR {
AsioOutputSettingsWidget::AsioOutputSettingsWidget(AudioEngine* audio, QWidget* parent)
    : QGroupBox(tr("ASIO RX Output / Microphone"), parent)
{
    const AsioOutputConfig saved = AsioOutputConfig::load();
    auto* form = new QFormLayout(this);
    auto* enabled = new QCheckBox(tr("Use ASIO for PC receive audio"), this);
    enabled->setAccessibleName(tr("Use ASIO for PC receive audio"));
    enabled->setChecked(saved.enabled);
    form->addRow(enabled);
    auto* hint = new QLabel(tr("To enable or disable ASIO, change the checkbox and click Apply ASIO changes. Other edits also require Apply."), this);
    hint->setWordWrap(true);
    hint->setAccessibleName(hint->text());
    enabled->setAccessibleDescription(hint->text());
    form->addRow(hint);
    auto* driver = new GuardedComboBox(this);
    applyComboStyle(driver);
    driver->setAccessibleName(tr("ASIO driver"));
    for (const AsioOutputDriver& item : AsioOutput::drivers()) { driver->addItem(item.name, item.id); }
    if (driver->findData(saved.driver) < 0 && !saved.driver.isEmpty()) {
        driver->addItem(tr("Unavailable: %1").arg(saved.driver), saved.driver);
    }
    driver->setCurrentIndex(driver->findData(saved.driver));
    form->addRow(tr("Driver:"), driver);
    auto* channel = new QSpinBox(this);
    channel->setRange(1, 255);
    channel->setValue(saved.firstChannel + 1);
    channel->setAccessibleName(tr("First ASIO output channel"));
    channel->setAccessibleDescription(tr("One-based first channel of two consecutive stereo output channels."));
    form->addRow(tr("First stereo channel:"), channel);
    auto* microphone = new QCheckBox(tr("Use ASIO microphone input"), this);
    microphone->setAccessibleName(tr("Use ASIO microphone input"));
    microphone->setChecked(saved.microphoneEnabled);
    form->addRow(microphone);
    auto* inputChannel = new QSpinBox(this);
    inputChannel->setRange(1, 255);
    inputChannel->setValue(saved.firstInputChannel + 1);
    inputChannel->setAccessibleName(tr("First ASIO microphone input channel"));
    form->addRow(tr("First input channel:"), inputChannel);
    auto* inputMode = new GuardedComboBox(this);
    applyComboStyle(inputMode);
    inputMode->setAccessibleName(tr("ASIO microphone channel selection"));
    inputMode->addItem(tr("Left / first channel"), 0);
    inputMode->addItem(tr("Right / next channel"), 1);
    inputMode->addItem(tr("Both channels mixed"), 2);
    inputMode->setCurrentIndex(saved.inputMode);
    form->addRow(tr("Microphone source:"), inputMode);
    auto* rate = new GuardedComboBox(this);
    applyComboStyle(rate);
    rate->setAccessibleName(tr("ASIO sample rate"));
    for (int hz : {44100, 48000, 96000, 192000}) { rate->addItem(QString::number(hz) + tr(" Hz"), hz); }
    rate->setCurrentIndex(rate->findData(saved.sampleRate));
    form->addRow(tr("Sample rate:"), rate);
    auto* buffer = new QSpinBox(this);
    buffer->setRange(0, 8192);
    buffer->setSpecialValueText(tr("Driver preferred"));
    buffer->setValue(saved.bufferFrames);
    buffer->setAccessibleName(tr("ASIO buffer frames"));
    form->addRow(tr("Buffer frames:"), buffer);
    auto* status = new QLabel(tr("RX and microphone share this driver, sample rate and buffer. Apply changes while receiving. CW sidetone and other outputs retain their devices."), this);
    status->setWordWrap(true);
    status->setAccessibleName(tr("ASIO output status"));
#ifdef Q_OS_WIN
    if (driver->count() == 0) { status->setText(tr("No 64-bit ASIO driver is installed.")); }
#endif
    form->addRow(status);
    auto* apply = new QPushButton(tr("Apply ASIO changes"), this);
    apply->setAutoDefault(false);
    apply->setAccessibleName(tr("Apply ASIO changes"));
    form->addRow(apply);
    const QPointer<AudioEngine> engine(audio);
    connect(apply, &QPushButton::clicked, this, [engine, enabled, driver, channel, rate, buffer, status, microphone, inputChannel, inputMode] {
        if (!engine) { return; }
        AsioOutputConfig config;
        config.enabled = enabled->isChecked();
        config.driver = driver->currentData().toString();
        config.firstChannel = channel->value() - 1;
        config.sampleRate = rate->currentData().toInt();
        config.bufferFrames = buffer->value();
        config.microphoneEnabled = microphone->isChecked();
        config.firstInputChannel = inputChannel->value() - 1;
        config.inputMode = inputMode->currentData().toInt();
        if ((config.enabled || config.microphoneEnabled) && config.driver.isEmpty()) {
            status->setText(tr("Select an installed 64-bit ASIO driver first."));
            return;
        }
        status->setText(tr("Applying ASIO output settings…"));
        QMetaObject::invokeMethod(engine, [engine, config] {
            if (engine) { engine->setAsioOutputConfig(config); }
        }, Qt::QueuedConnection);
    });
    if (engine) {
        connect(engine, &AudioEngine::asioOutputStatus, status, &QLabel::setText);
    }
#ifndef Q_OS_WIN
    const QString reason = tr("ASIO output is available on Windows only.");
    setEnabled(false);
    setAccessibleDescription(reason);
    setToolTip(reason);
    status->setText(reason);
#endif
}
}
