#include "AnanTransmitPowerPage.h"
#include "models/RadioModel.h"
#include "core/backends/IRadioBackend.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QObject>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVBoxLayout>
#include <memory>

namespace AetherSDR {
QWidget* createAnanTransmitPowerPage(RadioModel* model, QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* hint = new QLabel(QStringLiteral("TX signal processing is prepared automatically. MOX/PTT, CW and TUNE control transmission. Save PA gains, band power limits and the TX antenna below. Thetis reference gains are not PA measurements."),page);
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
    layout->addWidget(group,1);
    group->setEnabled(false);
    group->setAccessibleDescription(QStringLiteral("Connect an ANAN and select an explicit receive profile to configure PA reference values."));
    struct State { QString identity; QString sessionIdentity; QVariantMap reference; quint64 latest = 0;
        int pending = 0; bool dirty = false; bool filling = false; };
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
            if (state->identity != identity || (verb == QLatin1String("txpower.set") && options() == submittedOptions)) { state->dirty = false; }
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
            txAntenna->setEnabled(!busy);
            if (!state->dirty) {
                state->filling = true;
                txAntenna->setCurrentIndex(txAntenna->findData(session.value(QStringLiteral("antenna")).toInt()));
                state->filling = false;
            }
            table->setEnabled(!busy); apply->setEnabled(!busy); defaults->setEnabled(!busy);
            sessionStatus->setText(armed ? QStringLiteral("TX ready: Slice A, %1 MHz, ANT%2, band limit %3 W.")
                .arg(session.value(QStringLiteral("frequencyHz")).toDouble()/1e6,0,'f',6)
                .arg(session.value(QStringLiteral("antenna")).toInt())
                .arg(session.value(QStringLiteral("limitWatts")).toDouble())
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
                        spin->setAccessibleName(name + (column == 1 ? QStringLiteral(" PA gain") : QStringLiteral(" power limit")));
                        spin->setSuffix(column == 1 ? QStringLiteral(" dB") : QStringLiteral(" W"));
                        table->setCellWidget(row,column,spin);
                        QObject::connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),page,[=] {
                            if (!state->filling) { state->dirty = true; output->setText(QStringLiteral("PA profile changes have not been saved.")); }
                        });
                    }
                }
            }
            if (!state->dirty) { fill(data.value(QStringLiteral("options")).toMap()); }
        });
        QObject::connect(backend,&IRadioBackend::extensionError,guard,[=](quint64 reply,const QString& error) {
            if (reply != id) { return; } finish(); guard->deleteLater(); if (backend && model->backend() == backend && model->isConnected()) { output->setText(error); }
        });
        QTimer::singleShot(1500,guard,[=] { finish(); guard->deleteLater(); });
        const QVariantMap payload = verb == QLatin1String("txpower.get") ? QVariantMap{} : QVariantMap{
            {QStringLiteral("identity"),state->identity},{QStringLiteral("options"),submittedOptions},
            {QStringLiteral("antenna"),txAntenna->currentData()}};
        model->invokeBackendExtension(QStringLiteral("anan"),verb,id,payload);
    };
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
