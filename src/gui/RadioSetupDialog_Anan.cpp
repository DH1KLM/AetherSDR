#include "RadioSetupDialog.h"
#include "models/RadioModel.h"
#include "core/ThemeManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>
#include <array>

namespace AetherSDR {

QWidget* RadioSetupDialog::buildAnanHardwareTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* identity = new QLabel;
    identity->setWordWrap(true);
    identity->setAccessibleDescription(QStringLiteral("Effective and detected ANAN hardware"));
    layout->addWidget(identity);
    auto* status = new QLabel(QStringLiteral("Connect an ANAN to configure its receive filters."));
    status->setWordWrap(true);
    status->setAccessibleDescription(QStringLiteral("Hardware filter configuration status"));
    layout->addWidget(status);
    auto* adc0 = new QCheckBox(QStringLiteral("ADC0 RF filter bypass"));
    adc0->setAccessibleName(QStringLiteral("ADC0 RF filter bypass"));
    auto* adc1 = new QCheckBox(QStringLiteral("ADC1 RF filter bypass"));
    adc1->setAccessibleName(QStringLiteral("ADC1 RF filter bypass"));
    layout->addWidget(adc0);
    layout->addWidget(adc1);
    auto* active = new QLabel;
    active->setWordWrap(true);
    active->setAccessibleDescription(QStringLiteral("Commanded filter selection, without relay readback"));
    layout->addWidget(active);
    auto* tables = new QHBoxLayout;
    layout->addLayout(tables);
    auto* receiveGroup = new QGroupBox(QStringLiteral("Receive filters"));
    auto* lpfGroup = new QGroupBox(QStringLiteral("ALEX LPF bands"));
    tables->addWidget(receiveGroup);
    tables->addWidget(lpfGroup);
    auto makeTable = [](QGroupBox* group, int rows) {
        auto* box = new QVBoxLayout(group);
        auto* table = new QTableWidget(rows, 3, group);
        table->setHorizontalHeaderLabels({QStringLiteral("Filter"), QStringLiteral("Low MHz"), QStringLiteral("High MHz")});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setMinimumHeight(280);
        for (int row = 0; row < rows; ++row) {
            auto* label = new QTableWidgetItem;
            label->setFlags(Qt::ItemIsEnabled);
            table->setItem(row, 0, label);
            for (int column = 1; column < 3; ++column) {
                auto* spin = new QDoubleSpinBox(table);
                spin->setDecimals(6);
                spin->setRange(0.0, 61.44);
                spin->setSingleStep(0.1);
                spin->setKeyboardTracking(false);
                spin->setAccessibleName(QStringLiteral("Filter %1 %2 frequency").arg(row + 1)
                    .arg(column == 1 ? QStringLiteral("lower") : QStringLiteral("upper")));
                spin->setAccessibleDescription(QStringLiteral("MHz, 0 to 61.44, six decimal places"));
                table->setCellWidget(row, column, spin);
            }
        }
        box->addWidget(table);
        return table;
    };
    auto* receive = makeTable(receiveGroup, 6);
    receive->setAccessibleName(QStringLiteral("ADC receive filter frequency ranges"));
    auto* lpf = makeTable(lpfGroup, 7);
    lpf->setAccessibleName(QStringLiteral("ALEX low pass filter frequency ranges"));
    auto* apply = new QPushButton(QStringLiteral("Apply"));
    apply->setAutoDefault(false);
    apply->setAccessibleName(QStringLiteral("Apply ANAN receive filter configuration"));
    auto* defaults = new QPushButton(QStringLiteral("Restore model defaults"));
    defaults->setAutoDefault(false);
    defaults->setAccessibleName(QStringLiteral("Restore ANAN model filter defaults"));
    auto* actions = new QHBoxLayout;
    actions->addWidget(apply);
    actions->addWidget(defaults);
    actions->addStretch();
    layout->addLayout(actions);
    auto* note = new QLabel(QStringLiteral("Receive-only settings. Apply saves this device and model and updates the running receiver. "
        "Filter indicators show commands, not physical relay feedback. TX, PureSignal and OC outputs are not configured here."));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();
    ThemeManager::instance().applyStyleSheet(page, QStringLiteral(
        "QGroupBox { color: {{color.canon.inkSoft}}; border: 1px solid {{color.canon.lineHi}}; "
        "margin-top: 10px; padding-top: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; }"));

