#include "AnanTransmitPowerPage.h"
#include "models/RadioModel.h"
#include "core/backends/IRadioBackend.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
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
            state->filling = true;
            for (int ddc = 0; ddc < 7; ++ddc) {
                const int logical = state->ddcMap.indexOf(ddc);
                const bool supported = logical >= 0 && logical < data.value(QStringLiteral("streams")).toInt();
                groups[ddc]->setEnabled(supported && !busy);
                groups[ddc]->setAccessibleDescription(!supported ? QStringLiteral("This session does not provide this DDC.")
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
            for (int physical = 0; physical < 7; ++physical) {
                const bool active = !receiverCards[physical].isEmpty();
                cards[physical]->setEnabled(active);
                cards[physical]->setText(active ? receiverCards[physical].join(QStringLiteral("<br><br>")) : QStringLiteral("Not in use"));
                cards[physical]->setAccessibleDescription(active ? cards[physical]->text()
                    : QStringLiteral("No active RX slice uses this DDC. Reserved and unsupported DDCs are not enabled."));
            }
            status->setText(QStringLiteral("%1 active RX slice(s). Shared DDCs share their ADC input. Grey cards have no active RX; diversity and PureSignal routing are not enabled.")
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
