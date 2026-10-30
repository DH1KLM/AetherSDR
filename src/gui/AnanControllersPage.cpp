#include "AnanControllersPage.h"
#include "GuardedSlider.h"
#include "SerialPortCombo.h"
#include "models/RadioModel.h"
#include "models/FrontPanelModel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include "core/backends/IRadioBackend.h"
#include <memory>
#include <QMap>
#include <algorithm>
#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif
namespace AetherSDR {
namespace {
QString actionLabel(const QString& id)
{
    if (id.startsWith(QLatin1String("unavailable:"))) {
        return QStringLiteral("Unavailable — %1").arg(id.mid(12));
    }
    static const QMap<QString, QString> labels{{"none", "No action"}, {"vfo", "Tune VFO"}, {"afGain", "AF gain"},
        {"agcT", "AGC-T"}, {"rfGain", "RF gain / attenuation"}, {"filterLow", "Filter low cut"}, {"filterHigh", "Filter high cut"},
        {"drive", "RF power"}, {"rit", "RIT"}, {"xit", "XIT"}, {"cwSpeed", "CW speed"}, {"monitorLevel", "Monitor level"},
        {"mox", "MOX toggle"}, {"pttHold", "PTT while held"}, {"tune", "TUNE toggle"}, {"mute", "Mute"}, {"lock", "VFO lock"},
        {"clearOffsets", "Clear RIT / XIT"}, {"receiver", "Select A / B"}, {"nb", "Noise blanker"}, {"nr", "Noise reduction"},
        {"anf", "Auto notch"}, {"agc", "Cycle AGC mode"}, {"modeUp", "Next mode"}, {"modeDown", "Previous mode"}, {"monitor", "MON"},
        {"filterReset", "Reset filter shift"}, {"filterUp", "Next filter preset"}, {"filterDown", "Previous filter preset"},
        {"bandUp", "Next band"}, {"bandDown", "Previous band"}, {"shift", "Band shift"}, {"ctun", "CTUN"},
        {"copyAtoB", "Copy VFO A to B"}, {"copyBtoA", "Copy VFO B to A"}, {"swapVfo", "Swap VFO A / B"},
        {"multifunction", "Multifunction encoder"}, {"multifunctionMenu", "Select / confirm multifunction"},
        {"modeMenu", "Mode selection"}, {"filterMenu", "Filter selection"}, {"bandMenu", "Band selection"}, {"stepMenu", "Tuning step"},
        {"gainMenu", "RX controls"}, {"setup", "Radio Setup"}, {"txSettings", "Transmit settings"},
        {"displayMenu", "Display settings"}, {"audioSettings", "Audio settings"}, {"equalizer", "Equalizer"},
        {"centerDisplay", "Center display"}, {"zoomIn", "Zoom in"}, {"zoomOut", "Zoom out"},
        {"recordAudio", "Record RX audio"}, {"playAudio", "Play RX recording locally"}, {"receiverMenu", "Receiver selection"}, {"receiverToggle", "Receiver B on/off"},
        {"bandstack", "Next bandstack bookmark"}, {"bandstackMenu", "Bandstack panel"},
        {"quickSave", "Save panel quick memory"}, {"quickRestore", "Recall panel quick memory"},
        {"audioPan", "RX stereo balance"}, {"micGain", "Microphone gain"}, {"compression", "Compressor level"},
        {"cwPitch", "CW pitch"}, {"displayPan", "Pan display"}, {"displayZoom", "Display zoom"},
        {"tuneStep", "Tuning step"}, {"masterGain", "Master RX volume"},
        {"attenuationStep", "Step attenuation"}, {"antennaStep", "Next RX antenna"}};
    if (id.startsWith(QLatin1String("softkey"))) { return QStringLiteral("Menu softkey %1").arg(id.mid(7)); }
    return labels.value(id, id);
}
}
QWidget* createAnanControllersPage(RadioModel* model, QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("ananControllerSettings"));
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto& panel = model->frontPanel();
    const auto initial = panel.state();
    auto* group = new QGroupBox(QStringLiteral("ANAN front panel"), page);
    group->setObjectName(QStringLiteral("ananFrontPanelGroup"));
    auto* form = new QFormLayout(group);
    auto* enabled = new QCheckBox(QStringLiteral("Enable front panel"));
    enabled->setAccessibleName(enabled->text()); enabled->setChecked(initial.value(QStringLiteral("enabled")).toBool());
    auto* kind = new GuardedComboBox;
    kind->setObjectName(QStringLiteral("ananFrontPanelType"));
    kind->setAccessibleName(QStringLiteral("Front panel type"));
    kind->addItem(QStringLiteral("Andromeda — USB / COM"), QStringLiteral("andromeda"));
    kind->addItem(QStringLiteral("G2-V2 — TCP CAT"), QStringLiteral("g2v2"));
    kind->setCurrentIndex(kind->findData(initial.value(QStringLiteral("panel"))));
    auto* serial = new GuardedComboBox;
    serial->setObjectName(QStringLiteral("ananPanelSerialPort"));
    serial->setMinimumWidth(200);
    serial->setAccessibleName(QStringLiteral("Andromeda COM port"));
    auto* customSerial = new QLineEdit;
    customSerial->setAccessibleName(QStringLiteral("Custom Andromeda serial port"));
    customSerial->setPlaceholderText(QStringLiteral("COM3 or /dev/ttyACM0"));
    auto* customSerialLabel = new QLabel(QStringLiteral("Path:"));
    auto* refreshSerial = new QPushButton(QStringLiteral("Refresh"));
    refreshSerial->setAccessibleName(QStringLiteral("Refresh Andromeda COM ports"));
    auto* serialRow = new QHBoxLayout;
    serialRow->setProperty("rsPacked", true);
    serialRow->addWidget(serial); serialRow->addWidget(refreshSerial);
    const auto serialVisibility = [=] {
        const bool custom = SerialPortCombo::isCustom(serial);
        customSerialLabel->setVisible(custom); customSerial->setVisible(custom);
    };
#ifdef HAVE_SERIALPORT
    SerialPortCombo::populate(serial, customSerial, initial.value(QStringLiteral("serialPort")).toString(), QSerialPortInfo::availablePorts());
    QObject::connect(refreshSerial, &QPushButton::clicked, page, [=] {
        SerialPortCombo::refresh(serial, customSerial, [] { return QSerialPortInfo::availablePorts(); });
        serialVisibility();
    });
#else
    serial->addItem(QStringLiteral("Custom..."), QStringLiteral("__custom__"));
    customSerial->setText(initial.value(QStringLiteral("serialPort")).toString());
#endif
    QObject::connect(serial, &QComboBox::currentIndexChanged, page, [serialVisibility](int) { serialVisibility(); });
    auto* listen = new QLineEdit(initial.value(QStringLiteral("listenAddress")).toString());
    listen->setAccessibleName(QStringLiteral("G2-V2 listen address"));
    auto* port = new QSpinBox; port->setRange(1, 65535); port->setValue(initial.value(QStringLiteral("tcpPort")).toInt());
    port->setAccessibleName(QStringLiteral("G2-V2 CAT listen port"));
    auto* peer = new QLineEdit(initial.value(QStringLiteral("peerAddress")).toString());
    peer->setAccessibleName(QStringLiteral("Expected G2-V2 panel IP"));
    peer->setPlaceholderText(QStringLiteral("Required for a LAN listener"));
    auto* connectionHint = new QLabel(QStringLiteral("Andromeda: 9600 baud, 8N1, no handshake. G2-V2 connects to this PC's CAT listener; configure the PC address and port on the panel. A LAN listener accepts only the configured peer IP."));
    connectionHint->setWordWrap(true);
    auto* status = new QLabel; status->setAccessibleName(QStringLiteral("Front panel connection status")); status->setWordWrap(true);
    auto* firmware = new QLabel;
    firmware->setObjectName(QStringLiteral("ananPanelFirmware"));
    firmware->setAccessibleName(QStringLiteral("Front panel firmware version"));
    form->addRow(enabled); form->addRow(QStringLiteral("Panel:"), kind);
    form->addRow(QStringLiteral("COM port:"), serialRow);
    form->addRow(customSerialLabel, customSerial); serialVisibility();
    form->addRow(QStringLiteral("Firmware:"), firmware);
    form->addRow(QStringLiteral("Listen address:"), listen);
    form->addRow(QStringLiteral("TCP port:"), port); form->addRow(QStringLiteral("Panel IP:"), peer); form->addRow(connectionHint);
    auto* control = new GuardedComboBox; control->setAccessibleName(QStringLiteral("Panel control to assign"));
    for (int n = 1; n <= 20; ++n) { control->addItem(QStringLiteral("Encoder %1").arg(n), QStringLiteral("e%1").arg(n)); }
    for (int n = 1; n <= 99; ++n) {
        control->addItem(QStringLiteral("Button %1 — short press").arg(n), QStringLiteral("p%1").arg(n));
        control->addItem(QStringLiteral("Button %1 — long press").arg(n), QStringLiteral("h%1").arg(n));
    }
    for (int n = 1; n <= 20; ++n) { control->addItem(QStringLiteral("LED %1").arg(n), QStringLiteral("l%1").arg(n)); }
    auto* action = new GuardedComboBox; action->setAccessibleName(QStringLiteral("Assigned panel action"));
    auto* target = new GuardedComboBox; target->setAccessibleName(QStringLiteral("Panel receiver target"));
    target->addItems({QStringLiteral("Active receiver"), QStringLiteral("Receiver A"), QStringLiteral("Receiver B")});
    auto* learn = new QCheckBox(QStringLiteral("Learn controls (suppress actions)")); learn->setAccessibleName(learn->text());
    auto* last = new QLabel; last->setAccessibleName(QStringLiteral("Last front panel input"));
    auto* menuState = new QLabel; menuState->setWordWrap(true);
    menuState->setAccessibleName(QStringLiteral("Andromeda menu and multifunction selection"));
    auto* map = new QPushButton(QStringLiteral("Assign control"));
    auto* apply = new QPushButton(QStringLiteral("Apply panel changes"));
    auto* standard = new QPushButton(QStringLiteral("Load standard Andromeda mapping"));
    standard->setObjectName(QStringLiteral("ananStandardMapping"));
    standard->setAccessibleName(standard->text());
    form->addRow(QStringLiteral("Control:"), control); form->addRow(QStringLiteral("Action:"), action);
    form->addRow(QStringLiteral("Receiver:"), target); form->addRow(learn); form->addRow(last); form->addRow(map, apply); form->addRow(status);
    form->addRow(standard);
    form->addRow(menuState);
    const auto bindings = std::make_shared<QJsonObject>(QJsonObject::fromVariantMap(initial.value(QStringLiteral("bindings")).toMap()));
    const auto selectControl = [=] {
        const QString key = control->currentData().toString();
        const QString category = key.startsWith(QLatin1Char('e')) ? QStringLiteral("encoder")
            : key.startsWith(QLatin1Char('l')) ? QStringLiteral("indicator") : QStringLiteral("button");
        const auto saved = bindings->value(key).toObject();
        const QSignalBlocker blocker(action); action->clear();
        for (const auto& id : FrontPanelModel::actions(category)) {
            action->addItem(actionLabel(id), id);
            const bool dspUnavailable = (id == QLatin1String("anf") || id == QLatin1String("nb")) && !model->backendCapabilities().hasRadioSideDsp;
            const bool offsetUnavailable = id == QLatin1String("rit") || id == QLatin1String("xit") || id == QLatin1String("clearOffsets");
            const bool mappingUnavailable = id.startsWith(QLatin1String("unavailable:"));
            if (dspUnavailable || offsetUnavailable || mappingUnavailable) {
                if (auto* items = qobject_cast<QStandardItemModel*>(action->model())) {
                    auto* item = items->item(action->count() - 1);
                    item->setEnabled(false);
                    item->setToolTip(mappingUnavailable ? QStringLiteral("This Thetis standard function has no Aether panel integration") : offsetUnavailable ? QStringLiteral("RIT/XIT panel dispatch is not integrated for extended ANAN")
                        : QStringLiteral("Radio-side DSP unavailable; host NR/ANF mapping is not integrated for this panel"));
                    item->setAccessibleDescription(item->toolTip());
                }
            }
        }
        action->setCurrentIndex(std::max(0, action->findData(saved.value(QStringLiteral("action")).toString())));
        target->setCurrentIndex(saved.value(QStringLiteral("target")).toInt());
    };
    QObject::connect(control, &QComboBox::currentIndexChanged, page, [selectControl](int) { selectControl(); });
    QObject::connect(standard, &QPushButton::clicked, page, [=] {
        *bindings = FrontPanelModel::standardAndromedaBindings();
        selectControl();
        status->setText(QStringLiteral("Standard mapping staged, including MOX on button 47 and TUNE on 48. Click Apply panel changes to save."));
    });
    QObject::connect(map, &QPushButton::clicked, page, [=] {
        bindings->insert(control->currentData().toString(), QJsonObject{{QStringLiteral("action"), action->currentData().toString()},
            {QStringLiteral("target"), target->currentIndex()}});
        status->setText(QStringLiteral("Assignment staged; click Apply panel changes to save."));
    });
    QObject::connect(learn, &QCheckBox::toggled, &panel, &FrontPanelModel::setLearning);
    QObject::connect(page, &QObject::destroyed, &panel, [&panel] { panel.setLearning(false); });
    QObject::connect(apply, &QPushButton::clicked, page, [=, &panel] {
        panel.configure({{QStringLiteral("enabled"), enabled->isChecked()}, {QStringLiteral("panel"), kind->currentData()},
            {QStringLiteral("serialPort"), SerialPortCombo::isCustom(serial) ? customSerial->text().trimmed() : serial->currentData().toString()}, {QStringLiteral("listenAddress"), listen->text()},
            {QStringLiteral("tcpPort"), port->value()}, {QStringLiteral("peerAddress"), peer->text()},
            {QStringLiteral("bindings"), bindings->toVariantMap()}});
    });
    const auto transportControls = [=] {
        const bool tcp = kind->currentData().toString() == QLatin1String("g2v2");
        standard->setEnabled(!tcp);
        standard->setAccessibleDescription(tcp ? QStringLiteral("This preset is for the USB Andromeda panel") : QStringLiteral("Stage Thetis standard controls; Apply saves the mapping including MOX and TUNE"));
        serial->setEnabled(!tcp); listen->setEnabled(tcp); port->setEnabled(tcp); peer->setEnabled(tcp);
        customSerial->setEnabled(!tcp); refreshSerial->setEnabled(!tcp);
        customSerial->setAccessibleDescription(tcp ? QStringLiteral("G2-V2 uses the TCP listener") : QStringLiteral("Custom USB serial port path"));
        refreshSerial->setAccessibleDescription(tcp ? QStringLiteral("G2-V2 uses the TCP listener") : QStringLiteral("Rescan available USB serial ports"));
        serial->setAccessibleDescription(tcp ? QStringLiteral("G2-V2 uses the TCP listener") : QStringLiteral("USB serial port, fixed 9600/8N1"));
        for (QWidget* widget : {static_cast<QWidget*>(listen), static_cast<QWidget*>(port), static_cast<QWidget*>(peer)}) {
            widget->setAccessibleDescription(tcp ? QStringLiteral("G2-V2 TCP CAT connection") : QStringLiteral("Andromeda uses USB serial"));
        }
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, page, [transportControls](int) { transportControls(); });
    const auto updateStatus = [=, &panel] {
        const auto state = panel.state();
        QStringList keys;
        const QStringList actions = state.value(QStringLiteral("softkeys")).toStringList();
        for (int i = 0; i < actions.size(); ++i) { keys.append(QStringLiteral("%1: %2").arg(i + 21).arg(actionLabel(actions.at(i)))); }
        menuState->setText(QStringLiteral("%1 menu — %2\nMulti: %3 (%4); Shift: %5")
            .arg(state.value(QStringLiteral("menu")).toString(), keys.join(QStringLiteral(" · ")),
                actionLabel(state.value(QStringLiteral("multifunction")).toString()),
                state.value(QStringLiteral("selectingMultifunction")).toBool() ? QStringLiteral("selecting; press 11 to confirm") : QStringLiteral("press 11 to select"),
                state.value(QStringLiteral("shift")).toBool() ? QStringLiteral("ON") : QStringLiteral("OFF")));
        const bool supported = state.value(QStringLiteral("supported")).toBool();
        group->setVisible(supported);
        status->setText(state.value(QStringLiteral("status")).toString());
        firmware->setText(state.value(QStringLiteral("identified")).toBool()
            ? QStringLiteral("h/w=%1  s/w=%2").arg(state.value(QStringLiteral("hardwareVersion")).toInt()).arg(state.value(QStringLiteral("firmwareVersion")).toInt())
            : QStringLiteral("Not identified"));
        const QString input = state.value(QStringLiteral("lastInput")).toString();
        last->setText(input);
        if (learn->isChecked() && !input.isEmpty()) {
            const QString key = state.value(QStringLiteral("lastControl")).toString();
            const int index = control->findData(key); if (index >= 0) { control->setCurrentIndex(index); }
        }
    };
    QObject::connect(&panel, &FrontPanelModel::stateChanged, page, updateStatus);
    QObject::connect(model, &RadioModel::capabilitiesChanged, page, [updateStatus](bool, const RadioCapabilities&) { updateStatus(); });
    selectControl(); transportControls(); updateStatus(); outer->addWidget(group);

