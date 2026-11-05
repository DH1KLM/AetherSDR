#include "PureSignalWindow.h"

#include "core/ThemeManager.h"
#include "core/TxKeyingMarker.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <memory>

namespace AetherSDR {
namespace {
const std::array<QString, 4> kSettingNames{QStringLiteral("moxWaitSeconds"),
    QStringLiteral("calibrationWaitSeconds"), QStringLiteral("amplifierDelayNs"), QStringLiteral("hardwarePeak")};

void unavailable(QWidget* widget, const QString& reason)
{
    widget->setEnabled(false);
    widget->setToolTip(reason);
    widget->setAccessibleDescription(reason);
}

void onTop(CanonWindow* window, bool enabled)
{
    const QRect geometry = window->geometry();
    window->setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    window->setGeometry(geometry);
    window->show();
}
} // namespace

PureSignalWindow::PureSignalWindow(RadioModel& model, QWidget* parent)
    : CanonWindow(QStringLiteral("Pure Signal"), parent, Kind::Workspace), m_model(model)
{
    setObjectName(QStringLiteral("PureSignalWindow"));
    setGeometryKey(QStringLiteral("PureSignalWindowGeometry"));
    ThemeManager::instance().applyStyleSheet(bodyWidget(),QStringLiteral(
        "QPushButton:disabled, QDoubleSpinBox:disabled, QCheckBox:disabled { color: {{color.text.disabled}}; }"));
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->addWidget(makeCanonHeader(QStringLiteral("Pure Signal")));
    auto* actions = new QHBoxLayout;
    layout->addLayout(actions);
    const auto button = [this, actions](const QString& text, const QString& name) {
        auto* result = new QPushButton(text, bodyWidget());
        result->setObjectName(name); result->setAccessibleName(text);
        result->setAutoDefault(false); actions->addWidget(result);
        return result;
    };
    m_twoTone = button(QStringLiteral("Two-Tone"), QStringLiteral("pureSignalTwoTone"));
    markTxKeying(m_twoTone);
    unavailable(m_twoTone, QStringLiteral("Prepare an SSB TX session with both 700 and 1900 Hz inside the TX filter."));
    m_single = button(QStringLiteral("Single Cal"), QStringLiteral("pureSignalSingleCal"));
    m_auto = button(QStringLiteral("Start Auto"), QStringLiteral("pureSignalStartAuto"));
    m_apply = button(QStringLiteral("Apply Current"), QStringLiteral("pureSignalApplyCurrent"));
    auto* amp = button(QStringLiteral("AmpView"), QStringLiteral("pureSignalAmpView"));
    auto* advanced = button(QStringLiteral("Advanced"), QStringLiteral("pureSignalAdvanced"));
    advanced->setCheckable(true); advanced->setChecked(true);
    m_save = button(QStringLiteral("Save"), QStringLiteral("pureSignalSave"));
    m_restore = button(QStringLiteral("Restore"), QStringLiteral("pureSignalRestore"));
    m_off = button(QStringLiteral("OFF"), QStringLiteral("pureSignalOff"));
    m_views = new QTabWidget(bodyWidget());
    m_views->setObjectName(QStringLiteral("pureSignalViews"));
    m_views->setAccessibleName(QStringLiteral("Pure Signal view"));
    layout->addWidget(m_views);
    auto* calibration = new QWidget(m_views);
    m_views->addTab(calibration,QStringLiteral("Calibration"));
    layout = new QVBoxLayout(calibration);
    layout->setAlignment(Qt::AlignTop);

    auto* indicators = new QHBoxLayout;
    m_feedback = new QLabel(QStringLiteral("Feedback Level: —"), bodyWidget());
    m_feedback->setObjectName(QStringLiteral("pureSignalFeedbackLevel"));
    m_correcting = new QLabel(QStringLiteral("Correction: OFF"), bodyWidget());
    m_correcting->setObjectName(QStringLiteral("pureSignalCorrecting"));
    indicators->addWidget(m_feedback); indicators->addWidget(m_correcting); indicators->addStretch();
    auto* measurements = new QCheckBox(QStringLiteral("Show 2Tone measurements"), bodyWidget());
    unavailable(measurements, QStringLiteral("A measured PA-feedback spectrum is not available to the spectrum display."));
    indicators->addWidget(measurements); layout->addLayout(indicators);
    m_actionStatus = new QLabel(QStringLiteral("No Pure Signal action pending."),calibration);
    m_actionStatus->setObjectName(QStringLiteral("pureSignalActionStatus"));
    m_actionStatus->setWordWrap(true); layout->addWidget(m_actionStatus);
    m_status = new QLabel(QStringLiteral("Reading Pure Signal state…"), bodyWidget());
    m_status->setObjectName(QStringLiteral("pureSignalStatus")); m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_routing = new QLabel(bodyWidget()); m_routing->setWordWrap(true);
    m_routing->setObjectName(QStringLiteral("pureSignalRouting")); layout->addWidget(m_routing);
    auto* settings = new QHBoxLayout;
    auto* form = new QFormLayout; settings->addLayout(form);
    const std::array<QString,4> labels{QStringLiteral("MOX Wait (sec)"), QStringLiteral("CAL Wait (sec)"),
        QStringLiteral("AMP Delay (ns)"), QStringLiteral("SetPk")};
    const std::array<double,4> maxima{5,5,1000000,1};
    const std::array<double,4> defaults{0.2,0,150,0.4072};
    for (std::size_t i = 0; i < m_settings.size(); ++i) {
        auto* spin = new QDoubleSpinBox(bodyWidget());
        spin->setObjectName(QStringLiteral("pureSignal_%1").arg(kSettingNames[i]));
        spin->setAccessibleName(labels[i]); spin->setDecimals(i == 2 ? 0 : i == 3 ? 4 : 2);
        spin->setRange(i == 3 ? 0.01 : 0, maxima[i]); spin->setValue(defaults[i]);
        spin->setSingleStep(i == 2 ? 10 : i == 3 ? 0.0001 : 0.1);
        m_settings[i] = spin; form->addRow(labels[i], spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { if (!m_filling) { m_dirty = true; } });
    }
    auto* options = new QVBoxLayout; settings->addLayout(options);
    m_autoAttenuate = new QCheckBox(QStringLiteral("Auto-Attenuate"),bodyWidget());
    m_autoAttenuate->setObjectName(QStringLiteral("pureSignalAutoAttenuate"));
    m_autoAttenuate->setAccessibleName(m_autoAttenuate->text());
    m_autoAttenuate->setToolTip(QStringLiteral("Adjust feedback attenuation after each valid calibration; pause and settle before collecting again."));
    connect(m_autoAttenuate,&QCheckBox::toggled,this,[this] { if (!m_filling) { m_dirty = true; } });
    options->addWidget(m_autoAttenuate);
    for (const QString& name : {QStringLiteral("Quick Attenuate Response"),
                               QStringLiteral("Display PS-RX and PS-TX spectra")}) {
        auto* control = new QCheckBox(name, bodyWidget());
        control->setAccessibleName(name);
        unavailable(control, QStringLiteral("This feedback-control path is not available in this build."));
        options->addWidget(control);
    }
    auto* top = new QCheckBox(QStringLiteral("Always On Top"), bodyWidget());
    top->setAccessibleName(top->text()); options->addWidget(top);
    connect(top, &QCheckBox::toggled, this, [this](bool enabled) { onTop(this, enabled); });
    layout->addLayout(settings);
    auto* information = new QGroupBox(QStringLiteral("Calibration Information"), bodyWidget());
    information->setObjectName(QStringLiteral("pureSignalCalibrationInformation"));
    auto* grid = new QGridLayout(information);
    const std::array<QString,16> names{QStringLiteral("bldr.rx"), QStringLiteral("bldr.cm"),
        QStringLiteral("bldr.cc"), QStringLiteral("bldr.cs"), QStringLiteral("feedbk"), QStringLiteral("cal.cnt"),
        QStringLiteral("sln.chk"), QStringLiteral("attempts"), QStringLiteral("raw[8]"), QStringLiteral("raw[9]"),
        QStringLiteral("raw[10]"), QStringLiteral("raw[11]"), QStringLiteral("file.error"), QStringLiteral("raw[13]"),
        QStringLiteral("correction.run"), QStringLiteral("state")};
    for (int i = 0; i < 16; ++i) {
        const int column = i / 4 * 2, row = i % 4;
        grid->addWidget(new QLabel(names[i], information), row, column);
        auto* value = new QLabel(QStringLiteral("—"), information);
        value->setObjectName(QStringLiteral("pureSignalInfo%1").arg(i)); value->setAccessibleName(names[i]);
        value->setMinimumWidth(58); value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_information[i] = value; grid->addWidget(value, row, column+1);
    }
    m_peak = new QLabel(QStringLiteral("GetPk: —"), information);
    m_peak->setObjectName(QStringLiteral("pureSignalGetPeak")); grid->addWidget(m_peak,4,0,1,8);
    layout->addWidget(information);
    connect(advanced, &QPushButton::toggled, information, &QWidget::setVisible);
    connect(amp, &QPushButton::clicked, this, &PureSignalWindow::showAmpView);
    connect(m_single, &QPushButton::clicked, this, [this] { request(true,2); });
    connect(m_auto, &QPushButton::clicked, this, [this] { request(true,1); });
    connect(m_off, &QPushButton::clicked, this, [this] { request(true,0); });
    connect(m_apply, &QPushButton::clicked, this, [this] { request(true,3); });
    connect(m_save, &QPushButton::clicked, this, [this] { chooseCorrectionFile(true); });
    connect(m_restore, &QPushButton::clicked, this, [this] { chooseCorrectionFile(false); });
    connect(m_twoTone, &QPushButton::clicked, this, [this] {
        toggleTwoTone(m_model.localTxController()->capture(TxController::Activity::Tune));
    });
    registerTxKeyingAction(m_twoTone, [this](const std::shared_ptr<TxController>& controller,
        const QString& action, const QString&) -> TxKeyingAction::Prepared {
        if (!controller || !controller->belongsTo(&m_model) || action != QLatin1String("click")) { return {}; }
        const TxController::Input input = controller->capture(TxController::Activity::Tune);
        return [this, input] { toggleTwoTone(input); };
    });
    m_poll = new QTimer(this); m_poll->setInterval(250);
    connect(m_poll, &QTimer::timeout, this, [this] { if (isVisible() || (m_ampView && m_ampView->isVisible())) { request(); } });
    m_poll->start();
    setAvailability(false, QStringLiteral("Waiting for the connected radio's Pure Signal state."));
    resize(1120, 740); request();
}

void PureSignalWindow::toggleTwoTone(const TxController::Input& input)
{
    if (m_twoToneInput.active()) { stopTwoTone(); return; }
    if (!input.belongsTo(&m_model) || m_model.transmitModel().isTuning()
        || m_model.transmitModel().isTransmitting()) { return; }
    if (input.start(true)) { m_twoToneInput = input; }
}

void PureSignalWindow::stopTwoTone()
{
    if (m_twoToneInput.active()) {
        m_twoToneInput.stop();
        m_model.transmitModel().setTuneMode(QStringLiteral("single_tone"));
    }
    m_twoToneInput = {};
}

void PureSignalWindow::closeEvent(QCloseEvent* event)
{
    stopTwoTone();
    CanonWindow::closeEvent(event);
}

void PureSignalWindow::retire()
{
    stopTwoTone();
    m_twoTone->setEnabled(false);
    m_identity.clear(); m_state.clear(); m_dirty = false;
    m_actionStatus->setText(QStringLiteral("No Pure Signal action pending."));
    for (QLabel* value : m_information) { value->setText(QStringLiteral("—")); }
    m_feedback->setText(QStringLiteral("Feedback Level: —"));
    m_correcting->setText(QStringLiteral("Correction: OFF"));
    m_peak->setText(QStringLiteral("GetPk: —"));
    m_status->setText(QStringLiteral("Connect an OpenHPSDR device first."));
    m_routing->clear();
    setAvailability(false, QStringLiteral("Connect an OpenHPSDR device first."));
    if (m_scene) { m_scene->clear(); }
    close();
}

void PureSignalWindow::setAvailability(bool available, const QString& reason)
{
    for (QWidget* widget : std::array<QWidget*,6>{m_single, m_auto, m_off, m_apply, m_save, m_restore}) {
        widget->setEnabled(available); widget->setAccessibleDescription(reason); widget->setToolTip(reason);
    }
    m_autoAttenuate->setEnabled(available);
    m_autoAttenuate->setAccessibleDescription(reason);
    m_autoAttenuate->setToolTip(available
        ? QStringLiteral("Adjust feedback attenuation after each valid calibration; pause and settle before collecting again.")
        : reason);
    for (QDoubleSpinBox* spin : m_settings) {
        spin->setEnabled(available); spin->setAccessibleDescription(reason);
    }
}

void PureSignalWindow::chooseCorrectionFile(bool save)
{
    const QString identity = m_identity;
    const QPointer<PureSignalWindow> guard(this);
    const QString filter = QStringLiteral("WDSP corrections (*.pscorr *.txt);;All files (*)");
    const QString path = save ? QFileDialog::getSaveFileName(this,QStringLiteral("Save Pure Signal correction"),{},filter)
        : QFileDialog::getOpenFileName(this,QStringLiteral("Restore Pure Signal correction"),{},filter);
    if (!guard || path.isEmpty()) { return; }
    if (identity != m_identity || !m_model.isConnected()) {
        m_actionStatus->setText(QStringLiteral("The connection changed while choosing the correction file."));
        return;
    }
    request(true,3,save ? QStringLiteral("puresignal.save") : QStringLiteral("puresignal.restore"),path);
}

void PureSignalWindow::request(bool write, int mode, const QString& operation, const QString& path)
{
    if (m_pending || !m_model.isConnected() || !m_model.backendDeclaresExtension(QStringLiteral("anan"))) { return; }
    QPointer<IRadioBackend> backend = m_model.backend();
    if (!backend) { return; }
    static quint64 sequence = 0x505300000000ULL;
    const quint64 id = ++sequence;
    auto* guard = new QTimer(this); guard->setSingleShot(true); guard->start(write ? 120000 : 2000);
    m_pending = true;
    const auto finish = [this, guard] { m_pending = false; guard->stop(); guard->deleteLater(); };
    connect(guard, &QTimer::timeout, this, [this, finish] {
        finish(); m_status->setText(QStringLiteral("No Pure Signal reply; waiting for the session."));
        m_actionStatus->setText(m_status->text());
        setAvailability(false, m_status->text());
    });
    connect(backend, &IRadioBackend::extensionError, guard, [this, id, guard, finish](quint64 reply, const QString& error) {
        if (reply != id || !guard->isActive()) { return; }
        finish(); m_actionStatus->setText(error);
    });
    connect(backend, &IRadioBackend::extensionResult, guard,
        [this, backend, id, guard, finish, write, operation, mode](quint64 reply, const QVariant& result) {
            if (reply != id || !guard->isActive()) { return; }
            finish();
            if (!backend || backend != m_model.backend() || !m_model.isConnected()) { return; }
            if (write) {
                if (operation != QStringLiteral("puresignal.save")) { m_dirty = false; }
                m_actionStatus->setText(operation == QStringLiteral("puresignal.save") ? QStringLiteral("Correction saved.")
                    : operation == QStringLiteral("puresignal.restore") ? QStringLiteral("Correction restored; no transmission started.")
                    : mode == 3 ? QStringLiteral("Current correction selected; no transmission started.")
                    : QStringLiteral("Pure Signal configuration applied."));
            }
            acceptState(result.toMap());
        });
    QVariantMap fields;
    if (write) {
        fields.insert(QStringLiteral("identity"), m_identity);
        if (operation != QStringLiteral("puresignal.save")) {
            fields.insert(QStringLiteral("mode"), mode);
            fields.insert(QStringLiteral("autoAttenuate"),m_autoAttenuate->isChecked());
            for (std::size_t i = 0; i < m_settings.size(); ++i) { fields.insert(kSettingNames[i], m_settings[i]->value()); }
        }
        if (!path.isEmpty()) { fields.insert(QStringLiteral("path"), path); }
        m_actionStatus->setText(QStringLiteral("Pure Signal operation pending…"));
        setAvailability(false, QStringLiteral("Applying Pure Signal configuration."));
    } else { fields.insert(QStringLiteral("display"), m_ampView && m_ampView->isVisible()); }
    m_model.invokeBackendExtension(QStringLiteral("anan"), !operation.isEmpty() ? operation
        : write ? QStringLiteral("puresignal.set") : QStringLiteral("puresignal.get"), id, fields);
}

void PureSignalWindow::acceptState(const QVariantMap& state)
{
    const QString identity = state.value(QStringLiteral("identity")).toString();
    const bool changed = identity != m_identity;
    m_identity = identity; m_state = state;
    const bool available = state.value(QStringLiteral("available")).toBool();
    const QString reason = state.value(QStringLiteral("reason")).toString();
    const bool keyed = state.value(QStringLiteral("keyed")).toBool();
    const bool ownTone = m_twoToneInput.active();
    const bool toneReady = available && !keyed && m_model.backendCapabilities().twoToneGenerator.has_value();
    m_twoTone->setEnabled(ownTone || toneReady);
    m_twoTone->setText(ownTone ? QStringLiteral("Stop Two-Tone") : QStringLiteral("Two-Tone"));
    m_twoTone->setAccessibleName(m_twoTone->text());
    const QString toneReason = ownTone ? QStringLiteral("Stop this window's two-tone transmission.")
        : toneReady ? QStringLiteral("Transmit 700/1900 Hz through TXA at the configured TUNE power.")
        : !available ? reason : QStringLiteral("Prepare an SSB TX session with both 700 and 1900 Hz inside the TX filter.");
    m_twoTone->setToolTip(toneReason); m_twoTone->setAccessibleDescription(toneReason);
    setAvailability(available && !keyed, keyed
        ? QStringLiteral("Finish transmission before changing Pure Signal.") : reason);
    if (available && !keyed && !state.value(QStringLiteral("correctionAvailable")).toBool()) {
        unavailable(m_apply,QStringLiteral("No retained correction is available. Calibrate or restore a correction first."));
        unavailable(m_save,QStringLiteral("No correction coefficients are available to save."));
    }
    m_filling = true;
    if (changed || !m_dirty) {
        m_autoAttenuate->setChecked(state.value(QStringLiteral("autoAttenuate")).toBool());
        for (std::size_t i = 0; i < m_settings.size(); ++i) {
            if (!m_settings[i]->hasFocus()) { m_settings[i]->setValue(state.value(kSettingNames[i]).toDouble()); }
        }
    }
    m_filling = false;
    const QVariantList info = state.value(QStringLiteral("information")).toList();
    for (int i = 0; i < 16; ++i) { m_information[i]->setText(info.size() == 16 ? info[i].toString() : QStringLiteral("—")); }
    const int mode = state.value(QStringLiteral("mode")).toInt();
    const bool applied = state.value(QStringLiteral("correcting")).toBool();
    const int level = info.size() == 16 ? info[4].toInt() : 0;
    m_feedback->setText(QStringLiteral("Feedback Level: %1").arg(info.size() == 16 ? QString::number(level) : QStringLiteral("—")));
    m_correcting->setText(!applied ? state.value(QStringLiteral("correctionBusy")).toBool()
        ? QStringLiteral("Correction transition pending")
        : state.value(QStringLiteral("correctionAvailable")).toBool()
            ? QStringLiteral("Correction retained • not applied") : QStringLiteral("Correction: OFF")
        : level > 181 ? QStringLiteral("Correction applied • feedback high")
        : level > 128 ? QStringLiteral("Correction applied • feedback in range")
                      : QStringLiteral("Correction applied • feedback low"));
    m_status->setText(!available ? reason : mode == 0 ? QStringLiteral("Pure Signal OFF. Opening this window does not transmit.")
        : mode == 1 ? QStringLiteral("Automatic calibration armed; waiting for operator transmission and feedback.")
        : mode == 3 ? QStringLiteral("Retained correction selected; waiting for operator transmission.")
                    : QStringLiteral("Single calibration armed; waiting for operator transmission and feedback."));
    m_routing->setText(state.value(QStringLiteral("routing")).toString()
        + QStringLiteral(" Feedback attenuation request: %1 dB%2.")
            .arg(state.value(QStringLiteral("feedbackAttenuationDb"),31).toInt())
            .arg(state.value(QStringLiteral("autoAttenuationPending")).toBool()
                ? QStringLiteral("; waiting for pause/settling") : QString()));
    if (m_dirty) { m_status->setText(QStringLiteral("Configuration changes pending. Press Single Cal or Start Auto to apply.")); }
    m_peak->setText(QStringLiteral("GetPk: %1").arg(state.value(QStringLiteral("maximumTransmit")).toDouble(),0,'f',4));
    if (m_ampView && m_ampView->isVisible()) { renderAmpView(); }
}

void PureSignalWindow::showAmpView()
{
    if (!m_ampView) {
        auto* window = new QWidget(m_views);
        window->setObjectName(QStringLiteral("PureSignalAmpView"));
        auto* layout = new QVBoxLayout(window);
        auto* view = new QGraphicsView(window);
        view->setAccessibleName(QStringLiteral("Pure Signal amplifier magnitude and phase curves"));
        view->setAccessibleDescription(QStringLiteral("Measured magnitude and phase samples and correction curves versus normalized input magnitude."));
        view->setRenderHint(QPainter::Antialiasing);
        m_scene = new QGraphicsScene(view); view->setScene(m_scene); layout->addWidget(view);
        auto* controls = new QHBoxLayout; layout->addLayout(controls);
        m_gain = new QCheckBox(QStringLiteral("Show Gain"),window);
        m_phaseZoom = new QCheckBox(QStringLiteral("Phase Zoom"),window); m_phaseZoom->setChecked(true);
        m_lowRes = new QCheckBox(QStringLiteral("Low Res"),window); m_lowRes->setChecked(true);
        auto* top = new QCheckBox(QStringLiteral("On Top"),window);
        for (QCheckBox* control : {m_gain,m_phaseZoom,m_lowRes,top}) {
            control->setAccessibleName(control->text()); controls->addWidget(control);
        }
        for (QCheckBox* control : {m_gain,m_phaseZoom,m_lowRes}) {
            connect(control,&QCheckBox::toggled,this,&PureSignalWindow::renderAmpView);
        }
        connect(top,&QCheckBox::toggled,this,[this](bool enabled) { onTop(this,enabled); });
        m_ampView = window;
        m_views->addTab(window,QStringLiteral("AmpView"));
    }
    m_views->setCurrentWidget(m_ampView); renderAmpView(); request();
}

void PureSignalWindow::renderAmpView()
{
    if (!m_scene || !m_ampView) { return; }
    m_scene->clear();
    ThemeManager& theme = ThemeManager::instance();
    const QColor ink = theme.color(this,QStringLiteral("color.text.primary"));
    const QColor grid = theme.color(this,QStringLiteral("color.border.subtle"));
    const std::array<QColor,4> colors{theme.color(this,QStringLiteral("color.accent")),
        theme.color(this,QStringLiteral("color.accent.warning")), theme.color(this,QStringLiteral("color.accent.danger")),
        theme.color(this,QStringLiteral("color.accent.success"))};
    m_scene->setBackgroundBrush(theme.brush(QStringLiteral("color.background.spectrum"),QRect(0,0,800,460)));
    const QRectF plot(55,20,680,380); m_scene->setSceneRect(0,0,800,460);
    const double magnitudeMax = m_gain->isChecked() ? 2 : 1;
    const double phaseMax = m_phaseZoom->isChecked() ? 45 : 180;
    const auto point = [plot](double x, double normalizedY) { return QPointF(plot.left()+x*plot.width(),plot.bottom()-normalizedY*plot.height()); };
    const auto text = [this,ink](double x,double y,const QString& value) {
        auto* item = m_scene->addText(value); item->setDefaultTextColor(ink); item->setPos(x,y);
    };
    for (int i = 0; i <= 5; ++i) {
        const double t = i/5.0;
        m_scene->addLine(QLineF(point(t,0),point(t,1)),QPen(grid));
        m_scene->addLine(QLineF(point(0,t),point(1,t)),QPen(grid));
        text(42+t*680,403,QString::number(t,'f',1));
        text(6,390-t*380,QString::number(t*magnitudeMax,'f',1));
        text(738,390-t*380,QString::number(-phaseMax+2*t*phaseMax,'f',0));
    }
    text(300,430,QStringLiteral("Input Magnitude"));
    text(50,0,m_gain->isChecked() ? QStringLiteral("Gain") : QStringLiteral("Magnitude"));
    text(675,0,QStringLiteral("Phase (°)"));
    m_scene->addLine(QLineF(point(0,m_gain->isChecked()?0.5:0),point(1,m_gain->isChecked()?0.5:1)),QPen(grid,1,Qt::DashLine));
    const QVariantList measurements = m_state.value(QStringLiteral("measurements")).toList();
    const QVariantList correction = m_state.value(QStringLiteral("correction")).toList();
    if (measurements.size() != 4 || measurements[0].toList().isEmpty()) {
        text(220,180,m_state.value(QStringLiteral("correctionAvailable")).toBool()
            ? QStringLiteral("Retained correction • no measured samples") : QStringLiteral("Waiting for measured calibration data"));
    } else {
        const auto x = measurements[0].toList(), magnitude = measurements[1].toList();
        const auto cosine = measurements[2].toList(), sine = measurements[3].toList();
        const int count = std::min({x.size(),magnitude.size(),cosine.size(),sine.size()});
        for (int i = 0; i < count; i += m_lowRes->isChecked() ? 4 : 1) {
            const double horizontal = x[i].toDouble()*magnitude[i].toDouble();
            if (m_gain->isChecked() && magnitude[i].toDouble() == 0) { continue; }
            const double mag = m_gain->isChecked() ? (magnitude[i].toDouble() != 0 ? 1/magnitude[i].toDouble() : 0) : x[i].toDouble();
            const double phase = std::remainder(std::atan2(sine[i].toDouble(),cosine[i].toDouble())*180/std::numbers::pi
                - m_state.value(QStringLiteral("phaseReferenceDegrees")).toDouble(),360.0);
            for (int s = 0; s < 2; ++s) {
                const double y = s == 0 ? mag/magnitudeMax : 0.5+phase/(2*phaseMax);
                if (!std::isfinite(horizontal) || !std::isfinite(y) || horizontal < 0 || horizontal > 1 || y < 0 || y > 1) { continue; }
                const QPointF p = point(horizontal,y); m_scene->addEllipse(QRectF(p-QPointF(1,1),QSizeF(2,2)),QPen(colors[s]),QBrush(colors[s]));
            }
        }
    }
    if (correction.size() == 4 && m_state.value(QStringLiteral("correctionAvailable")).toBool()) {
        for (int s = 0; s < 2; ++s) {
            const auto xs = correction[s*2].toList(), ys = correction[s*2+1].toList();
            QPainterPath path; bool started = false;
            for (int i = 0; i < std::min(xs.size(),ys.size()); ++i) {
                const double horizontal = xs[i].toDouble();
                const double y = s == 0 ? ys[i].toDouble()*(m_gain->isChecked()?1:horizontal)/magnitudeMax
                    : 0.5+ys[i].toDouble()/(2*phaseMax);
                if (!std::isfinite(horizontal) || !std::isfinite(y) || horizontal < 0 || horizontal > 1 || y < 0 || y > 1) { started = false; continue; }
                if (!started) { path.moveTo(point(horizontal,y)); started = true; } else { path.lineTo(point(horizontal,y)); }
            }
            m_scene->addPath(path,QPen(colors[s+2],1.5,s == 0 ? Qt::SolidLine : Qt::DashLine));
        }
    }
    const std::array<QString,4> legends{QStringLiteral("Mag Amp •"),QStringLiteral("Phs Amp •"),QStringLiteral("Mag Corr —"),QStringLiteral("Phs Corr - -")};
    for (int i = 0; i < 4; ++i) { auto* item = m_scene->addText(legends[i]); item->setDefaultTextColor(colors[i]); item->setPos(80+i*160,453); }
}
} // namespace AetherSDR
