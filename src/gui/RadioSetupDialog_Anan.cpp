#include "RadioSetupDialog.h"
#include "models/RadioModel.h"
#include "core/ThemeManager.h"
#include <QApplication>
#include <QBoxLayout>
#include <QAccessible>
#include <QStyle>
#include <QTabWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QTreeWidget>
#include "GuardedSlider.h"
#include "models/BandPlanManager.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QResizeEvent>
#include <QScrollBar>
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
namespace {
QLabel* makeFilterLed(QWidget* parent, const QString& name)
{
    auto* led = new QLabel(parent);
    led->setObjectName(QStringLiteral("ananFilterLed"));
    led->setFixedSize(12,12);
    led->setProperty("active", false);
    led->setAccessibleName(name + QStringLiteral(": not selected"));
    led->setAccessibleDescription(QStringLiteral("Commanded filter selection; no physical relay readback"));
    ThemeManager::instance().applyStyleSheet(led, QStringLiteral(
        "QLabel { background: {{color.background.2}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 6px; }"
        "QLabel[active=\"true\"] { background: {{color.accent.success}}; }"));
    return led;
}
void updateFilterLed(QLabel* led, bool selected, const QString& name)
{
    const QString text = name + (selected ? QStringLiteral(": selected") : QStringLiteral(": not selected"));
    if (led->property("active").toBool() == selected && led->accessibleName() == text) { return; }
    led->setProperty("active", selected);
    led->setAccessibleName(text);
    led->setToolTip(text);
    led->style()->unpolish(led);
    led->style()->polish(led);
    led->update();
    QAccessibleEvent event(led, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

//DH1KLM: Keep both banks visible without imposing their combined width on
// the setup viewport. The surrounding scroll area handles vertical overflow.
class FilterTables : public QWidget {
public:
    FilterTables(QWidget* first, QWidget* second, int firstStretch = 0, int secondStretch = 0)
        : m_first(first), m_second(second), m_box(new QBoxLayout(QBoxLayout::TopToBottom, this))
    {
        m_box->setContentsMargins(0,0,0,0);
        m_box->setSizeConstraint(QLayout::SetNoConstraint);
        m_box->addWidget(first, firstStretch);
        m_box->addWidget(second, secondStretch);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    QSize minimumSizeHint() const override
    {
        return {qMax(m_first->minimumSizeHint().width(), m_second->minimumSizeHint().width()), heightForWidth(width())};
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        const int firstHeight = qMax(m_first->sizeHint().height(), m_first->minimumSizeHint().height());
        const int secondHeight = qMax(m_second->sizeHint().height(), m_second->minimumSizeHint().height());
        return sideBySide(width) ? qMax(firstHeight, secondHeight)
            : firstHeight + secondHeight + m_box->spacing();
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        const auto direction = sideBySide(event->size().width()) ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
        if (m_box->direction() != direction) {
            m_box->setDirection(direction);
            updateGeometry();
        }
        //DH1KLM: SetNoConstraint allows width reflow, but must not let an
        // enclosing tab squeeze away the final filter row or its controls.
        setMinimumHeight(heightForWidth(event->size().width()));
        QWidget::resizeEvent(event);
    }
private:
    bool sideBySide(int width) const
    {
        return width >= m_first->minimumSizeHint().width() + m_second->minimumSizeHint().width() + m_box->spacing();
    }
    QWidget* m_first;
    QWidget* m_second;
    QBoxLayout* m_box;
};
} // namespace


QWidget* RadioSetupDialog::buildAnanHardwareTab()
{
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    auto* tabs = new QTabWidget(page);
    tabs->setObjectName(QStringLiteral("ananHardwareTabs"));
    tabs->setAccessibleName(QStringLiteral("ANAN hardware configuration categories"));
    outer->addWidget(tabs, 1);
    const auto tab = [](QTabWidget* parent, const QString& name) {
        auto* widget = new QWidget(parent);
        parent->addTab(widget, name);
        return widget;
    };
    auto* infoPage = tab(tabs, QStringLiteral("Radio Info"));
    auto* antennaPage = tab(tabs, QStringLiteral("Antenna / ALEX"));
    auto* ocPage = tab(tabs, QStringLiteral("OC Outputs"));
    auto* calibrationPage = tab(tabs, QStringLiteral("Calibration"));
    auto* infoLayout = new QVBoxLayout(infoPage);
    auto* identityGroup = new QGroupBox(QStringLiteral("Board Identity"), infoPage);
    auto* identityLayout = new QVBoxLayout(identityGroup);
    infoLayout->addWidget(identityGroup);
    auto* facts = new QFormLayout;
    identityLayout->addLayout(facts);
    const QStringList factNames{QStringLiteral("Board:"), QStringLiteral("Protocol:"), QStringLiteral("ADC count:"),
        QStringLiteral("Max RX:"), QStringLiteral("Firmware:"), QStringLiteral("MAC:"), QStringLiteral("IP address:")};
    const auto factLabels = std::make_shared<QVector<QLabel*>>();
    for (const auto& name : factNames) {
        auto* value = new QLabel(QStringLiteral("—"), identityGroup);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setAccessibleDescription(name);
        facts->addRow(name, value);
        factLabels->append(value);
    }
    auto* operatingGroup = new QGroupBox(QStringLiteral("Operating Parameters"), infoPage);
    auto* operatingLayout = new QFormLayout(operatingGroup);
    auto* operating = new QLabel;
    operating->setWordWrap(true);
    operating->setAccessibleDescription(QStringLiteral("Live sample rate, ADC routing and linearisation"));
    operatingLayout->addRow(operating);
    auto* ctun = new QCheckBox(QStringLiteral("CTUN — tune within a fixed spectrum"), operatingGroup);
    ctun->setObjectName(QStringLiteral("ananCtun"));
    ctun->setAccessibleName(QStringLiteral("ANAN CTUN"));
    ctun->setAccessibleDescription(QStringLiteral("Hold the DDC centre while tuning digitally; applies immediately"));
    operatingLayout->addRow(ctun);
    auto* ctunInfo = new QLabel(operatingGroup);
    ctunInfo->setWordWrap(true);
    ctunInfo->setAccessibleDescription(QStringLiteral("Live CTUN state and hardware DDC centre"));
    operatingLayout->addRow(ctunInfo);
    infoLayout->addWidget(operatingGroup);
    infoLayout->addStretch();
    auto* antennaLayout = new QVBoxLayout(antennaPage);
    auto* antennaTabs = new QTabWidget(antennaPage);
    antennaTabs->setAccessibleName(QStringLiteral("Antenna and ALEX sections"));
    antennaLayout->addWidget(antennaTabs);
    auto* antennaControl = tab(antennaTabs, QStringLiteral("Antenna Control"));
    auto* alex1 = tab(antennaTabs, QStringLiteral("Alex-1 Filters"));
    auto* alex2 = tab(antennaTabs, QStringLiteral("Alex-2 Filters"));
    auto* antennaControlLayout = new QVBoxLayout(antennaControl);
    auto* antennaGroup = new QGroupBox(QStringLiteral("Receive Antenna"), antennaControl);
    auto* antennaForm = new QFormLayout(antennaGroup);
    auto* antennaCombo = new GuardedComboBox(antennaGroup);
    antennaCombo->setAccessibleName(QStringLiteral("ADC0 receive antenna"));
    for (int n = 1; n <= 3; ++n) { antennaCombo->addItem(QStringLiteral("ANT%1").arg(n), n); }
    auto* rxInput = new GuardedComboBox(antennaGroup);
    rxInput->setAccessibleName(QStringLiteral("ADC0 receive input"));
    rxInput->setAccessibleDescription(QStringLiteral("Select the ADC0 receive input; changes apply after Apply"));
    antennaForm->addRow(QStringLiteral("ADC0 input:"), rxInput);
    antennaForm->addRow(QStringLiteral("ANT bank:"), antennaCombo);
    auto* antennaHint = new QLabel(QStringLiteral("Connect an ANAN to show its active receive input."));
    antennaHint->setWordWrap(true);
    const auto setAntennaHint = [antennaHint](const QString& text) {
        if (antennaHint->text() == text && antennaHint->accessibleName() == text) { return; }
        antennaHint->setText(text);
        antennaHint->setAccessibleName(text);
        QAccessibleEvent event(antennaHint, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    };
    antennaForm->addRow(antennaHint);
    antennaControlLayout->addWidget(antennaGroup);
    const auto unsupported = [](QVBoxLayout* parent, const QString& title, const QStringList& fields, const QString& reason) {
        auto* group = new QGroupBox(title);
        auto* box = new QVBoxLayout(group);
        auto* hint = new QLabel(reason, group);
        hint->setWordWrap(true);
        box->addWidget(hint);
        for (const auto& field : fields) {
            auto* check = new QCheckBox(field, group);
            check->setAccessibleName(field);
            check->setAccessibleDescription(reason);
            check->setEnabled(false);
            box->addWidget(check);
        }
        parent->addWidget(group);
        return group;
    };
    unsupported(antennaControlLayout, QStringLiteral("TX Antenna per Band"),
        {QStringLiteral("Block TX on Ant 2"), QStringLiteral("Block TX on Ant 3"), QStringLiteral("Use TX antenna for RX")},
        QStringLiteral("Unavailable: the ANAN backend currently supports receive only; per-band antenna routing is not implemented."));
    antennaControlLayout->addStretch();
    auto* layout = new QVBoxLayout(alex1);
    auto* alex2Layout = new QVBoxLayout(alex2);
    auto* alex2Group = new QGroupBox(QStringLiteral("Alex-2 Receive Filters"), alex2);
    auto* alex2GroupLayout = new QVBoxLayout(alex2Group);
    alex2Layout->addWidget(alex2Group);
    auto* alex2Hint = new QLabel(QStringLiteral("ADC1 follows the single receiver frequency with its own band limits. Edit limits, then select Apply."));
    alex2Hint->setWordWrap(true);
    alex2GroupLayout->addWidget(alex2Hint);
    alex2Layout->addStretch();
    auto* ocLayout = new QVBoxLayout(ocPage);
    ocLayout->setContentsMargins(0, 0, 0, 0);
    auto* digitalGroup = new QGroupBox(QStringLiteral("User Dig Out"), ocPage);
    auto* digitalLayout = new QHBoxLayout(digitalGroup);
    digitalLayout->setContentsMargins(8, 4, 8, 4);
    for (int pin = 1; pin <= 4; ++pin) {
        auto* check = new QCheckBox(QStringLiteral("Pin %1").arg(pin), digitalGroup);
        check->setAccessibleName(QStringLiteral("User digital output %1").arg(pin));
        check->setAccessibleDescription(QStringLiteral("Unavailable: user digital outputs are not implemented for this ANAN backend"));
        check->setToolTip(check->accessibleDescription());
        check->setEnabled(false);
        digitalLayout->addWidget(check);
    }
    digitalLayout->addStretch();
    ocLayout->addWidget(digitalGroup);
    auto* ocTabs = new QTabWidget(ocPage);
    ocTabs->setAccessibleName(QStringLiteral("OC output bands"));
    ocLayout->addWidget(ocTabs, 1);
    auto* hf = tab(ocTabs, QStringLiteral("HF"));
    auto* vhf = tab(ocTabs, QStringLiteral("VHF"));
    auto* swl = tab(ocTabs, QStringLiteral("SWL"));
    auto* hfLayout = new QVBoxLayout(hf);
    hfLayout->setContentsMargins(8, 8, 8, 8);
    hfLayout->setSpacing(6);
    auto* ocControls = new QHBoxLayout;
    auto* ocEnabled = new QCheckBox(QStringLiteral("Enable RX OC outputs"), hf);
    ocEnabled->setAccessibleName(QStringLiteral("Enable receive open collector outputs"));
    ocEnabled->setAccessibleDescription(QStringLiteral("Explicitly enable the configured RX pin matrix. Default off; no TX or extra Anvelina pins are driven."));
    ocControls->addWidget(ocEnabled);
    ocControls->addStretch();
    auto* ocReset = new QPushButton(QStringLiteral("Reset OC defaults"), hf);
    ocReset->setAccessibleDescription(QStringLiteral("Clear RX OC pin assignments and disable RX OC outputs. Select Apply to save."));
    ocReset->setToolTip(ocReset->accessibleDescription());
    ocControls->addWidget(ocReset);
    hfLayout->addLayout(ocControls);
    const QStringList ocBandNames{QStringLiteral("160m"), QStringLiteral("80m"), QStringLiteral("60m"), QStringLiteral("40m"),
        QStringLiteral("30m"), QStringLiteral("20m"), QStringLiteral("17m"), QStringLiteral("15m"), QStringLiteral("12m"),
        QStringLiteral("10m"), QStringLiteral("6m"), QStringLiteral("2m"), QStringLiteral("GEN"),
        QStringLiteral("WWV"), QStringLiteral("XVTR")};
    struct OcMatrix {
        QGroupBox* group;
        std::array<QLabel*, 11> bands{};
        std::array<std::array<QCheckBox*, 7>, 11> pins{};
    };
    const auto matrix = [&](const QString& title, bool tx) {
        auto result = std::make_shared<OcMatrix>();
        result->group = new QGroupBox(title, hf);
        auto* grid = new QGridLayout(result->group);
        grid->setContentsMargins(8, 6, 8, 6);
        grid->setHorizontalSpacing(4);
        grid->setVerticalSpacing(1);
        grid->addWidget(new QLabel(QStringLiteral("Band"), result->group), 0, 0);
        for (int pin = 0; pin < 7; ++pin) {
            grid->addWidget(new QLabel(QStringLiteral("P%1").arg(pin+1), result->group), 0, pin+1, Qt::AlignHCenter);
        }
        grid->setColumnStretch(8, 1);
        for (int row = 0; row < ocBandNames.size(); ++row) {
            auto* band = new QLabel(ocBandNames[row], result->group);
            grid->addWidget(band, row+1, 0);
            const bool supported = !tx && row < 11;
            const QString reason = tx ? QStringLiteral("Unavailable: ANAN transmit is not implemented")
                : QStringLiteral("Unavailable: 2m, independent GEN/WWV and transverter OC routing are not implemented; the RX matrix covers 160m through 6m");
            if (!supported) {
                band->setEnabled(false);
                band->setAccessibleDescription(reason);
                band->setToolTip(reason);
            }
            if (row < 11) { result->bands[row] = band; }
            for (int pin = 0; pin < 7; ++pin) {
                auto* check = new QCheckBox(result->group);
                check->setAccessibleName(QStringLiteral("%1 %2 pin %3").arg(tx ? QStringLiteral("TX") : QStringLiteral("RX"), ocBandNames[row]).arg(pin+1));
                check->setAccessibleDescription(supported
                    ? QStringLiteral("Assert this OC pin only within the configured receive band when RX OC outputs are enabled") : reason);
                check->setToolTip(check->accessibleDescription());
                check->setEnabled(supported);
                grid->addWidget(check, row+1, pin+1, Qt::AlignHCenter);
                if (row < 11) { result->pins[row][pin] = check; }
            }
        }
        if (tx) {
            result->group->setAccessibleDescription(QStringLiteral("Unavailable: ANAN transmit is not implemented"));
            result->group->setToolTip(result->group->accessibleDescription());
            result->group->setEnabled(false);
        }
        return result;
    };
    const auto ocRx = matrix(QStringLiteral("RX OC Pins per Band"), false);
    const auto ocTx = matrix(QStringLiteral("TX OC Pins per Band"), true);
    hfLayout->addWidget(new FilterTables(ocRx->group, ocTx->group, 1, 1));
    auto* ocActions = new QGroupBox(QStringLiteral("TX Pin Action mapping"), hf);
    auto* actionGrid = new QGridLayout(ocActions);
    actionGrid->setHorizontalSpacing(6);
    actionGrid->setVerticalSpacing(1);
    const QStringList actionNames{QStringLiteral("MOX"), QStringLiteral("Tune"), QStringLiteral("TwoTone"),
        QStringLiteral("MOX+Tune"), QStringLiteral("MOX+\nTwoTone"), QStringLiteral("Tune+\nTwoTone"),
        QStringLiteral("MOX+Tune+\nTwoTone")};
    actionGrid->addWidget(new QLabel(QStringLiteral("Pin"), ocActions), 0, 0);
    for (int action = 0; action < actionNames.size(); ++action) {
        auto* label = new QLabel(actionNames[action], ocActions);
        label->setAlignment(Qt::AlignCenter);
        actionGrid->addWidget(label, 0, action+1);
        for (int pin = 0; pin < 7; ++pin) {
            auto* check = new QCheckBox(ocActions);
            check->setAccessibleName(QStringLiteral("TX pin %1 %2").arg(pin+1).arg(QString(actionNames[action]).replace(QLatin1Char('\n'), QString())));
            check->setAccessibleDescription(QStringLiteral("Unavailable: ANAN TX pin actions are not implemented"));
            check->setToolTip(check->accessibleDescription());
            check->setEnabled(false);
            actionGrid->addWidget(check, pin+1, action+1, Qt::AlignHCenter);
        }
    }
    for (int pin = 0; pin < 7; ++pin) {
        actionGrid->addWidget(new QLabel(QStringLiteral("Pin %1").arg(pin+1), ocActions), pin+1, 0);
    }
    ocActions->setAccessibleDescription(QStringLiteral("Unavailable: ANAN TX pin actions are not implemented"));
    ocActions->setToolTip(ocActions->accessibleDescription());
    ocActions->setEnabled(false);
    auto* ocLive = new QGroupBox(QStringLiteral("Live OC pin state"), hf);
    auto* liveLayout = new QVBoxLayout(ocLive);
    auto* ledRow = new QHBoxLayout;
    const auto ocLeds = std::make_shared<std::array<QLabel*, 7>>();
    for (int pin = 0; pin < 7; ++pin) {
        auto* column = new QVBoxLayout;
        auto* led = makeFilterLed(ocLive, QStringLiteral("OC pin %1").arg(pin+1));
        led->setAccessibleDescription(QStringLiteral("Commanded RX OC output; no physical pin feedback"));
        (*ocLeds)[pin] = led;
        column->addWidget(led, 0, Qt::AlignHCenter);
        column->addWidget(new QLabel(QString::number(pin+1), ocLive), 0, Qt::AlignHCenter);
        ledRow->addLayout(column);
    }
    liveLayout->addLayout(ledRow);
    auto* ocState = new QLabel(ocLive);
    ocState->setWordWrap(true);
    ocState->setAccessibleDescription(QStringLiteral("Commanded RX OC pin state"));
    liveLayout->addWidget(ocState);
    auto* ocLiveNote = new QLabel(QStringLiteral("Reflects the commanded RX OC byte. No physical pin feedback."), ocLive);
    ocLiveNote->setWordWrap(true);
    liveLayout->addWidget(ocLiveNote);
    liveLayout->addStretch();
    hfLayout->addWidget(new FilterTables(ocActions, ocLive, 3, 1));
    auto* ocNote = new QLabel(QStringLiteral("Hover a band name to inspect its frequency range, taken from the active band plan on first configuration. Outside configured bands all pins are off. Select Apply to save."));
    ocNote->setWordWrap(true);
    hfLayout->addWidget(ocNote);
    hfLayout->addStretch();
    auto* vhfLayout = new QVBoxLayout(vhf);
    unsupported(vhfLayout, QStringLiteral("VHF OC Outputs"), {QStringLiteral("Enable VHF OC matrix")},
        QStringLiteral("Unavailable: the current ANAN receive path ends at 61.44 MHz; VHF/transverter OC routing is not implemented."));
    vhfLayout->addStretch();
    auto* swlLayout = new QVBoxLayout(swl);
    unsupported(swlLayout, QStringLiteral("SWL — RX / TX OC matrix"), {QStringLiteral("Enable SWL OC matrix")},
        QStringLiteral("Unavailable: independent SWL OC ranges are not implemented. The HF RX matrix remains available."));
    swlLayout->addStretch();
    BandPlanManager bandPlan;
    bandPlan.loadPlans();
    const std::array<std::pair<double,double>,11> searches{{{1.7,2.1},{3.0,4.1},{5.0,5.5},{6.8,7.4},{10.0,10.2},
        {14.0,14.4},{18.0,18.2},{21.0,21.5},{24.8,25.0},{28.0,30.0},{50.0,54.1}}};
    const auto ocDefaultRanges = std::make_shared<QVariantList>();
    for (const auto& search : searches) {
        const auto regions = bandPlan.contiguousRegionsForBand(search.first, search.second);
        double low = 0, high = 0;
        if (!regions.isEmpty()) { low = regions.first().lowMhz; high = regions.last().highMhz; }
        ocDefaultRanges->append(QVariantMap{{QStringLiteral("lowHz"), qRound64(low*1e6)}, {QStringLiteral("highHz"), qRound64(high*1e6)}});
    }
    auto* calibrationLayout = new QVBoxLayout(calibrationPage);
    auto* calibrationColumns = new QHBoxLayout;
    calibrationLayout->addLayout(calibrationColumns);
    auto* calLeft = new QVBoxLayout;
    auto* calRight = new QVBoxLayout;
    calibrationColumns->addLayout(calLeft,1);
    calibrationColumns->addLayout(calRight,1);
    const auto calGroup = [](QVBoxLayout* parent, const QString& name, const QString& reason) {
        auto* group = new QGroupBox(name);
        auto* form = new QFormLayout(group);
        auto* hint = new QLabel(reason, group);
        hint->setWordWrap(true);
        form->addRow(hint);
        parent->addWidget(group);
        return form;
    };
    const auto unavailableNumber = [](QFormLayout* form, const QString& name,
                                      double low, double high, double value, int decimals) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(low, high); spin->setDecimals(decimals); spin->setValue(value);
        spin->setAccessibleName(name);
        spin->setAccessibleDescription(QStringLiteral("Unavailable: this ANAN calibration function is not implemented. Displayed value is a reference default, not a measured calibration."));
        spin->setEnabled(false);
        form->addRow(name, spin);
    };
    const auto unavailableButton = [](QFormLayout* form, const QString& name) {
        auto* button = new QPushButton(name);
        button->setAutoDefault(false);
        button->setAccessibleName(name);
        button->setAccessibleDescription(QStringLiteral("Unavailable: this ANAN calibration function is not implemented"));
        button->setEnabled(false);
        form->addRow(button);
    };
    auto* frequencyCal = calGroup(calLeft, QStringLiteral("Freq Cal"), QStringLiteral("Unavailable: host oscillator calibration is not implemented for ANAN."));
    unavailableNumber(frequencyCal, QStringLiteral("Frequency (Hz):"),0,30000000,10000000,0);
    unavailableButton(frequencyCal, QStringLiteral("Start frequency calibration"));
    auto* levelCal = calGroup(calLeft, QStringLiteral("Level Cal"), QStringLiteral("Unavailable: receive level and LNA calibration are not implemented."));
    unavailableNumber(levelCal,QStringLiteral("Frequency (Hz):"),0,30000000,14100000,0);
    unavailableNumber(levelCal,QStringLiteral("Level (dBm):"),-200,0,-73,1);
    unavailableNumber(levelCal,QStringLiteral("Rx1 6m LNA:"),0,25,13,1);
    unavailableNumber(levelCal,QStringLiteral("Rx2 6m LNA:"),0,25,13,1);
    unavailableButton(levelCal,QStringLiteral("Start level calibration"));
    unavailableButton(levelCal,QStringLiteral("Reset level calibration"));
    auto* diagnosticCal = calGroup(calRight,QStringLiteral("HPSDR Freq Cal Diagnostic"),QStringLiteral("Unavailable: ANAN oscillator correction and external-reference control are not implemented."));
    unavailableNumber(diagnosticCal,QStringLiteral("Correction factor:"),0.5,1.5,1.0,9);
    unavailableNumber(diagnosticCal,QStringLiteral("10 MHz factor:"),0.5,1.5,1.0,9);
    auto* txCal = calGroup(calRight,QStringLiteral("TX Display Cal"),QStringLiteral("Unavailable: ANAN transmit is not implemented."));
    unavailableNumber(txCal,QStringLiteral("Offset (dB):"),-100,100,0,1);
    auto* currentCal = calGroup(calRight,QStringLiteral("Volts/Amps Calibration"),QStringLiteral("Unavailable: ANAN current calibration is not implemented. No measured values are shown."));
    unavailableNumber(currentCal,QStringLiteral("Sensitivity:"),0.001,5000,1,3);
    unavailableNumber(currentCal,QStringLiteral("Offset:"),-1000,1000,0,3);
    calLeft->addStretch(); calRight->addStretch();
    auto* droop = new QPushButton(QStringLiteral("Open Droop Correction"), calibrationPage);
    droop->setAutoDefault(false);
    droop->setAccessibleName(QStringLiteral("Open ANAN DDC droop calibration"));
    calibrationLayout->addWidget(droop);
    connect(droop, &QPushButton::clicked, page, [this] {
        if (m_navigation && m_droopCalibrationPageIndex >= 0) { m_navigation->setCurrentItem(m_pageItems.value(m_droopCalibrationPageIndex)); }
    });
    calibrationLayout->addStretch();
    auto* identity = new QLabel;
    identity->setWordWrap(true);
    identity->setAccessibleDescription(QStringLiteral("Effective and detected ANAN hardware"));
    identityLayout->insertWidget(0, identity);
    auto* status = new QLabel(QStringLiteral("Connect an ANAN to configure its receive filters."));
    status->setWordWrap(true);
    status->setAccessibleDescription(QStringLiteral("Hardware filter configuration status"));
    outer->addWidget(status);
    auto* adc0 = new QCheckBox(QStringLiteral("ADC0 RF filter bypass"));
    adc0->setAccessibleName(QStringLiteral("ADC0 RF filter bypass"));
    auto* adc1 = new QCheckBox(QStringLiteral("ADC1 RF filter bypass"));
    adc1->setAccessibleName(QStringLiteral("ADC1 RF filter bypass"));
    adc1->setObjectName(QStringLiteral("ananAdc1FilterBypass"));
    // RX2 bypass is independent of the presence of an Alex-2 filter bank.
    antennaForm->insertRow(2, adc1);
    auto* bypassRow = new QHBoxLayout;
    auto* bypassLed = makeFilterLed(alex1, QStringLiteral("ADC0 receive bypass"));
    bypassRow->addWidget(bypassLed);
    bypassRow->addWidget(adc0);
    bypassRow->addStretch();
    layout->addLayout(bypassRow);
    auto* adc1BypassRow = new QHBoxLayout;
    auto* adc1BypassLed = makeFilterLed(alex2, QStringLiteral("ADC1 receive bypass"));
    adc1BypassRow->addWidget(adc1BypassLed);
    auto* adc1BypassLabel = new QLabel(QStringLiteral("ADC1 receive bypass"), alex2Group);
    adc1BypassRow->addWidget(adc1BypassLabel);
    adc1BypassRow->addStretch();
    alex2GroupLayout->insertLayout(0, adc1BypassRow);
    auto* active = new QLabel;
    active->setWordWrap(true);
    active->setAccessibleDescription(QStringLiteral("Commanded filter selection, without relay readback"));
    layout->addWidget(active);
    auto* receiveGroup = new QGroupBox(QStringLiteral("Receive filters"));
    auto* lpfGroup = new QGroupBox(QStringLiteral("ALEX LPF bands"));
    layout->addWidget(new FilterTables(receiveGroup, lpfGroup));
    auto makeTable = [](QGroupBox* group, int rows, bool bandBypass = true) {
        const bool hasBypass = rows == 6 && bandBypass;
        auto* box = new QVBoxLayout(group);
        auto* table = new QTableWidget(rows, hasBypass ? 4 : 3, group);
        table->setHorizontalHeaderLabels(hasBypass
            ? QStringList{QStringLiteral("Filter"), QStringLiteral("Low MHz"), QStringLiteral("High MHz"), QStringLiteral("Bypass")}
            : QStringList{QStringLiteral("Filter"), QStringLiteral("Low MHz"), QStringLiteral("High MHz")});
        table->verticalHeader()->hide();
        const QFontMetrics metrics(table->font());
        const int filterWidth = metrics.horizontalAdvance(QStringLiteral("9.5 MHz HPF")) + 34;
        const int bypassWidth = metrics.horizontalAdvance(QStringLiteral("Bypass")) + 20;
        auto* header = table->horizontalHeader();
        header->setSectionResizeMode(0, QHeaderView::Fixed);
        table->setColumnWidth(0, filterWidth);
        for (int column = 1; column < 3; ++column) {
            header->setSectionResizeMode(column, QHeaderView::Stretch);
        }
        if (hasBypass) {
            header->setSectionResizeMode(3, QHeaderView::Fixed);
            table->setColumnWidth(3, bypassWidth);
        }
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setMinimumHeight(280);
        for (int row = 0; row < rows; ++row) {
            auto* label = new QTableWidgetItem;
            label->setFlags(Qt::ItemIsEnabled);
            table->setItem(row, 0, label);
            auto* nameCell = new QWidget(table);
            auto* nameLayout = new QHBoxLayout(nameCell);
            nameLayout->setContentsMargins(2,0,2,0);
            auto* name = new QLabel(nameCell);
            name->setObjectName(QStringLiteral("filterBandName"));
            nameLayout->addWidget(makeFilterLed(nameCell, QStringLiteral("Filter %1").arg(row+1)));
            nameLayout->addWidget(name);
            nameLayout->addStretch();
            table->setCellWidget(row,0,nameCell);
            if (hasBypass) {
                auto* bypass = new QCheckBox(table);
                bypass->setAccessibleName(QStringLiteral("Receive filter %1 band bypass").arg(row+1));
                bypass->setAccessibleDescription(QStringLiteral("Bypass this ADC0 receive filter band after Apply"));
                table->setCellWidget(row,3,bypass);
            }
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
        //DH1KLM: Preserve localized six-decimal values within each bank;
        // FilterTables stacks the complete banks when their combined width cannot fit.
        const int frequencyWidth = qobject_cast<QDoubleSpinBox*>(table->cellWidget(0,1))->sizeHint().width();
        table->setMinimumWidth(filterWidth + 2 * frequencyWidth
            + (hasBypass ? bypassWidth : 0) + 2 * table->frameWidth()
            + table->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
        box->addWidget(table);
        return table;
    };
    auto* receive = makeTable(receiveGroup, 6);
    auto* adc1RangesGroup = new QGroupBox(QStringLiteral("ADC1 BPF bands"), alex2Group);
    auto* receiveAdc1 = makeTable(adc1RangesGroup, 6, false);
    receiveAdc1->setAccessibleName(QStringLiteral("ADC1 receive filter frequency ranges"));
    for (int row = 0; row < 6; ++row) {
        for (int col = 1; col < 3; ++col) {
            receiveAdc1->cellWidget(row, col)->setAccessibleName(QStringLiteral("ADC1 filter %1 %2 frequency").arg(row + 1)
                .arg(col == 1 ? QStringLiteral("lower") : QStringLiteral("upper")));
        }
    }
    alex2GroupLayout->addWidget(adc1RangesGroup);
    auto* receiveOptions = qobject_cast<QVBoxLayout*>(receiveGroup->layout());
    for (const auto& text : {QStringLiteral("HPF Bypass on TX"), QStringLiteral("HPF Bypass on PureSignal feedback"),
            QStringLiteral("Disable 6m LNA on TX")}) {
        auto* check = new QCheckBox(text, receiveGroup);
        check->setAccessibleName(text);
        check->setAccessibleDescription(QStringLiteral("Unavailable: ANAN TX and PureSignal are not implemented"));
        check->setEnabled(false);
        receiveOptions->insertWidget(1,check);
    }
    auto* disable6mRx = new QCheckBox(QStringLiteral("Disable 6m LNA on RX"), receiveGroup);
    disable6mRx->setAccessibleName(QStringLiteral("Disable 6m LNA on receive"));
    disable6mRx->setAccessibleDescription(QStringLiteral("Select receive bypass instead of the 6m filter/LNA on ADC0; applies after Apply"));
    receiveOptions->insertWidget(1,disable6mRx);
    receive->setAccessibleName(QStringLiteral("ADC receive filter frequency ranges"));
    auto* lpf = makeTable(lpfGroup, 7);
    lpf->setAccessibleName(QStringLiteral("ALEX low pass filter frequency ranges"));
    auto* lpfBypassRx = new QCheckBox(QStringLiteral("6m/ByPass on RX"), lpfGroup);
    lpfBypassRx->setAccessibleName(QStringLiteral("Use 6m low pass filter on receive"));
    lpfBypassRx->setAccessibleDescription(QStringLiteral("Force the 6m LPF instead of automatic receive LPF selection; applies after Apply"));
    qobject_cast<QVBoxLayout*>(lpfGroup->layout())->addWidget(lpfBypassRx);
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
    outer->insertLayout(0, actions);
    outer->insertWidget(1, status);
    auto* note = new QLabel(QStringLiteral("Receive-only settings. Apply saves this device and model and updates the running receiver. "
        "Filter indicators show commands, not physical relay feedback. TX and PureSignal are unavailable. OC outputs are configured on their own tab."));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();
    ThemeManager::instance().applyStyleSheet(page, QStringLiteral(
        "QGroupBox { color: {{color.canon.inkSoft}}; border: 1px solid {{color.canon.lineHi}}; "
        "margin-top: 10px; padding-top: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; }"));

    struct State { QVariantMap current; bool dirty = false; bool filling = false; };
    const auto state = std::make_shared<State>();
    const auto setAlex2Available = [=](bool available, const QString& reason) {
        alex2Hint->setText(available
            ? QStringLiteral("ADC1 follows the receiver frequency with independent filter limits. Changes apply after Apply.") : reason);
        alex2Hint->setAccessibleDescription(alex2Hint->text());
        alex2->setAccessibleDescription(alex2Hint->text());
        antennaTabs->setTabToolTip(antennaTabs->indexOf(alex2), alex2Hint->text());
        adc1RangesGroup->setEnabled(available);
        receiveAdc1->setEnabled(available);
        receiveAdc1->setAccessibleDescription(available
            ? QStringLiteral("Independent ADC1 ranges in MHz; ordered and non-overlapping") : reason);
        adc1BypassLed->setEnabled(available);
        adc1BypassLabel->setEnabled(available);
        adc1BypassLed->setAccessibleDescription(available
            ? QStringLiteral("Commanded ADC1 receive bypass; no physical relay feedback") : reason);
        for (int row = 0; row < receiveAdc1->rowCount(); ++row) {
            for (int col = 1; col < 3; ++col) {
                auto* field = receiveAdc1->cellWidget(row, col);
                field->setAccessibleDescription(available
                    ? QStringLiteral("Independent ADC1 filter frequency in MHz; select Apply to save") : reason);
                field->setToolTip(field->accessibleDescription());
            }
        }
    };
    const auto unavailable = [=] {
        state->current.clear();
        state->dirty = false;
        identity->clear();
        setAntennaHint(QStringLiteral("Connect an ANAN to show its active receive input."));
        setAlex2Available(false, QStringLiteral("Unavailable: connect an ANAN with a validated Alex-2 receive profile first."));
        updateFilterLed(bypassLed, false, QStringLiteral("ADC0 receive bypass"));
        updateFilterLed(adc1BypassLed, false, QStringLiteral("ADC1 receive bypass"));
        for (QTableWidget* table : {receive, receiveAdc1, lpf}) {
            for (int row = 0; row < table->rowCount(); ++row) {
                updateFilterLed(table->cellWidget(row,0)->findChild<QLabel*>(QStringLiteral("ananFilterLed")),
                    false, table->item(row,0)->data(Qt::AccessibleTextRole).toString());
            }
        }
        operating->clear();
        ctun->setEnabled(false);
        ctun->setAccessibleDescription(QStringLiteral("Unavailable without a connected ANAN receive profile"));
        { const QSignalBlocker block(ctun); ctun->setChecked(false); }
        ctunInfo->setText(QStringLiteral("Connect an ANAN to use CTUN."));
        ocState->clear();
        for (int pin = 0; pin < 7; ++pin) {
            updateFilterLed((*ocLeds)[pin], false, QStringLiteral("OC pin %1").arg(pin+1));
        }
        for (auto* label : *factLabels) { label->setText(QStringLiteral("—")); }
        for (QWidget* control : std::array<QWidget*, 6>{antennaCombo, rxInput, ocRx->group, ocEnabled, droop, ocReset}) {
            control->setEnabled(false);
            control->setAccessibleDescription(QStringLiteral("Unavailable without a connected ANAN receive profile"));
        }
        active->clear();
        status->setText(QStringLiteral("Connect an ANAN with a validated receive profile first."));
        for (QWidget* widget : std::array<QWidget*, 9>{adc0, adc1, receive, receiveAdc1, lpf, apply, defaults, disable6mRx, lpfBypassRx}) {
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
            status->setText(QStringLiteral("Edit the receive input, filters or RX OC pins, then select Apply. ANT1/2/3 selection applies immediately."));
        }
        state->current = data;
        identity->setText(QStringLiteral("Model: %1 • P%2 • Override %3\nDetected: %4 • %5")
            .arg(data.value(QStringLiteral("model")).toString()).arg(data.value(QStringLiteral("protocol")).toInt())
            .arg(data.value(QStringLiteral("override")).toBool() ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(data.value(QStringLiteral("physical")).toString(), data.value(QStringLiteral("identity")).toString()));
        const QStringList values{data.value(QStringLiteral("physical")).toString(),
            QStringLiteral("Protocol %1").arg(data.value(QStringLiteral("protocol")).toInt()),
            data.value(QStringLiteral("numAdcs")).toString(),
            QStringLiteral("%1 reported; 1 active receiver").arg(data.value(QStringLiteral("reportedReceivers")).toInt()),
            data.value(QStringLiteral("firmware")).toString(), data.value(QStringLiteral("identity")).toString(),
            data.value(QStringLiteral("host")).toString()};
        for (int n = 0; n < values.size(); ++n) { (*factLabels)[n]->setText(values[n]); }
        operating->setText(QStringLiteral("Sample rate: %1 ksps • ADC%2 • Dither %3 • Random %4")
            .arg(data.value(QStringLiteral("rateKsps")).toInt()).arg(data.value(QStringLiteral("adcIndex")).toInt())
            .arg(data.value(QStringLiteral("dither")).toBool() ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(data.value(QStringLiteral("random")).toBool() ? QStringLiteral("ON") : QStringLiteral("OFF")));
        { const QSignalBlocker block(ctun); ctun->setChecked(data.value(QStringLiteral("ctun")).toBool()); }
        ctun->setEnabled(data.value(QStringLiteral("ctunAvailable")).toBool());
        ctun->setAccessibleDescription(ctun->isEnabled()
            ? QStringLiteral("Hold the DDC centre while tuning digitally; applies immediately")
            : QStringLiteral("Unavailable while a droop calibration sweep is running"));
        ctunInfo->setText(QStringLiteral("CTUN %1 • DDC centre: %2 MHz • RX: %3 MHz")
            .arg(ctun->isChecked() ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(data.value(QStringLiteral("ddcCenterHz")).toDouble() / 1e6, 0, 'f', 6)
            .arg(data.value(QStringLiteral("sliceFrequencyHz")).toDouble() / 1e6, 0, 'f', 6));
        antennaCombo->setEnabled(true);
        //DH1KLM: The ADC0 relay bank is distinct from the active receiver's ADC route.
        const bool activeAdc1 = data.value(QStringLiteral("adcIndex")).toInt() == 1;
        const QStringList inputNames = data.value(QStringLiteral("rxInputNames")).toStringList();
        const int input = data.value(QStringLiteral("rxOnlyInput")).toInt();
        const QString inputName = input == 0
            ? QStringLiteral("ANT%1").arg(data.value(QStringLiteral("antenna")).toInt())
            : inputNames.value(input);
        rxInput->setEnabled(true);
        rxInput->setAccessibleDescription(QStringLiteral("Select the ADC0 input; applies after Apply. Active input: %1").arg(inputName));
        const QString route = activeAdc1
            ? QStringLiteral("Active receiver: ADC1 / RX2. ADC0 input and antenna settings do not change this active input.")
            : QStringLiteral("Active receiver: ADC0 / %1. %2")
                .arg(inputName)
                .arg(data.value(QStringLiteral("numAdcs")).toInt() > 1
                    ? QStringLiteral("ADC1 uses its dedicated RX2 input.")
                    : QStringLiteral("This profile has one ADC."));
        setAntennaHint(route + QStringLiteral(" Input changes apply after Apply; ANT bank changes apply immediately."));
        antennaCombo->setAccessibleDescription(route + QStringLiteral(" ADC0 antenna changes apply immediately."));
        { const QSignalBlocker block(antennaCombo); antennaCombo->setCurrentIndex(data.value(QStringLiteral("antenna")).toInt()-1); }
        ocRx->group->setEnabled(true);
        ocRx->group->setAccessibleDescription(QStringLiteral("Receive OC pins, all off by default"));
        ocEnabled->setEnabled(true);
        ocReset->setEnabled(true);
        ocReset->setAccessibleDescription(QStringLiteral("Clear RX OC pin assignments and disable RX OC outputs. Select Apply to save."));
        droop->setEnabled(data.value(QStringLiteral("protocol")).toInt() == 2);
        droop->setAccessibleDescription(droop->isEnabled() ? QStringLiteral("Open the existing P2 DDC droop sweep page")
            : QStringLiteral("Unavailable: the droop sweep requires Protocol 2"));
        ocState->setText(QStringLiteral("Commanded RX OC mask: 0x%1").arg(data.value(QStringLiteral("ocMask")).toInt(), 2, 16, QLatin1Char('0')));
        for (int pin = 0; pin < 7; ++pin) {
            updateFilterLed((*ocLeds)[pin], data.value(QStringLiteral("ocMask")).toInt() & (1 << pin), QStringLiteral("OC pin %1").arg(pin+1));
        }
        const bool mkII = data.value(QStringLiteral("mkII")).toBool();
        const QStringList rxNames = mkII
            ? QStringList{QStringLiteral("160 m"), QStringLiteral("80/60 m"), QStringLiteral("40/30 m"), QStringLiteral("20/15 m"), QStringLiteral("12/10 m"), QStringLiteral("6 m")}
            : QStringList{QStringLiteral("1.5 MHz HPF"), QStringLiteral("6.5 MHz HPF"), QStringLiteral("9.5 MHz HPF"), QStringLiteral("13 MHz HPF"), QStringLiteral("20 MHz HPF"), QStringLiteral("6 m")};
        const QStringList lpNames{QStringLiteral("160 m"), QStringLiteral("80 m"), QStringLiteral("60/40 m"), QStringLiteral("30/20 m"), QStringLiteral("17/15 m"), QStringLiteral("12/10 m"), QStringLiteral("6 m")};
        receiveGroup->setTitle(mkII ? QStringLiteral("ALEX BPF bands") : QStringLiteral("ALEX HPF bands"));
        const auto selected = [](const QStringList& names, int row) {
            return row >= 0 && row < names.size() ? names[row] : QStringLiteral("Bypass");
        };
        const bool showAlex2 = data.value(QStringLiteral("hasAlex2Filters")).toBool();
        setAlex2Available(showAlex2, QStringLiteral("Unavailable: the selected model / override profile does not have an Alex-2 filter bank."));
        const bool twoAdcs = data.value(QStringLiteral("numAdcs")).toInt() > 1;
        const bool lpEnabled = data.value(QStringLiteral("selectLpf")).toBool();
        active->setText(QStringLiteral("%1 MHz • Commanded ADC0: %2 • ADC1: %3 • LPF: %4")
            .arg(data.value(QStringLiteral("frequencyHz")).toDouble() / 1e6, 0, 'f', 6)
            .arg(selected(rxNames, data.value(QStringLiteral("receiveActive")).toInt()))
            .arg(twoAdcs ? selected(rxNames, data.value(QStringLiteral("adc1Active")).toInt()) : QStringLiteral("Not fitted"))
            .arg(lpEnabled ? selected(lpNames, data.value(QStringLiteral("lpfActive")).toInt()) : QStringLiteral("Not driven by this profile")));
        //DH1KLM: LEDs follow commanded state on every poll, even while a table edit is pending.
        const auto lightRows = [](QTableWidget* table, const QStringList& names, int activeRow) {
            for (int row = 0; row < table->rowCount(); ++row) {
                QWidget* cell = table->cellWidget(row,0);
                cell->findChild<QLabel*>(QStringLiteral("filterBandName"))->setText(names[row]);
                updateFilterLed(cell->findChild<QLabel*>(QStringLiteral("ananFilterLed")), row == activeRow, names[row]);
            }
        };
        lightRows(receive,rxNames,data.value(QStringLiteral("receiveActive")).toInt());
        lightRows(receiveAdc1,rxNames,showAlex2 ? data.value(QStringLiteral("adc1Active")).toInt() : -1);
        updateFilterLed(adc1BypassLed, showAlex2 && data.value(QStringLiteral("adc1Active")).toInt() < 0,
                        QStringLiteral("ADC1 receive bypass"));
        lightRows(lpf,lpNames,lpEnabled ? data.value(QStringLiteral("lpfActive")).toInt() : -1);
        updateFilterLed(bypassLed, data.value(QStringLiteral("receiveActive")).toInt() < 0,
                        QStringLiteral("ADC0 receive bypass"));
        adc0->setEnabled(true);
        disable6mRx->setEnabled(true);
        lpfBypassRx->setEnabled(lpEnabled);
        lpfBypassRx->setAccessibleDescription(lpEnabled ? QStringLiteral("Force 6m LPF after Apply")
            : QStringLiteral("Unavailable: this profile does not drive receive LPF selection"));
        adc1->setEnabled(twoAdcs);
        adc1->setAccessibleDescription(twoAdcs
            ? QStringLiteral("Bypass the separate ADC1/RX2 receive filter bank; select Apply to save and activate")
            : QStringLiteral("This profile has only one ADC"));
        receive->setEnabled(true);
        receive->setAccessibleDescription(QStringLiteral("Ordered, non-overlapping frequency ranges in MHz"));
        lpf->setEnabled(lpEnabled);
        lpf->setAccessibleDescription(lpEnabled ? QStringLiteral("Ordered, non-overlapping frequency ranges in MHz") : QStringLiteral("LPF control is not enabled for this receive profile"));
        apply->setEnabled(true);
        apply->setAccessibleDescription(QStringLiteral("Save and apply the edited receive filter settings"));
        defaults->setEnabled(true);
        defaults->setAccessibleDescription(QStringLiteral("Save and apply Thetis-compatible defaults for this model"));
        bool editing = false;
        for (QTableWidget* table : {receive, receiveAdc1, lpf}) {
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int col = 1; col < 3; ++col) {
                    QWidget* cell = table->cellWidget(row, col);
                    editing = editing || cell->hasFocus() || cell->isAncestorOf(QApplication::focusWidget());
                }
            }
        }
        if (!state->dirty && (!editing || changed)) {
            state->filling = true;
            {
                const QSignalBlocker block(rxInput);
                if (changed || rxInput->count() != inputNames.size()) {
                    rxInput->clear();
                    for (int n = 0; n < inputNames.size(); ++n) { rxInput->addItem(inputNames[n], n); }
                }
                rxInput->setCurrentIndex(input);
            }
            ocEnabled->setChecked(data.value(QStringLiteral("ocEnabled")).toBool());
            const auto ocRows = data.value(QStringLiteral("ocReceive")).toList();
            for (int row = 0; row < 11; ++row) {
                auto value = ocRows[row].toMap();
                if (value.value(QStringLiteral("lowHz")).toLongLong() == value.value(QStringLiteral("highHz")).toLongLong()) {
                    const auto defaults = (*ocDefaultRanges)[row].toMap();
                    value.insert(QStringLiteral("lowHz"), defaults.value(QStringLiteral("lowHz")));
                    value.insert(QStringLiteral("highHz"), defaults.value(QStringLiteral("highHz")));
                }
                ocRx->bands[row]->setProperty("lowHz", value.value(QStringLiteral("lowHz")));
                ocRx->bands[row]->setProperty("highHz", value.value(QStringLiteral("highHz")));
                ocRx->bands[row]->setToolTip(QStringLiteral("%1 – %2 MHz").arg(value.value(QStringLiteral("lowHz")).toDouble()/1e6, 0, 'f', 6)
                    .arg(value.value(QStringLiteral("highHz")).toDouble()/1e6, 0, 'f', 6));
                ocRx->bands[row]->setAccessibleDescription(ocRx->bands[row]->toolTip());
                const int mask = value.value(QStringLiteral("mask")).toInt();
                for (int pin = 0; pin < 7; ++pin) {
                    auto* check = ocRx->pins[row][pin];
                    const bool available = value.value(QStringLiteral("lowHz")).toLongLong() < value.value(QStringLiteral("highHz")).toLongLong();
                    check->setEnabled(available);
                    check->setAccessibleDescription(available ? QStringLiteral("Receive frequency range: %1").arg(ocRx->bands[row]->toolTip())
                        : QStringLiteral("Unavailable: this band has no frequency range in the active band plan"));
                    check->setChecked(mask & (1<<pin));
                }
            }
            disable6mRx->setChecked(data.value(QStringLiteral("disable6mLnaOnRx")).toBool());
            lpfBypassRx->setChecked(data.value(QStringLiteral("lpfBypassOnRx")).toBool());
            const auto bypasses = data.value(QStringLiteral("receiveBypass")).toList();
            for (int row = 0; row < 6; ++row) {
                qobject_cast<QCheckBox*>(receive->cellWidget(row,3))->setChecked(bypasses[row].toBool());
            }
            adc0->setChecked(data.value(QStringLiteral("bypassAdc0")).toBool());
            adc1->setChecked(data.value(QStringLiteral("bypassAdc1")).toBool());
            const auto populate = [](QTableWidget* table, const QVariantList& rows, const QStringList& names) {
                for (int row = 0; row < table->rowCount(); ++row) {
                    //DH1KLM: The cell widget paints the name; the item supplies
                    // accessibility metadata only, avoiding text underneath the LED.
                    table->item(row, 0)->setData(Qt::AccessibleTextRole, names[row]);
                    for (int col = 1; col < 3; ++col) {
                        auto* spin = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, col));
                        spin->setValue(rows[row].toMap().value(col == 1 ? QStringLiteral("lowHz") : QStringLiteral("highHz")).toDouble() / 1e6);
                    }
                }
            };
            populate(receive, data.value(QStringLiteral("receive")).toList(), rxNames);
            populate(receiveAdc1, data.value(QStringLiteral("receiveAdc1"), data.value(QStringLiteral("receive"))).toList(), rxNames);
            populate(lpf, data.value(QStringLiteral("lowPass")).toList(), lpNames);
            state->filling = false;
        }
    };
    const auto dirty = [=] { if (!state->filling) { state->dirty = true; status->setText(QStringLiteral("Changes pending — select Apply to update the receiver.")); } };
    connect(disable6mRx, &QCheckBox::toggled, page, dirty);
    connect(lpfBypassRx, &QCheckBox::toggled, page, dirty);
    connect(rxInput, &QComboBox::currentIndexChanged, page, dirty);
    for (int row = 0; row < 6; ++row) {
        connect(qobject_cast<QCheckBox*>(receive->cellWidget(row,3)), &QCheckBox::toggled, page, dirty);
    }
    connect(ocEnabled, &QCheckBox::toggled, page, dirty);
    connect(ocReset, &QPushButton::clicked, page, [=] {
        ocEnabled->setChecked(false);
        for (const auto& band : ocRx->pins) {
            for (auto* check : band) { check->setChecked(false); }
        }
        dirty();
    });
    for (int row = 0; row < 11; ++row) {
        for (int pin = 0; pin < 7; ++pin) {
            connect(ocRx->pins[row][pin], &QCheckBox::toggled, page, dirty);
        }
    }
    connect(adc0, &QCheckBox::toggled, page, dirty);
    connect(adc1, &QCheckBox::toggled, page, dirty);
    for (QTableWidget* table : {receive, receiveAdc1, lpf}) {
        for (int row = 0; row < table->rowCount(); ++row) {
            for (int col = 1; col < 3; ++col) {
                connect(qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, col)),
                    &QDoubleSpinBox::valueChanged, page, dirty);
            }
        }
    }
    const auto request = [=, this](const QString& verb, const QVariant& options) {
        IRadioBackend* backend = m_model->backend();
        if (!backend || !m_model->backendDeclaresExtension(QStringLiteral("anan"))) { unavailable(); return; }
        const QVariantMap target = state->current;
        const auto sameAntennaTarget = [state, target] {
            for (const QString& key : {QStringLiteral("identity"), QStringLiteral("modelId"), QStringLiteral("boardId")}) {
                if (state->current.value(key) != target.value(key)) { return false; }
            }
            return true;
        };
        //DH1KLM: A fresh receiver-bound guard prevents replies from a previous session repainting this page.
        auto* guard = new QObject(page);
        static quint64 nextId = 0x414e414e0000ULL;
        const quint64 id = ++nextId;
        connect(backend, &IRadioBackend::extensionResult, guard, [=, this](quint64 replyId, const QVariant& result) {
            if (replyId != id) { return; }
            guard->deleteLater();
            if (m_model->backend() != backend) { return; }
            if (verb == QLatin1String("ctun.set")) {
                if (!sameAntennaTarget()) { return; }
                fill(result.toMap());
                status->setText(state->dirty
                    ? QStringLiteral("CTUN applied. Filter edits are still pending Apply.")
                    : QStringLiteral("CTUN applied to this receive session."));
                return;
            }
            if (verb == QLatin1String("rx-antenna.set")) {
                if (!sameAntennaTarget()) { return; }
                //DH1KLM: Antenna replies contain only the selected input; retain
                // the full hardware snapshot and any un-applied filter edits.
                QVariantMap current = state->current;
                current.insert(QStringLiteral("antenna"), result.toMap().value(QStringLiteral("antenna")));
                fill(current);
                status->setText(QStringLiteral("RX antenna saved for this device and model; applied to ADC0."));
                return;
            }
            if (verb != QLatin1String("hw.get")) {
                state->dirty = false;
                status->setText(QStringLiteral("Saved for this device and model; applied to the receiver."));
            }
            fill(result.toMap());
        });
        connect(backend, &IRadioBackend::extensionError, guard, [=, this](quint64 replyId, const QString& error) {
            if (replyId != id) { return; }
            guard->deleteLater();
            if (m_model->backend() == backend) {
                if (verb == QLatin1String("rx-antenna.set")) {
                    if (!sameAntennaTarget()) { return; }
                    const QSignalBlocker block(antennaCombo);
                    antennaCombo->setCurrentIndex(state->current.value(QStringLiteral("antenna")).toInt() - 1);
                }
                if (verb == QLatin1String("ctun.set")) {
                    if (!sameAntennaTarget()) { return; }
                    const QSignalBlocker block(ctun);
                    ctun->setChecked(state->current.value(QStringLiteral("ctun")).toBool());
                }
                status->setText(error);
            }
        });
        QTimer::singleShot(1500, guard, &QObject::deleteLater);
        m_model->invokeBackendExtension(QStringLiteral("anan"), verb, id, options);
    };
    connect(ctun, &QCheckBox::toggled, page, [=](bool enabled) {
        request(QStringLiteral("ctun.set"), enabled);
    });
    connect(antennaCombo, &QComboBox::currentIndexChanged, page, [=](int) {
        request(QStringLiteral("rx-antenna.set"), antennaCombo->currentData());
    });
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
        QVariantList ocRows;
        for (int row = 0; row < 11; ++row) {
            int mask = 0;
            for (int pin = 0; pin < 7; ++pin) {
                if (ocRx->pins[row][pin]->isChecked()) { mask |= 1<<pin; }
            }
            ocRows.append(QVariantMap{{QStringLiteral("lowHz"), ocRx->bands[row]->property("lowHz")},
                {QStringLiteral("highHz"), ocRx->bands[row]->property("highHz")}, {QStringLiteral("mask"), mask}});
        }
        options.insert(QStringLiteral("rxOnlyInput"), rxInput->currentData());
        options.insert(QStringLiteral("ocEnabled"), ocEnabled->isChecked());
        options.insert(QStringLiteral("ocReceive"), ocRows);
        QVariantList bypasses;
        for (int row = 0; row < 6; ++row) {
            bypasses.append(qobject_cast<QCheckBox*>(receive->cellWidget(row,3))->isChecked());
        }
        options.insert(QStringLiteral("receiveBypass"), bypasses);
        options.insert(QStringLiteral("disable6mLnaOnRx"), disable6mRx->isChecked());
        options.insert(QStringLiteral("lpfBypassOnRx"), lpfBypassRx->isChecked());
        options.insert(QStringLiteral("receive"), rows(receive));
        if (state->current.value(QStringLiteral("hasAlex2Filters")).toBool()) {
            options.insert(QStringLiteral("receiveAdc1"), rows(receiveAdc1));
        }
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