    auto* vitals = new QGroupBox(QStringLiteral("ANAN voltage / current sensors"), page);
    vitals->setObjectName(QStringLiteral("ananVitalSensorsGroup"));
    auto* sensorForm = new QFormLayout(vitals);
    auto* diy = new QCheckBox(QStringLiteral("Enable voltage / current sensors for DIY hardware")); diy->setAccessibleName(diy->text());
    diy->setObjectName(QStringLiteral("ananDiyVitalSensors"));
    auto* offset = new QDoubleSpinBox; offset->setRange(0, 5000); offset->setDecimals(3); offset->setSuffix(QStringLiteral(" mV")); offset->setAccessibleName(QStringLiteral("Current sensor zero offset"));
    auto* sensitivity = new QDoubleSpinBox; sensitivity->setRange(0.001, 5000); sensitivity->setDecimals(3); sensitivity->setSuffix(QStringLiteral(" mV/A")); sensitivity->setAccessibleName(QStringLiteral("Current sensor sensitivity"));
    auto* reference = new QDoubleSpinBox; reference->setRange(0.1, 10); reference->setDecimals(3);
    reference->setSuffix(QStringLiteral(" V")); reference->setAccessibleName(QStringLiteral("Slow ADC reference voltage"));
    auto* divider = new QDoubleSpinBox; divider->setRange(0.01, 1000); divider->setDecimals(6);
    divider->setAccessibleName(QStringLiteral("Voltage divider multiplier"));
    auto* sensorSave = new QPushButton(QStringLiteral("Apply sensor settings"));
    sensorSave->setObjectName(QStringLiteral("ananVitalSensorsApply"));
    auto* sensorStatus = new QLabel; sensorStatus->setWordWrap(true); sensorStatus->setAccessibleName(QStringLiteral("Voltage current sensor status"));
    sensorForm->addRow(diy); sensorForm->addRow(QStringLiteral("Zero-current offset:"), offset);
    sensorForm->addRow(QStringLiteral("Sensitivity:"), sensitivity);
    sensorForm->addRow(QStringLiteral("ADC reference:"), reference);
    sensorForm->addRow(QStringLiteral("Voltage divider:"), divider); sensorForm->addRow(sensorSave); sensorForm->addRow(sensorStatus);
    struct SensorState { QString identity; bool dirty = false; bool filling = false; bool busy = false; };
    const auto sensor = std::make_shared<SensorState>();
    const auto request = [=](bool write) {
        if (sensor->busy || !model->isConnected() || !model->backendDeclaresExtension(QStringLiteral("anan"))) { return; }
        static quint64 next = 0x564954000000ULL;
        const quint64 id = ++next;
        const QPointer<IRadioBackend> backend = model->backend();
        auto* guard = new QObject(vitals); sensor->busy = true;
        QObject::connect(backend, &IRadioBackend::extensionResult, guard, [=](quint64 reply, const QVariant& result) {
            if (reply != id) { return; } sensor->busy = false; guard->deleteLater();
            if (!backend || model->backend() != backend) { return; }
            const auto state = result.toMap(); const QString identity = state.value(QStringLiteral("identity")).toString();
            if (write || sensor->identity != identity) { sensor->dirty = false; }
            sensor->identity = identity;
            if (!sensor->dirty) {
                const auto values = state.value(QStringLiteral("options")).toMap(); sensor->filling = true;
                diy->setChecked(values.value(QStringLiteral("diy")).toBool()); offset->setValue(values.value(QStringLiteral("offsetMv")).toDouble());
                sensitivity->setValue(values.value(QStringLiteral("sensitivityMvPerAmp")).toDouble());
                reference->setValue(values.value(QStringLiteral("referenceVolts")).toDouble());
                divider->setValue(values.value(QStringLiteral("voltageDivider")).toDouble()); sensor->filling = false;
            }
            sensorStatus->setText(state.value(QStringLiteral("native")).toBool()
                ? QStringLiteral("Native sensor model: voltage/current shown in the footer. Thetis sensor defaults; verify against your hardware.")
                : state.value(QStringLiteral("enabled")).toBool() ? QStringLiteral("DIY sensors enabled. AIN3 is voltage, AIN4 is current; verify the configured ADC reference, divider and current-sensor scale against the wiring.")
                : QStringLiteral("No native voltage/current sensors for this model. Enable DIY only for matching AIN3/AIN4 hardware."));
        });
        QObject::connect(backend, &IRadioBackend::extensionError, guard, [=](quint64 reply, const QString& error) {
            if (reply != id) { return; } sensor->busy = false; sensorStatus->setText(error); guard->deleteLater();
        });
        QTimer::singleShot(2000, guard, [=] { sensor->busy = false; guard->deleteLater(); });
        model->invokeBackendExtension(QStringLiteral("anan"), write ? QStringLiteral("vitals.set") : QStringLiteral("vitals.get"), id,
            write ? QVariantMap{{QStringLiteral("identity"), sensor->identity}, {QStringLiteral("options"), QVariantMap{
                {QStringLiteral("diy"), diy->isChecked()}, {QStringLiteral("offsetMv"), offset->value()},
                {QStringLiteral("sensitivityMvPerAmp"), sensitivity->value()},
                {QStringLiteral("referenceVolts"), reference->value()}, {QStringLiteral("voltageDivider"), divider->value()}}}} : QVariantMap{});
    };
    const auto dirty = [=] { if (!sensor->filling) { sensor->dirty = true; } };
    QObject::connect(diy, &QCheckBox::toggled, page, [dirty](bool) { dirty(); });
    QObject::connect(offset, &QDoubleSpinBox::valueChanged, page, [dirty](double) { dirty(); });
    QObject::connect(sensitivity, &QDoubleSpinBox::valueChanged, page, [dirty](double) { dirty(); });
    QObject::connect(reference, &QDoubleSpinBox::valueChanged, page, [dirty](double) { dirty(); });
    QObject::connect(divider, &QDoubleSpinBox::valueChanged, page, [dirty](double) { dirty(); });
    QObject::connect(sensorSave, &QPushButton::clicked, page, [request] { request(true); });
    const auto sensorAvailability = [=] {
        const bool supported = model->backendDeclaresExtension(QStringLiteral("anan"));
        vitals->setVisible(supported); vitals->setEnabled(model->isConnected());
        vitals->setAccessibleDescription(model->isConnected() ? QStringLiteral("Voltage and current sensor conversion") : QStringLiteral("Connect an ANAN to edit its sensor configuration"));
    };
    QObject::connect(model, &RadioModel::capabilitiesChanged, page, [sensorAvailability, request](bool, const RadioCapabilities&) { sensorAvailability(); request(false); });
    auto* timer = new QTimer(page); timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, page, [page, learn, request] {
        if (!page->isVisible() && learn->isChecked()) { learn->setChecked(false); }
        if (page->isVisible()) { request(false); }
    }); timer->start();
    sensorAvailability(); request(false); outer->addWidget(vitals);
    return page;
}
} // namespace AetherSDR