    struct State { QVariantMap current; bool dirty = false; bool filling = false; };
    const auto state = std::make_shared<State>();
    const auto unavailable = [=] {
        state->current.clear();
        state->dirty = false;
        identity->clear();
        active->clear();
        status->setText(QStringLiteral("Connect an ANAN with a validated receive profile first."));
        for (QWidget* widget : std::array<QWidget*, 6>{adc0, adc1, receive, lpf, apply, defaults}) {
            widget->setEnabled(false);
            widget->setAccessibleDescription(QStringLiteral("Unavailable without a connected ANAN receive profile"));
        }
    };
    const auto fill = [=](const QVariantMap& data) {
        if (data.isEmpty()) { unavailable(); return; }
        const bool changed = state->current.value(QStringLiteral("identity")) != data.value(QStringLiteral("identity"))
            || state->current.value(QStringLiteral("modelId")) != data.value(QStringLiteral("modelId"))
            || state->current.value(QStringLiteral("boardId")) != data.value(QStringLiteral("boardId"));
        if (changed) {
            state->dirty = false;
            status->setText(QStringLiteral("Edit frequency limits or bypass options, then select Apply."));
        }
        state->current = data;
        identity->setText(QStringLiteral("Model: %1 • P%2 • Override %3\nDetected: %4 • %5")
            .arg(data.value(QStringLiteral("model")).toString()).arg(data.value(QStringLiteral("protocol")).toInt())
            .arg(data.value(QStringLiteral("override")).toBool() ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(data.value(QStringLiteral("physical")).toString(), data.value(QStringLiteral("identity")).toString()));
        const bool mkII = data.value(QStringLiteral("mkII")).toBool();
        const QStringList rxNames = mkII
            ? QStringList{QStringLiteral("160 m"), QStringLiteral("80/60 m"), QStringLiteral("40/30 m"), QStringLiteral("20/15 m"), QStringLiteral("12/10 m"), QStringLiteral("6 m")}
            : QStringList{QStringLiteral("1.5 MHz HPF"), QStringLiteral("6.5 MHz HPF"), QStringLiteral("9.5 MHz HPF"), QStringLiteral("13 MHz HPF"), QStringLiteral("20 MHz HPF"), QStringLiteral("6 m")};
        const QStringList lpNames{QStringLiteral("160 m"), QStringLiteral("80 m"), QStringLiteral("60/40 m"), QStringLiteral("30/20 m"), QStringLiteral("17/15 m"), QStringLiteral("12/10 m"), QStringLiteral("6 m")};
        receiveGroup->setTitle(mkII ? QStringLiteral("ADC BPF bands") : QStringLiteral("ALEX HPF bands"));
        const auto selected = [](const QStringList& names, int row) {
            return row >= 0 && row < names.size() ? names[row] : QStringLiteral("Bypass");
        };
        const bool twoAdcs = data.value(QStringLiteral("numAdcs")).toInt() > 1;
        const bool lpEnabled = data.value(QStringLiteral("selectLpf")).toBool();
        active->setText(QStringLiteral("%1 MHz • Commanded ADC0: %2 • ADC1: %3 • LPF: %4")
            .arg(data.value(QStringLiteral("frequencyHz")).toDouble() / 1e6, 0, 'f', 6)
            .arg(selected(rxNames, data.value(QStringLiteral("receiveActive")).toInt()))
            .arg(twoAdcs ? selected(rxNames, data.value(QStringLiteral("adc1Active")).toInt()) : QStringLiteral("Not fitted"))
            .arg(lpEnabled ? selected(lpNames, data.value(QStringLiteral("lpfActive")).toInt()) : QStringLiteral("Not driven by this profile")));
        adc0->setEnabled(true);
        adc1->setEnabled(twoAdcs);
        adc1->setAccessibleDescription(twoAdcs ? QStringLiteral("Bypass the separate RX2 receive filter bank") : QStringLiteral("This profile has only one ADC"));
        receive->setEnabled(true);
        receive->setAccessibleDescription(QStringLiteral("Ordered, non-overlapping frequency ranges in MHz"));
        lpf->setEnabled(lpEnabled);
        lpf->setAccessibleDescription(lpEnabled ? QStringLiteral("Ordered, non-overlapping frequency ranges in MHz") : QStringLiteral("LPF control is not enabled for this receive profile"));
        apply->setEnabled(true);
        apply->setAccessibleDescription(QStringLiteral("Save and apply the edited receive filter settings"));
        defaults->setEnabled(true);
        defaults->setAccessibleDescription(QStringLiteral("Save and apply Thetis-compatible defaults for this model"));
        bool editing = false;
        for (QTableWidget* table : {receive, lpf}) {
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int col = 1; col < 3; ++col) {
                    QWidget* cell = table->cellWidget(row, col);
                    editing = editing || cell->hasFocus() || cell->isAncestorOf(QApplication::focusWidget());
                }
            }
        }
        if (!state->dirty && (!editing || changed)) {
            state->filling = true;
            adc0->setChecked(data.value(QStringLiteral("bypassAdc0")).toBool());
            adc1->setChecked(data.value(QStringLiteral("bypassAdc1")).toBool());
            const auto populate = [](QTableWidget* table, const QVariantList& rows, const QStringList& names) {
                for (int row = 0; row < table->rowCount(); ++row) {
                    table->item(row, 0)->setText(names[row]);
                    for (int col = 1; col < 3; ++col) {
                        auto* spin = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, col));
                        spin->setValue(rows[row].toMap().value(col == 1 ? QStringLiteral("lowHz") : QStringLiteral("highHz")).toDouble() / 1e6);
                    }
                }
            };
            populate(receive, data.value(QStringLiteral("receive")).toList(), rxNames);
            populate(lpf, data.value(QStringLiteral("lowPass")).toList(), lpNames);
            state->filling = false;
        }
    };
    const auto dirty = [=] { if (!state->filling) { state->dirty = true; } };
    connect(adc0, &QCheckBox::toggled, page, dirty);
    connect(adc1, &QCheckBox::toggled, page, dirty);
    for (QTableWidget* table : {receive, lpf}) {
        for (int row = 0; row < table->rowCount(); ++row) {
            for (int col = 1; col < 3; ++col) {
                connect(qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, col)),
                    &QDoubleSpinBox::valueChanged, page, dirty);
            }
        }
    }
    const auto request = [=, this](const QString& verb, const QVariantMap& options) {
        IRadioBackend* backend = m_model->backend();
        if (!backend || !m_model->backendDeclaresExtension(QStringLiteral("anan"))) { unavailable(); return; }
        //DH1KLM: A fresh receiver-bound guard prevents replies from a previous session repainting this page.
        auto* guard = new QObject(page);
        static quint64 nextId = 0x414e414e0000ULL;
        const quint64 id = ++nextId;
        connect(backend, &IRadioBackend::extensionResult, guard, [=, this](quint64 replyId, const QVariant& result) {
            if (replyId != id) { return; }
            guard->deleteLater();
            if (m_model->backend() != backend) { return; }
            if (verb != QLatin1String("hw.get")) {
                state->dirty = false;
                status->setText(QStringLiteral("Saved for this device and model; applied to the receiver."));
            }
            fill(result.toMap());
        });
        connect(backend, &IRadioBackend::extensionError, guard, [=, this](quint64 replyId, const QString& error) {
            if (replyId != id) { return; }
            guard->deleteLater();
            if (m_model->backend() == backend) { status->setText(error); }
        });
        QTimer::singleShot(1500, guard, &QObject::deleteLater);
        m_model->invokeBackendExtension(QStringLiteral("anan"), verb, id, options);
    };
    const auto collect = [=] {
        QVariantMap options = state->current;
        const auto rows = [](QTableWidget* table) {
            QVariantList values;
            for (int row = 0; row < table->rowCount(); ++row) {
                QVariantMap value;
                for (int col = 1; col < 3; ++col) {
                    const double mhz = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, col))->value();
                    value.insert(col == 1 ? QStringLiteral("lowHz") : QStringLiteral("highHz"), qRound64(mhz * 1e6));
                }
                values.append(value);
            }
            return values;
        };
        options.insert(QStringLiteral("receive"), rows(receive));
        options.insert(QStringLiteral("lowPass"), rows(lpf));
        options.insert(QStringLiteral("bypassAdc0"), adc0->isChecked());
        options.insert(QStringLiteral("bypassAdc1"), adc1->isChecked());
        return options;
    };
    connect(apply, &QPushButton::clicked, page, [=] { request(QStringLiteral("hw.set"), collect()); });
    connect(defaults, &QPushButton::clicked, page, [=] { request(QStringLiteral("hw.defaults"), state->current); });
    m_ananHardwareReseed = [=] { unavailable(); request(QStringLiteral("hw.get"), {}); };
    auto* timer = new QTimer(page);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, page, [=] { if (page->isVisible()) { request(QStringLiteral("hw.get"), {}); } });
    timer->start();
    m_ananHardwareReseed();
    return page;
}
} // namespace AetherSDR
