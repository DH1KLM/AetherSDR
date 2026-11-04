#include "AnanTransmitPowerPage.h"
#include "models/RadioModel.h"
#include "core/backends/IRadioBackend.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsLineItem>
#include <QGraphicsEllipseItem>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QPen>
#include <QPainter>
#include <cmath>
#include <algorithm>
#include <numbers>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QObject>
#include <QPushButton>
#include <QTableWidget>
#include <QSizePolicy>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVBoxLayout>
#include <memory>
#include <functional>

namespace AetherSDR {
namespace {
// Numeric controls provide the keyboard and screen-reader equivalent of dragging.
class DiversityPolarView final : public QGraphicsView {
public:
    DiversityPolarView(QDoubleSpinBox* gain, QDoubleSpinBox* phase, QWidget* parent)
        : QGraphicsView(parent), m_gain(gain), m_phase(phase)
    {
        setObjectName(QStringLiteral("ananDiversityPolar"));
        setAccessibleName(QStringLiteral("Diversity phase and amplitude diagram"));
        setAccessibleDescription(QStringLiteral("Drag to set phase and amplitude. Equivalent keyboard controls: Amplitude multiplier and Phase."));
        setMinimumSize(270, 270);
        setScene(new QGraphicsScene(this));
        scene()->setSceneRect(-112, -112, 224, 224);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setRenderHint(QPainter::Antialiasing);
        const auto redraw = [this] { refresh(); };
        QObject::connect(gain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, redraw);
        QObject::connect(phase, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, redraw);
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, redraw);
        refresh();
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event); fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { adjust(event->position()); event->accept(); }
        else { QGraphicsView::mousePressEvent(event); }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) { adjust(event->position()); event->accept(); }
        else { QGraphicsView::mouseMoveEvent(event); }
    }
