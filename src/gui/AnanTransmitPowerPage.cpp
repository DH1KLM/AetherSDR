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
    auto* hint = new QLabel(QStringLiteral("Profile and calculation preview only. This page does not enable transmission. Thetis defaults are reference values, not measurements of your PA. A zero power limit disables that band in the preview."),page);
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
    auto* band = new QComboBox(group);
    band->setObjectName(QStringLiteral("ananTxPowerBand"));
    band->setAccessibleName(QStringLiteral("PA preview band"));
    auto* watts = new QDoubleSpinBox(group);
    watts->setObjectName(QStringLiteral("ananTxPowerWatts"));
    watts->setAccessibleName(QStringLiteral("Requested preview power"));
    watts->setRange(0,2000); watts->setDecimals(1); watts->setSuffix(QStringLiteral(" W"));
    auto* preview = new QPushButton(QStringLiteral("Calculate preview"),group);
    preview->setObjectName(QStringLiteral("ananTxPowerPreview"));
    auto* apply = new QPushButton(QStringLiteral("Save PA profile"),group);
    apply->setObjectName(QStringLiteral("ananTxPowerApply"));
    auto* defaults = new QPushButton(QStringLiteral("Use Thetis defaults"),group);
    defaults->setObjectName(QStringLiteral("ananTxPowerDefaults"));
    auto* output = new QLabel(QStringLiteral("Preview not calculated"),group);
    output->setObjectName(QStringLiteral("ananTxPowerResult"));
    output->setWordWrap(true);
    auto* prepareDsp = new QPushButton(QStringLiteral("Prepare TX DSP"),group);
    prepareDsp->setObjectName(QStringLiteral("ananTxDspPrepare"));
    auto* cancelDsp = new QPushButton(QStringLiteral("Discard TX DSP"),group);
    cancelDsp->setObjectName(QStringLiteral("ananTxDspCancel"));
    auto* dspStatus = new QLabel(QStringLiteral("TX DSP not prepared. Transmission remains disabled."),group);
    dspStatus->setObjectName(QStringLiteral("ananTxDspStatus"));
    dspStatus->setWordWrap(true);
    prepareDsp->setEnabled(false); cancelDsp->setEnabled(false);
    auto* controls = new QFormLayout;
    controls->addRow(QStringLiteral("Preview band:"),band);
    controls->addRow(QStringLiteral("Requested power:"),watts);
    controls->addRow(preview,output);
    controls->addRow(defaults,apply);
    controls->addRow(prepareDsp,cancelDsp);
    controls->addRow(dspStatus);
    form->addLayout(controls);
    layout->addWidget(group,1);
    group->setEnabled(false);
    group->setAccessibleDescription(QStringLiteral("Connect an ANAN and select an explicit receive profile to configure PA reference values."));
    struct State { QString identity; QVariantMap reference; bool busy = false; bool dirty = false; bool filling = false; };
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
        if (state->busy || !model->isConnected() || !model->backendDeclaresExtension(QStringLiteral("anan"))) { return; }
        const QPointer<IRadioBackend> backend = model->backend();
        auto* guard = new QObject(page);
        static quint64 next = 0x504147000000ULL;
        const quint64 id = ++next;
        state->busy = true;
        QObject::connect(backend,&IRadioBackend::extensionResult,guard,[=](quint64 reply,const QVariant& result) {
            if (reply != id) { return; }
            state->busy = false; guard->deleteLater();
            if (!backend || model->backend() != backend || !model->isConnected()) { return; }
            const QVariantMap data = result.toMap();
            const QString identity = data.value(QStringLiteral("identity")).toString();
            if (state->identity != identity || verb == QLatin1String("txpower.set")) { state->dirty = false; }
            state->identity = identity;
            const bool available = data.value(QStringLiteral("available")).toBool();
            group->setEnabled(available);
            group->setAccessibleDescription(available ? QStringLiteral("PA configuration preview only; transmit remains disabled")
                : QStringLiteral("Unavailable: connect an ANAN and select an explicit supported receive profile"));
            const auto preparation = data.value(QStringLiteral("preparation")).toMap();
            const bool preparing = preparation.value(QStringLiteral("building")).toBool();
            const bool ready = preparation.value(QStringLiteral("ready")).toBool();
            const bool supported = preparation.value(QStringLiteral("available")).toBool();
            prepareDsp->setEnabled(supported && !preparing);
            prepareDsp->setAccessibleDescription(supported ? QStringLiteral("Builds a DSP channel without keying or sending radio packets")
                : QStringLiteral("Requires an idle native P2 radio in USB or LSB"));
            cancelDsp->setEnabled(preparing || ready);
            const QString failure = preparation.value(QStringLiteral("error")).toString();
            dspStatus->setText(preparing ? QStringLiteral("Preparing TX DSP on the planning worker. Transmission remains disabled.")
                : ready ? QStringLiteral("TX DSP prepared for %1. Not installed in the transmitter; transmission remains disabled.").arg(preparation.value(QStringLiteral("mode")).toString())
                : failure.isEmpty() ? QStringLiteral("TX DSP not prepared. Transmission remains disabled.")
                : QStringLiteral("TX DSP preparation failed: %1").arg(failure));
            state->reference = data.value(QStringLiteral("reference")).toMap();
            const auto names = data.value(QStringLiteral("bands")).toList();
            if (table->rowCount() != names.size()) {
                table->setRowCount(names.size()); band->clear();
                for (int row = 0; row < names.size(); ++row) {
                    const QString name = names[row].toString();
                    band->addItem(name);
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
                            if (!state->filling) { state->dirty = true; output->setText(QStringLiteral("Preview not calculated")); }
                        });
                    }
                }
            }
            if (!state->dirty) { fill(data.value(QStringLiteral("options")).toMap()); }
            if (data.contains(QStringLiteral("preview"))) {
                const auto value = data.value(QStringLiteral("preview")).toMap();
                output->setText(QStringLiteral("Limited target: %1 W; volume scalar: %2; raw drive: %3/255. No RF output requested.")
                    .arg(value.value(QStringLiteral("watts")).toDouble(),0,'f',1)
                    .arg(value.value(QStringLiteral("audioVolume")).toDouble(),0,'f',6)
                    .arg(value.value(QStringLiteral("drive")).toInt()));
            }
        });
        QObject::connect(backend,&IRadioBackend::extensionError,guard,[=](quint64 reply,const QString& error) {
            if (reply != id) { return; } state->busy = false; guard->deleteLater(); if (backend && model->backend() == backend && model->isConnected()) { output->setText(error); }
        });
        QTimer::singleShot(1500,guard,[=] { state->busy = false; guard->deleteLater(); });
        const QVariantMap payload = verb == QLatin1String("txpower.get") ? QVariantMap{} : QVariantMap{
            {QStringLiteral("identity"),state->identity},{QStringLiteral("options"),options()},
            {QStringLiteral("band"),band->currentIndex()},{QStringLiteral("watts"),watts->value()}};
        model->invokeBackendExtension(QStringLiteral("anan"),verb,id,payload);
    };
    QObject::connect(prepareDsp,&QPushButton::clicked,page,[=] { request(QStringLiteral("txdsp.prepare")); });
    QObject::connect(cancelDsp,&QPushButton::clicked,page,[=] { request(QStringLiteral("txdsp.cancel")); });
    QObject::connect(apply,&QPushButton::clicked,page,[=] { request(QStringLiteral("txpower.set")); });
    QObject::connect(preview,&QPushButton::clicked,page,[=] { request(QStringLiteral("txpower.preview")); });
    QObject::connect(defaults,&QPushButton::clicked,page,[=] { fill(state->reference); state->dirty = true; output->setText(QStringLiteral("Preview not calculated")); });
    QObject::connect(model,&RadioModel::connectionStateChanged,page,[=] { state->dirty = false; state->identity.clear(); group->setEnabled(false); group->setAccessibleDescription(QStringLiteral("Unavailable: connect an ANAN and select an explicit supported receive profile")); output->setText(QStringLiteral("Preview not calculated")); });
    QObject::connect(band,QOverload<int>::of(&QComboBox::currentIndexChanged),page,[=] { output->setText(QStringLiteral("Preview not calculated")); });
    QObject::connect(watts,QOverload<double>::of(&QDoubleSpinBox::valueChanged),page,[=] { output->setText(QStringLiteral("Preview not calculated")); });
    auto* timer = new QTimer(page);
    timer->setInterval(500);
    QObject::connect(timer,&QTimer::timeout,page,[=] { if (page->isVisible()) { request(QStringLiteral("txpower.get")); } });
    timer->start();
    return page;
}
} // namespace AetherSDR
