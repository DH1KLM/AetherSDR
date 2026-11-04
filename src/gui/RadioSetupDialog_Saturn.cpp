#include "RadioSetupDialog.h"
#include "GuardedSlider.h"
#include "models/SaturnSettingsModel.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QList>
#include <QMap>
#include <QPair>
#include <QVariant>
#include <QVBoxLayout>
#include <functional>

namespace AetherSDR {
QWidget* RadioSetupDialog::buildAnanHardwareTab()
{
    auto* page = new QWidget;
    auto* settings = new SaturnSettingsModel(m_model, page);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    auto* implementation = new GuardedComboBox(page);
    implementation->setObjectName(QStringLiteral("ananImplementation"));
    implementation->setAccessibleName(QStringLiteral("Saturn implementation"));
    implementation->setAccessibleDescription(QStringLiteral("Choose the independent original or extended Saturn receiver. Disconnect before switching implementation."));
    for (const SaturnSettingsModel::ImplementationChoice& choice : settings->implementationChoices()) {
        implementation->addItem(choice.caption, choice.id);
    }
    form->addRow(QStringLiteral("Saturn implementation:"), implementation);
    layout->addLayout(form);
    auto* hint = new QLabel(page);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* stack = new QStackedWidget(page);
    layout->addWidget(stack, 1);
    QWidget* extended = buildAnanExtendedHardwareTab();
    stack->addWidget(extended);
    const std::function<void()> reseedExtended = m_ananHardwareReseed;
    auto* original = new QWidget(stack);
    auto* options = new QFormLayout(original);
    auto* provenance = new QLabel(QStringLiteral("Original main c2a67af5. Protocol 2, Saturn board 10, one receiver on DDC0. Settings and calibration are independent of DH1KLM."), original);
    provenance->setWordWrap(true);
    options->addRow(provenance);
    auto* rate = new GuardedComboBox(original);
    rate->setAccessibleName(QStringLiteral("Original Saturn sample rate"));
    rate->setObjectName(QStringLiteral("saturnOriginalRate"));
    for (const int ksps : {48, 96, 192, 384, 768, 1536}) { rate->addItem(QStringLiteral("%1 ksps").arg(ksps), ksps); }
    options->addRow(QStringLiteral("Sample rate (next connect):"), rate);
    auto* adc = new GuardedComboBox(original);
    adc->setAccessibleName(QStringLiteral("Original Saturn ADC"));
    adc->addItem(QStringLiteral("ADC0 — ANT1/2/3"), 0);
    adc->addItem(QStringLiteral("ADC1 — RX2"), 1);
    options->addRow(QStringLiteral("ADC (next connect):"), adc);
    const auto save = [settings](const QString& key, const QVariant& value) {
        settings->setOriginalOption(key, value);
    };
    connect(rate, QOverload<int>::of(&QComboBox::currentIndexChanged), original, [=](int i) {
        if (i >= 0) { save(QStringLiteral("ddc0RateKsps"), rate->itemData(i)); }
    });
    connect(adc, QOverload<int>::of(&QComboBox::currentIndexChanged), original, [=](int i) {
        if (i >= 0) { save(QStringLiteral("ddc0AdcIndex"), adc->itemData(i)); }
    });
    const QList<QPair<QString, QString>> fields{
        {QStringLiteral("ditherEnabled"), QStringLiteral("Dither (next connect)")},
        {QStringLiteral("randomEnabled"), QStringLiteral("Randomization (next connect)")},
        {QStringLiteral("bypassAdc0Filters"), QStringLiteral("Bypass ADC0 filters (next connect)")},
        {QStringLiteral("bypassAdc1Filters"), QStringLiteral("Bypass ADC1 filters (next connect)")},
        {QStringLiteral("speakerAudioEnabled"), QStringLiteral("Send receive audio to the radio (next connect)")},
        {QStringLiteral("bandFiltersEnabled"), QStringLiteral("Select receive band-pass filters")},
        {QStringLiteral("speakerAmpMuted"), QStringLiteral("Mute speaker amplifier")}};
    QMap<QString, QCheckBox*> checks;
    for (const auto& field : fields) {
        auto* check = new QCheckBox(field.second, original);
        check->setAccessibleName(field.second);
        check->setObjectName(QStringLiteral("saturnOriginal_") + field.first);
        options->addRow(check);
        checks.insert(field.first, check);
        connect(check, &QCheckBox::toggled, original, [=](bool on) { save(field.first, on); });
    }
    stack->addWidget(original);
    const auto reseed = [=] {
        const SaturnSettingsModel::Snapshot state = settings->snapshot();
        const bool connected = state.connected;
        const bool applicable = state.applicable;
        const QSignalBlocker blocker(implementation);
        implementation->setCurrentIndex(state.originalSelected ? 1 : 0);
        implementation->setEnabled(!connected);
        implementation->setAccessibleDescription(connected
            ? QStringLiteral("Disconnect before selecting another Saturn implementation")
            : QStringLiteral("Applies to the next Saturn connection; other radio models use DH1KLM"));
        hint->setText(!applicable ? QStringLiteral("This radio uses the DH1KLM model implementation; the original path requires Saturn/P2.")
            : connected ? QStringLiteral("Active: %1. Disconnect before switching implementation.").arg(implementation->currentText())
                        : QStringLiteral("Choose an implementation for the next Saturn connection. Both are also listed in Discovery and Connect by IP."));
        stack->setCurrentIndex(applicable && state.originalSelected ? 1 : 0);
        const QVariantMap values = state.originalOptions;
        const QSignalBlocker br(rate), ba(adc);
        rate->setCurrentIndex(rate->findData(values.value(QStringLiteral("ddc0RateKsps"))));
        adc->setCurrentIndex(adc->findData(values.value(QStringLiteral("ddc0AdcIndex"))));
        for (auto it = checks.begin(); it != checks.end(); ++it) {
            const QSignalBlocker bc(it.value());
            it.value()->setChecked(values.value(it.key()).toBool());
        }
        if (stack->currentIndex() == 0 && reseedExtended) { reseedExtended(); }
    };
    connect(implementation, QOverload<int>::of(&QComboBox::currentIndexChanged), page, [=](int i) {
        if (i < 0 || settings->isConnected()) { return; }
        if (!settings->setImplementation(implementation->currentData().toString())) {
            hint->setText(QStringLiteral("Could not save the selected Saturn implementation."));
            return;
        }
        reseed();
    });
    m_ananHardwareReseed = reseed;
    reseed();
    return page;
}
}