private:
    void adjust(const QPointF& position) {
        const QPointF point = mapToScene(position.toPoint());
        const double radius = std::hypot(point.x(), point.y());
        m_gain->setValue(std::min(5.0, radius / 20.0));
        if (radius > 0.01) {
            m_phase->setValue(std::atan2(-point.y(), point.x()) * 180.0 / std::numbers::pi);
        }
    }
    void refresh() {
        scene()->clear();
        ThemeManager& theme = ThemeManager::instance();
        const QPen grid(theme.color(this, QStringLiteral("color.canon.lineHi")));
        const QPen accent(theme.color(this, QStringLiteral("color.canon.cyan")), 2);
        setBackgroundBrush(theme.color(this, QStringLiteral("color.canon.ground")));
        for (int ring = 1; ring <= 5; ++ring) {
            const double radius = ring * 20.0;
            scene()->addEllipse(-radius, -radius, radius * 2, radius * 2, grid);
        }
        scene()->addLine(-100, 0, 100, 0, grid);
        scene()->addLine(0, -100, 0, 100, grid);
        const double angle = m_phase->value() * std::numbers::pi / 180.0;
        const double x = 20.0 * m_gain->value() * std::cos(angle);
        const double y = -20.0 * m_gain->value() * std::sin(angle);
        scene()->addLine(0, 0, x, y, accent);
        scene()->addEllipse(x - 3, y - 3, 6, 6, accent, accent.color());
        setAccessibleDescription(QStringLiteral("Amplitude %1, phase %2 degrees. Drag or use the numeric controls. Rings represent gain 1 to 5.")
            .arg(m_gain->value(), 0, 'f', 3).arg(m_phase->value(), 0, 'f', 2));
    }
    QDoubleSpinBox* m_gain;
    QDoubleSpinBox* m_phase;
};
} // namespace
QWidget* createAnanDiversityPage(RadioModel* model, QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* introduction = new QLabel(QStringLiteral("Combine phase-coherent ADC0 and ADC1 IQ before RX1 demodulation. Connect two antennas to the independent ADC inputs. Other receivers remain independent. Diversity starts disabled after a new connection."), page);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* enabled = new QCheckBox(QStringLiteral("Enable coherent IQ diversity"), page);
    enabled->setObjectName(QStringLiteral("ananDiversityEnabled"));
    enabled->setAccessibleName(QStringLiteral("Enable coherent IQ diversity"));
    ThemeManager::instance().applyStyleSheet(enabled, QStringLiteral(
        "QCheckBox { spacing: 10px; padding: 6px 2px; }"
        "QCheckBox::indicator { width: 20px; height: 20px; border-radius: 3px;"
        " border: 2px solid {{color.canon.ink}}; background: {{color.canon.ground}}; }"
        "QCheckBox::indicator:checked { background: {{color.canon.cyan}}; border-color: {{color.canon.cyan}}; }"
        "QCheckBox::indicator:hover, QCheckBox::indicator:focus { border-color: {{color.canon.cyan}}; }"
        "QCheckBox::indicator:disabled { border-color: {{color.canon.muted}}; background: {{color.canon.nested}}; }"));
    form->addRow(enabled);
    auto* gain = new QDoubleSpinBox(page);
    gain->setObjectName(QStringLiteral("ananDiversityGain"));
    gain->setAccessibleName(QStringLiteral("Diversity amplitude multiplier"));
    gain->setRange(0.0, 5.0); gain->setDecimals(3); gain->setSingleStep(0.001);
    gain->setValue(1.0); gain->setKeyboardTracking(false);
    form->addRow(QStringLiteral("Amplitude multiplier:"), gain);
    auto* phase = new QDoubleSpinBox(page);
    phase->setObjectName(QStringLiteral("ananDiversityPhase"));
    phase->setAccessibleName(QStringLiteral("Diversity phase in degrees"));
    phase->setRange(-180.0, 180.0); phase->setDecimals(2); phase->setSingleStep(0.1);
    phase->setSuffix(QStringLiteral(" °")); phase->setKeyboardTracking(false);
    form->addRow(QStringLiteral("Phase:"), phase);
    auto* reference = new QComboBox(page);
    reference->setObjectName(QStringLiteral("ananDiversityReference"));
    reference->setAccessibleName(QStringLiteral("Diversity reference input"));
    reference->addItem(QStringLiteral("ADC0"), 0); reference->addItem(QStringLiteral("ADC1"), 1);
    form->addRow(QStringLiteral("Reference input:"), reference);
    auto* output = new QComboBox(page);
    output->setObjectName(QStringLiteral("ananDiversityOutput"));
    output->setAccessibleName(QStringLiteral("Diversity output"));
    output->addItem(QStringLiteral("ADC0 only"), 0);
    output->addItem(QStringLiteral("ADC1 only"), 1);
    output->addItem(QStringLiteral("Combined IQ"), 2); output->setCurrentIndex(2);
    form->addRow(QStringLiteral("Output:"), output);
    auto* polar = new DiversityPolarView(gain, phase, page);
    layout->addWidget(polar, 1);
    auto* diagramHint = new QLabel(QStringLiteral("Drag the vector to adjust phase and amplitude. Rings: gain 1–5; right = 0°, top = +90°."), page);
    diagramHint->setWordWrap(true);
    layout->addWidget(diagramHint);
    auto* status = new QLabel(page);
    status->setObjectName(QStringLiteral("ananDiversityStatus"));
    status->setAccessibleName(QStringLiteral("Diversity status"));
    status->setWordWrap(true); layout->addWidget(status); layout->addStretch();
    const QList<QWidget*> controls{enabled, gain, phase, reference, output, polar};
    const auto availability = [=](bool available, const QString& reason) {
        for (QWidget* control : controls) {
            control->setEnabled(available);
            control->setAccessibleDescription(available
                ? QStringLiteral("Changes apply during reception. Phase and amplitude affect the non-reference input; output can audition either ADC or their IQ sum.") : reason);
        }
        status->setText(available ? QStringLiteral("Changes apply automatically. Adjustment values are saved per radio, model and protocol.") : reason);
    };
    availability(false, QStringLiteral("Connect a supported two-ADC Extended ANAN receiver."));
    struct State { QString identity; bool pending = false; bool filling = false; bool dirty = false; };
    const auto state = std::make_shared<State>();
    auto* edits = new QTimer(page); edits->setSingleShot(true); edits->setInterval(80);
    const auto request = [=](bool write) {
        if (state->pending || !model || !model->isConnected()
            || !model->backendDeclaresExtension(QStringLiteral("anan"))) { return; }
        if (!write && (state->dirty || gain->hasFocus() || phase->hasFocus())) { return; }
        const QPointer<IRadioBackend> backend = model->backend();
        if (!backend) { return; }
        static quint64 next = 0x444956000000ULL;
        const quint64 id = ++next;
        QVariantMap fields;
        if (write) {
            fields = {{QStringLiteral("identity"), state->identity}, {QStringLiteral("enabled"), enabled->isChecked()},
                {QStringLiteral("gain"), gain->value()}, {QStringLiteral("phaseDegrees"), phase->value()},
                {QStringLiteral("reference"), reference->currentData()}, {QStringLiteral("output"), output->currentData()}};
        }
        state->pending = true; state->dirty = false;
        auto* guard = new QTimer(page); guard->setSingleShot(true); guard->start(1500);
        const auto finish = [=] {
            state->pending = false; guard->stop(); guard->deleteLater();
            if (state->dirty) { edits->start(); }
        };
        QObject::connect(guard, &QTimer::timeout, page, [=] {
            state->dirty = false; edits->stop(); finish();
            availability(false, QStringLiteral("No diversity reply; reconnect or reopen Radio Setup."));
        });
        QObject::connect(backend, &IRadioBackend::extensionError, guard, [=](quint64 reply, const QString& error) {
            if (reply != id || !guard->isActive()) { return; }
            finish();
            if (backend == model->backend()) { status->setText(error); }
        });
        QObject::connect(backend, &IRadioBackend::extensionResult, guard, [=](quint64 reply, const QVariant& value) {
            if (reply != id || !guard->isActive()) { return; }
            finish();
            if (!backend || backend != model->backend() || !model->isConnected()) { return; }
            const QVariantMap data = value.toMap();
            state->identity = data.value(QStringLiteral("identity")).toString();
            availability(data.value(QStringLiteral("available")).toBool(), data.value(QStringLiteral("reason")).toString());
            if (state->dirty) { return; }
            state->filling = true;
            enabled->setChecked(data.value(QStringLiteral("enabled")).toBool());
            gain->setValue(data.value(QStringLiteral("gain"), 1.0).toDouble());
            phase->setValue(data.value(QStringLiteral("phaseDegrees")).toDouble());
            reference->setCurrentIndex(reference->findData(data.value(QStringLiteral("reference"))));
            output->setCurrentIndex(output->findData(data.value(QStringLiteral("output"))));
            state->filling = false;
        });
        model->invokeBackendExtension(QStringLiteral("anan"), write ? QStringLiteral("diversity.set")
            : QStringLiteral("diversity.get"), id, fields);
    };
    const auto changed = [=] { if (!state->filling) { state->dirty = true; edits->start(); } };
    QObject::connect(enabled, &QCheckBox::toggled, page, changed);
    QObject::connect(gain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), page, changed);
    QObject::connect(phase, QOverload<double>::of(&QDoubleSpinBox::valueChanged), page, changed);
    QObject::connect(reference, QOverload<int>::of(&QComboBox::currentIndexChanged), page, changed);
    QObject::connect(output, QOverload<int>::of(&QComboBox::currentIndexChanged), page, changed);
    QObject::connect(edits, &QTimer::timeout, page, [=] { request(true); });
    auto* refresh = new QTimer(page); refresh->setInterval(1000);
    QObject::connect(refresh, &QTimer::timeout, page, [=] {
        if (!model || !model->isConnected()) {
            state->dirty = false;
            availability(false, QStringLiteral("Connect a supported two-ADC Extended ANAN receiver."));
        } else { request(false); }
    });
    refresh->start(); request(false);
    return page;
}

