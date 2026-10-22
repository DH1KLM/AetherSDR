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
    : QGroupBox(tr("ASIO RX Output"), parent)
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
    auto* status = new QLabel(tr("Applies to RX speakers. Microphone, CW sidetone and other outputs use their existing devices."), this);
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
    connect(apply, &QPushButton::clicked, this, [engine, enabled, driver, channel, rate, buffer, status] {
        if (!engine) { return; }
        AsioOutputConfig config;
        config.enabled = enabled->isChecked();
        config.driver = driver->currentData().toString();
        config.firstChannel = channel->value() - 1;
        config.sampleRate = rate->currentData().toInt();
        config.bufferFrames = buffer->value();
        if (config.enabled && config.driver.isEmpty()) {
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