QWidget* createAnanAdcPage(RadioModel* model, QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* hint = new QLabel(QStringLiteral("Assign each hardware DDC to its ADC input. Changes apply during reception without reconnecting. Slices sharing a DDC share its input. Routing changes are unavailable during TX or calibration."),page);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* heading = new QLabel(QStringLiteral("Connect a radio to display its RX / DDC / ADC routing."),page);
    heading->setObjectName(QStringLiteral("ananAdcModelStatus"));
    heading->setWordWrap(true); layout->addWidget(heading);
    auto* columns = new QGridLayout;
    layout->addLayout(columns);
    struct State { QString identity; QVariantList assignments; QVariantList defaults; QVariantList ddcMap; bool filling = false; bool pending = false; quint64 latest = 0; };
    const auto state = std::make_shared<State>();
    QList<QGroupBox*> groups;
    QList<QRadioButton*> buttons;
    QList<QLabel*> cards;
    for (int ddc = 0; ddc < 7; ++ddc) {
        auto* group = new QGroupBox(QStringLiteral("DDC%1").arg(ddc),page);
        auto* rows = new QVBoxLayout(group);
        for (int adc = 0; adc < 2; ++adc) {
            auto* radio = new QRadioButton(QStringLiteral("ADC%1").arg(adc),group);
            radio->setObjectName(QStringLiteral("ananDdc%1Adc%2").arg(ddc).arg(adc));
            radio->setAccessibleName(QStringLiteral("DDC%1 input ADC%2").arg(ddc).arg(adc));
            rows->addWidget(radio); buttons.append(radio);
        }
        columns->addWidget(group,0,ddc); groups.append(group);
        columns->setColumnStretch(ddc,1);
        auto* arrow = new QLabel(QStringLiteral("↓"),page);
        arrow->setAlignment(Qt::AlignCenter);
        arrow->setAccessibleName(QStringLiteral("DDC%1 receiver connections").arg(ddc));
        columns->addWidget(arrow,1,ddc);
        auto* card = new QLabel(QStringLiteral("Not in use"),page);
        card->setObjectName(QStringLiteral("ananDdc%1Receivers").arg(ddc));
        card->setAlignment(Qt::AlignCenter); card->setWordWrap(true); card->setMinimumHeight(112);
        card->setTextFormat(Qt::RichText);
        card->setAccessibleName(QStringLiteral("DDC%1 receiver routing").arg(ddc));
        card->setAccessibleDescription(QStringLiteral("No active RX slice uses this DDC."));
        ThemeManager::instance().applyStyleSheet(card,QStringLiteral(
            "QLabel { background: {{color.background.1}}; color: {{color.text.primary}}; border: 1px solid {{color.border.accent}}; border-radius: 4px; padding: 6px; }"
            "QLabel:disabled { background: {{color.background.0}}; color: {{color.text.secondary}}; border: 1px dashed {{color.border.subtle}}; }"));
        card->setEnabled(false); cards.append(card); columns->addWidget(card,2,ddc);
        group->setEnabled(false);
        group->setAccessibleDescription(QStringLiteral("Connect a supported ANAN to assign its DDC inputs."));
    }
    auto* reset = new QPushButton(QStringLiteral("Reset to default"),page);
    reset->setObjectName(QStringLiteral("ananAdcReset"));
    reset->setAccessibleName(QStringLiteral("Reset DDC ADC assignments"));
    layout->addWidget(reset,0,Qt::AlignLeft);
    auto* status = new QLabel(page); status->setWordWrap(true); layout->addWidget(status); layout->addStretch();
    const auto request = [=](bool write) {
        if (state->pending || !model->isConnected() || !model->backendDeclaresExtension(QStringLiteral("anan"))) { return; }
        QPointer<IRadioBackend> backend = model->backend();
        static quint64 next = 0x414443000000ULL;
        const quint64 id = ++next;
        auto* guard = new QTimer(page); guard->setSingleShot(true); guard->start(1500);
        state->pending = true;
        const auto finish = [=] { state->pending = false; guard->stop(); guard->deleteLater(); };
        QObject::connect(guard,&QTimer::timeout,page,[=] { finish(); status->setText(QStringLiteral("No ADC assignment reply; reload before editing.")); });
        QObject::connect(backend,&IRadioBackend::extensionError,guard,[=](quint64 reply,const QString& error) {
            if (reply != id || !guard->isActive()) { return; } finish();
            if (backend == model->backend()) { status->setText(error); }
        });
        QObject::connect(backend,&IRadioBackend::extensionResult,guard,[=](quint64 reply,const QVariant& value) {
            if (reply != id || !guard->isActive()) { return; } finish();
            if (!backend || backend != model->backend() || !model->isConnected() || id < state->latest) { return; }
            state->latest = id;
            const auto data = value.toMap();
            const auto assignments = data.value(QStringLiteral("assignments")).toList();
            if (assignments.size() != 7) { return; }
            state->identity = data.value(QStringLiteral("identity")).toString(); state->assignments = assignments;
            state->ddcMap = data.value(QStringLiteral("ddcMap")).toList();
            if (state->ddcMap.size() != 7) { return; }
            state->defaults = data.value(QStringLiteral("defaults")).toList();
            heading->setText(QStringLiteral("%1 • Protocol %2 • RX → physical DDC → ADC input")
                .arg(data.value(QStringLiteral("model")).toString()).arg(data.value(QStringLiteral("protocol")).toInt()));
            const bool busy = data.value(QStringLiteral("busy")).toBool();
            const bool diversity = data.value(QStringLiteral("diversityEnabled")).toBool();
            state->filling = true;
            for (int ddc = 0; ddc < 7; ++ddc) {
                const int logical = state->ddcMap.indexOf(ddc);
                const bool supported = logical >= 0 && logical < data.value(QStringLiteral("streams")).toInt();
                groups[ddc]->setEnabled(supported && !busy);
                groups[ddc]->setAccessibleDescription(!supported ? QStringLiteral("This session does not provide this DDC.")
                    : diversity ? QStringLiteral("Disable diversity before changing ADC routing.")
                    : busy ? QStringLiteral("Finish TX or receive configuration before changing ADC routing.") : QStringLiteral("Changes apply immediately during reception."));
                for (int adc = 0; adc < 2; ++adc) {
                    QRadioButton* button = buttons[ddc*2+adc];
                    button->setChecked(supported && assignments[logical].toInt() == adc);
                    button->setEnabled(adc < data.value(QStringLiteral("numAdcs")).toInt());
                    button->setAccessibleDescription(adc < data.value(QStringLiteral("numAdcs")).toInt()
                        ? QStringLiteral("Select the input for this hardware DDC.") : QStringLiteral("This model has no ADC1."));
                }
            }
            state->filling = false; reset->setEnabled(!busy);
            reset->setAccessibleDescription(busy ? QStringLiteral("Finish TX or receive configuration before resetting ADC routing.") : QStringLiteral("Restore Thetis model and protocol ADC assignments."));
            QStringList routes;
            QList<QStringList> receiverCards(7);
            for (const QVariant& window : data.value(QStringLiteral("windows")).toList()) {
                const auto route = window.toMap();
                const int physical = route.value(QStringLiteral("physicalDdc")).toInt();
                if (physical < 0 || physical >= 7) { continue; }
                const QString text = QStringLiteral("RX%1 (Slice %2)\nDDC%3 → ADC%4\nInput: %5\n%6 MHz")
                    .arg(route.value(QStringLiteral("rx")).toInt()).arg(route.value(QStringLiteral("letter")).toString())
                    .arg(physical).arg(route.value(QStringLiteral("adc")).toInt())
                    .arg(route.value(QStringLiteral("input")).toString())
                    .arg(route.value(QStringLiteral("frequencyHz")).toDouble()/1e6,0,'f',6);
                routes.append(text);
                receiverCards[physical].append(text.toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br>")));
            }
            if (diversity) {
                receiverCards[1].append(QStringLiteral("Diversity input<br>ADC1 → RX1 IQ combiner"));
            }
            for (int physical = 0; physical < 7; ++physical) {
                const bool active = !receiverCards[physical].isEmpty();
                cards[physical]->setEnabled(active);
                cards[physical]->setText(active ? receiverCards[physical].join(QStringLiteral("<br><br>")) : QStringLiteral("Not in use"));
                cards[physical]->setAccessibleDescription(active ? cards[physical]->text()
                    : QStringLiteral("No active RX slice uses this DDC. Reserved and unsupported DDCs are not enabled."));
            }
            status->setText((diversity
                ? QStringLiteral("%1 active RX slice(s). DDC0/1 form the coherent diversity pair for RX1. Other DDCs remain independent; PureSignal routing is not enabled.")
                : QStringLiteral("%1 active RX slice(s). Shared DDCs share their ADC input. Grey cards have no active RX; diversity and PureSignal routing are not enabled."))
                .arg(routes.size()));
        });
        model->invokeBackendExtension(QStringLiteral("anan"),write ? QStringLiteral("adc.set") : QStringLiteral("adc.get"),id,
            write ? QVariantMap{{QStringLiteral("identity"),state->identity},{QStringLiteral("assignments"),state->assignments}} : QVariantMap{});
    };
    for (int index = 0; index < buttons.size(); ++index) {
        QObject::connect(buttons[index],&QRadioButton::clicked,page,[=] {
            if (state->filling || state->assignments.size() != 7 || state->pending) { return; }
            const int logical = state->ddcMap.indexOf(index/2);
            if (logical < 0 || logical >= 7) { return; }
            state->assignments[logical] = index%2; request(true);
        });
    }
    QObject::connect(reset,&QPushButton::clicked,page,[=] {
        if (state->assignments.size() != 7 || state->pending) { return; }
        if (state->defaults.size() != 7) { return; }
        state->assignments = state->defaults; request(true);
    });
    auto* timer = new QTimer(page); timer->setInterval(500);
    QObject::connect(timer,&QTimer::timeout,page,[=] { if (page->isVisible()) { request(false); } }); timer->start();
    QObject::connect(model,&RadioModel::connectionStateChanged,page,[=] {
        state->identity.clear(); state->assignments.clear(); reset->setEnabled(false);
        for (QGroupBox* group : groups) { group->setEnabled(false); }
        heading->setText(QStringLiteral("Connect a radio to display its RX / DDC / ADC routing."));
        for (QLabel* card : cards) { card->setEnabled(false); card->setText(QStringLiteral("Not in use")); }
    });
    return page;
}
QWidget* createAnanTransmitPowerPage(RadioModel* model, QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* hint = new QLabel(QStringLiteral("Select the TX source with the slice TX control. Only that slice transmits. During TX, the active band's PA gain applies and saves immediately. Power limits and antenna changes require RX and Save PA profile. Thetis reference gains are not PA measurements."),page);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* group = new QGroupBox(QStringLiteral("Transmit power profile"),page);
    auto* form = new QVBoxLayout(group);
    auto* table = new QTableWidget(group);
    table->setObjectName(QStringLiteral("ananTxPowerTable"));
    table->setAccessibleName(QStringLiteral("ANAN PA gain and operator power limits by band"));
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({QStringLiteral("Band"),QStringLiteral("PA gain (dB)"),QStringLiteral("Power limit (W)")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addWidget(table);
    auto* apply = new QPushButton(QStringLiteral("Save PA profile"),group);
    apply->setObjectName(QStringLiteral("ananTxPowerApply"));
    auto* defaults = new QPushButton(QStringLiteral("Use Thetis defaults"),group);
    defaults->setObjectName(QStringLiteral("ananTxPowerDefaults"));
    auto* output = new QLabel(QStringLiteral("PA profile changes have not been saved."),group);
    output->setObjectName(QStringLiteral("ananTxPowerResult"));
    output->setWordWrap(true);
    auto* txAntenna = new QComboBox(group);
    txAntenna->setObjectName(QStringLiteral("ananTxSessionAntenna"));
    txAntenna->setAccessibleName(QStringLiteral("Transmit antenna for this session"));
    for (int port = 1; port <= 3; ++port) { txAntenna->addItem(QStringLiteral("ANT%1").arg(port),port); }
    auto* sessionStatus = new QLabel(QStringLiteral("TX readiness is managed automatically."),group);
    sessionStatus->setObjectName(QStringLiteral("ananTxSessionStatus"));
    sessionStatus->setWordWrap(true);
    auto* controls = new QFormLayout;
    controls->addRow(defaults,apply);
    controls->addRow(QStringLiteral("TX antenna:"),txAntenna);
    controls->addRow(output);
    controls->addRow(sessionStatus);
    form->addLayout(controls);
    layout->addWidget(group,0,Qt::AlignTop);
    layout->addStretch(1);
    group->setEnabled(false);
    group->setAccessibleDescription(QStringLiteral("Connect an ANAN and select an explicit receive profile to configure PA reference values."));
    struct State { QString identity; QString sessionIdentity; QVariantMap reference; quint64 latest = 0;
        int pending = 0; int band = -1; bool keyed = false; bool dirty = false; bool filling = false;
        std::function<void()> liveGain; };
    const auto state = std::make_shared<State>();
    const auto options = [=] {
        QVariantList gains, limits;
        for (int row = 0; row < table->rowCount(); ++row) {
            gains.append(static_cast<QDoubleSpinBox*>(table->cellWidget(row,1))->value());
            limits.append(static_cast<QDoubleSpinBox*>(table->cellWidget(row,2))->value());
        }
        return QVariantMap{{QStringLiteral("gainsDb"),gains},{QStringLiteral("limitsWatts"),limits}};
    };
    const auto fill = [=](const QVariantMap& value) {
        const auto gains = value.value(QStringLiteral("gainsDb")).toList();
        const auto limits = value.value(QStringLiteral("limitsWatts")).toList();
        if (gains.size() != table->rowCount() || limits.size() != table->rowCount()) { return; }
        state->filling = true;
        for (int row = 0; row < table->rowCount(); ++row) {
            static_cast<QDoubleSpinBox*>(table->cellWidget(row,1))->setValue(gains[row].toDouble());
            static_cast<QDoubleSpinBox*>(table->cellWidget(row,2))->setValue(limits[row].toDouble());
        }
        state->filling = false;
    };
    const auto request = [=](const QString& verb) {
        if ((state->pending != 0 && verb == QLatin1String("txpower.get")) || !model->isConnected()
            || !model->backendDeclaresExtension(QStringLiteral("anan"))) { return; }
        const QPointer<IRadioBackend> backend = model->backend();
        auto* guard = new QObject(page);
        static quint64 next = 0x504147000000ULL;
        const quint64 id = ++next;
        const QVariantMap submittedOptions = options();
        const auto finished = std::make_shared<bool>(false);
        ++state->pending;
        const auto finish = [state,finished] { if (!*finished) { *finished = true; --state->pending; } };
        QObject::connect(backend,&IRadioBackend::extensionResult,guard,[=](quint64 reply,const QVariant& result) {
            if (reply != id) { return; }
            finish(); guard->deleteLater();
            if (!backend || model->backend() != backend || !model->isConnected()) { return; }
            if (id < state->latest) { return; }
            state->latest = id;
            const QVariantMap data = result.toMap();
            const QString identity = data.value(QStringLiteral("identity")).toString();
            if (state->identity != identity || ((verb == QLatin1String("txpower.set") || verb == QLatin1String("txpower.gain")) && options() == submittedOptions)) { state->dirty = false; }
            state->identity = identity;
            if (!state->dirty) { output->clear(); }
            const bool available = data.value(QStringLiteral("available")).toBool();
            group->setEnabled(available);
            group->setAccessibleDescription(available ? QStringLiteral("Experimental PA and ANAN TX session controls")
                : QStringLiteral("Unavailable: connect an ANAN and select an explicit supported receive profile"));
            const auto preparation = data.value(QStringLiteral("preparation")).toMap();
            const bool preparing = preparation.value(QStringLiteral("building")).toBool();
            const auto session = data.value(QStringLiteral("session")).toMap();
            state->sessionIdentity = session.value(QStringLiteral("identity")).toString();
            const bool armed = session.value(QStringLiteral("enabled")).toBool();
            const bool arming = session.value(QStringLiteral("arming")).toBool();
            const QString reason = state->dirty ? QStringLiteral("Save the PA profile changes.")
                : session.value(QStringLiteral("reason")).toString();
            const bool busy = session.value(QStringLiteral("keyed")).toBool() || arming;
            state->keyed = session.value(QStringLiteral("keyed")).toBool();
            state->band = session.value(QStringLiteral("band"),-1).toInt();
            txAntenna->setEnabled(!busy);
            if (!state->dirty) {
                state->filling = true;
                txAntenna->setCurrentIndex(txAntenna->findData(session.value(QStringLiteral("antenna")).toInt()));
                state->filling = false;
            }
            table->setEnabled(!arming); apply->setEnabled(!busy); defaults->setEnabled(!busy);
            sessionStatus->setText(armed ? QStringLiteral("TX ready: Slice %4, %1 MHz, ANT%2, band limit %3 W.")
                .arg(session.value(QStringLiteral("frequencyHz")).toDouble()/1e6,0,'f',6)
                .arg(session.value(QStringLiteral("antenna")).toInt())
                .arg(session.value(QStringLiteral("limitWatts")).toDouble())
                .arg(session.value(QStringLiteral("sliceLabel")).toString())
                : preparing || arming ? QStringLiteral("Preparing TX signal processing automatically.")
                : QStringLiteral("TX unavailable. %1").arg(reason));
            state->reference = data.value(QStringLiteral("reference")).toMap();
            const auto names = data.value(QStringLiteral("bands")).toList();
            if (table->rowCount() != names.size()) {
                table->setRowCount(names.size());
                for (int row = 0; row < names.size(); ++row) {
                    const QString name = names[row].toString();
                    auto* item = new QTableWidgetItem(name);
                    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                    table->setItem(row,0,item);
                    for (int column = 1; column <= 2; ++column) {
                        auto* spin = new QDoubleSpinBox(table);
                        spin->setRange(0,column == 1 ? 100 : 2000); spin->setDecimals(1);
                        spin->setKeyboardTracking(false);
                        spin->setAccessibleName(name + (column == 1 ? QStringLiteral(" PA gain") : QStringLiteral(" power limit")));
                        spin->setSuffix(column == 1 ? QStringLiteral(" dB") : QStringLiteral(" W"));
                        table->setCellWidget(row,column,spin);
                        QObject::connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),page,[=] {
                            if (!state->filling) {
                                state->dirty = true;
                                output->setText(QStringLiteral("PA profile changes have not been saved."));
                                if (column == 1 && state->keyed && row == state->band && state->liveGain) { state->liveGain(); }
                            }
                        });
                    }
                }
            }
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int column = 1; column <= 2; ++column) {
                    QWidget* cell = table->cellWidget(row,column);
                    const bool enabled = !arming && (!busy || (column == 1 && row == state->band && !state->dirty));
                    cell->setEnabled(enabled);
                    cell->setAccessibleDescription(enabled ? QStringLiteral("PA gain applies immediately during TX; power limits require Save PA profile in RX.")
                        : QStringLiteral("During TX only the active band's saved PA gain can change. Finish unsaved edits in RX."));
                }
            }
            table->setFixedHeight(table->horizontalHeader()->height()
                + table->verticalHeader()->length() + 2 * table->frameWidth());
            if (!state->dirty) { fill(data.value(QStringLiteral("options")).toMap()); }
        });
        QObject::connect(backend,&IRadioBackend::extensionError,guard,[=](quint64 reply,const QString& error) {
            if (reply != id) { return; } finish(); guard->deleteLater(); if (backend && model->backend() == backend && model->isConnected()) { output->setText(error); }
        });
        QTimer::singleShot(1500,guard,[=] { finish(); guard->deleteLater(); });
        QVariantMap payload = verb == QLatin1String("txpower.get") ? QVariantMap{} : QVariantMap{
            {QStringLiteral("identity"),state->identity},{QStringLiteral("options"),submittedOptions},
            {QStringLiteral("antenna"),txAntenna->currentData()}};
        if (verb == QLatin1String("txpower.gain") && state->band >= 0 && state->band < table->rowCount()) {
            payload = {{QStringLiteral("identity"),state->identity},{QStringLiteral("sessionIdentity"),state->sessionIdentity},
                {QStringLiteral("band"),state->band},{QStringLiteral("gainDb"),static_cast<QDoubleSpinBox*>(table->cellWidget(state->band,1))->value()}};
        }
        model->invokeBackendExtension(QStringLiteral("anan"),verb,id,payload);
    };
    state->liveGain = [=] { request(QStringLiteral("txpower.gain")); };
    QObject::connect(page,&QObject::destroyed,[state] { state->liveGain = {}; });
    QObject::connect(txAntenna,QOverload<int>::of(&QComboBox::currentIndexChanged),page,[=] {
        if (!state->filling) { state->dirty = true; output->setText(QStringLiteral("PA profile changes have not been saved.")); }
    });
    QObject::connect(apply,&QPushButton::clicked,page,[=] { request(QStringLiteral("txpower.set")); });
    QObject::connect(defaults,&QPushButton::clicked,page,[=] { fill(state->reference); state->dirty = true; output->setText(QStringLiteral("PA profile changes have not been saved.")); });
    QObject::connect(model,&RadioModel::connectionStateChanged,page,[=] {
        state->dirty = false; state->identity.clear(); group->setEnabled(false);
        group->setAccessibleDescription(QStringLiteral("Unavailable: connect an ANAN and select an explicit supported receive profile"));
        output->setText(QStringLiteral("PA profile changes have not been saved.")); sessionStatus->setText(QStringLiteral("TX disabled: connection changed."));
        });
    auto* timer = new QTimer(page);
    timer->setInterval(500);
    QObject::connect(timer,&QTimer::timeout,page,[=] { if (page->isVisible()) { request(QStringLiteral("txpower.get")); } });
    timer->start();
    return page;
}
} // namespace AetherSDR
