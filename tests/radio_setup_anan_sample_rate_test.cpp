#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/AppSettings.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

using namespace AetherSDR;
namespace AetherSDR::anan {
class AnanLinkStatsTestAccess {
public:
    static void prepare(AnanBackend& backend, int protocol, bool redPitaya)
    {
        backend.m_connected = true;
        backend.m_discoveryInfoReceived = true;
        backend.m_discoveredBoardId = 5;
        backend.m_radioSerial = QStringLiteral("sample-rate-ui-test-%1-%2").arg(protocol).arg(redPitaya);
        backend.m_pendingParams.protocol = protocol;
        backend.m_pendingParams.redPitayaP1 = redPitaya;
        backend.m_pendingParams.model = redPitaya ? AnanModel::RedPitaya : AnanModel::Anan7000;
        backend.m_pendingParams.ddc0RateKsps = 48;
        backend.m_pendingDspConfig.inputSampleRateHz = 48000;
        backend.m_sliceFreqHz = backend.m_ddcCenterHz = 14'175'000;
        backend.m_receiveLayout.reset(5, backend.m_sliceFreqHz, 48);
        backend.m_streamApplyOverride = [](const auto&, const auto&) { return true; };
        backend.loadHardwareFilters(5);
    }
    static void publish(AnanBackend& backend) { backend.emitPanState(); backend.emitSliceState(); }
    static void connected(AnanBackend& backend, bool connected) { backend.m_connected = connected; }
    static void calibrationMode(AnanBackend& backend)
    {
        backend.m_mode = QStringLiteral("AM");
        backend.m_filterLowHz = -500; backend.m_filterHighHz = 500;
    }
    static void measurement(AnanBackend& backend, int binOffset, bool alterContext = false)
    {
        if (!backend.m_receiveCalibrationRun) { return; }
        //DH1KLM Inject recorded raw meter power and FFT frames; no peer or radio traffic.
        backend.m_receiveCalibrationRun->meterPower = 10*std::pow(10.0,-90.0/10.0);
        backend.m_receiveCalibrationRun->meterSamples = 10;
        std::vector<float> bins(1024,-120);
        const int index = 512+binOffset;
        bins[index-1] = -80; bins[index] = -60; bins[index+1] = -80;
        if (alterContext) { backend.m_ddcCenterHz += 1000; }
        for (int n = 0; n < 10; ++n) { backend.acceptCalibrationSpectrum(bins, 2000); }
        if (alterContext) { backend.m_ddcCenterHz -= 1000; }
    }
    static auto calibration(const AnanBackend& backend) { return backend.m_receiveCalibration; }
    static bool calibrating(const AnanBackend& backend) { return backend.m_receiveCalibrationRun.has_value(); }
};
}

void check(bool ok, const char* reason)
{
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-rate-ui"));
    QApplication app(argc, argv);
    AppSettings::instance().load();
    for (const auto [protocol, redPitaya, count] : {std::tuple{2, false, 6}, {1, false, 3}, {1, true, 4}}) {
        RadioModel model;
        auto owned = std::make_unique<anan::AnanBackend>();
        auto* backend = owned.get();
        anan::AnanLinkStatsTestAccess::prepare(*backend, protocol, redPitaya);
        model.setBackendForTest(std::move(owned), QStringLiteral("anan"));
        QSignalSpy replies(backend, &IRadioBackend::extensionResult);
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("ANAN Hardware"));
        auto* rate = dialog.findChild<QComboBox*>(QStringLiteral("ananSampleRate"));
        auto* window = dialog.findChild<QComboBox*>(QStringLiteral("ananSampleRateWindow"));
        check(rate && window, "manual sample rate controls exist on ANAN Hardware");
        check(rate->count() == count && rate->currentData().toInt() == 48000 && rate->isEnabled(),
              "manual control uses protocol/profile rates and displays the live capture rate");
        check(window->count() == 1 && window->currentData() == QStringLiteral("anan-0"),
              "single capture is explicitly selected");
        auto* swlPin = dialog.findChild<QCheckBox*>(QStringLiteral("ananSwl4Pin7"));
        check(swlPin && swlPin->isEnabled(), "SWL RX pin controls available for supported P1/P2 profiles");
        swlPin->setChecked(true);
        for (int n = 0; n < 10; ++n) { app.processEvents(); QThread::msleep(5); }
        check(!replies.isEmpty(), "SWL hw.set returns a backend reply");
        const auto swlReply = replies.last().at(1).toMap();
        check(swlReply.value(QStringLiteral("ocSwl")).toList().value(4).toMap()
            .value(QStringLiteral("mask")).toInt() == 64, "SWL pin edit reaches backend through hw.set");
        //DH1KLM Independent manual pins combine without changing the HF/SWL master.
        auto* digital1 = dialog.findChild<QCheckBox*>(QStringLiteral("ananUserDigitalPin1"));
        auto* digital4 = dialog.findChild<QCheckBox*>(QStringLiteral("ananUserDigitalPin4"));
        check(digital1 && digital4 && digital1->isEnabled() && digital4->isEnabled(),
              "manual digital controls available for P1/P2");
        digital1->click();
        digital4->click();
        for (int n = 0; n < 10; ++n) { app.processEvents(); QThread::msleep(5); }
        check(replies.last().at(1).toMap().value(QStringLiteral("userDigitalOutputs")).toInt() == 9,
              "manual digital edits retain multiple selected pins");
        QVariantMap legacyFields = replies.last().at(1).toMap();
        legacyFields.remove(QStringLiteral("userDigitalOutputs"));
        backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("hw.set"), 9100, legacyFields);
        check(replies.last().at(1).toMap().value(QStringLiteral("userDigitalOutputs")).toInt() == 9,
              "legacy filter callers preserve manual digital outputs");
        auto* bandEnable = dialog.findChild<QCheckBox*>(QStringLiteral("ananBandAntennasEnabled"));
        auto* ant2 = dialog.findChild<QRadioButton*>(QStringLiteral("ananBand5Ant2"));
        auto* firstInput = dialog.findChild<QCheckBox*>(QStringLiteral("ananBand5Input1"));
        auto* secondInput = dialog.findChild<QCheckBox*>(QStringLiteral("ananBand5Input2"));
        auto* xvtr = dialog.findChild<QCheckBox*>(QStringLiteral("ananBand5Input3"));
        auto* twoMeters = dialog.findChild<QRadioButton*>(QStringLiteral("ananBand11Ant1"));
        check(bandEnable && ant2 && firstInput && secondInput && xvtr && twoMeters,
              "band antenna controls expose independent bank radios and RX-only checkboxes");
        check(bandEnable->isEnabled() && !bandEnable->isChecked() && ant2->isEnabled()
              && !xvtr->isEnabled() && !xvtr->accessibleDescription().isEmpty()
              && !twoMeters->isEnabled() && !twoMeters->accessibleDescription().isEmpty(),
              "manual mode is preserved; unsupported 2m and transverter modes have announced reasons");
        const auto settle = [&] {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 50) { QCoreApplication::processEvents(); QThread::msleep(1); }
        };
        //DH1KLM Both protocols expose the same fenced RX calibration path and persisted factor.
        anan::AnanLinkStatsTestAccess::calibrationMode(*backend);
        auto* freqReference = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ananFrequencyCalReference"));
        auto* freqStart = dialog.findChild<QPushButton*>(QStringLiteral("ananFrequencyCalStart"));
        auto* levelReference = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ananLevelCalReference"));
        auto* levelStart = dialog.findChild<QPushButton*>(QStringLiteral("ananLevelCalStart"));
        auto* levelReset = dialog.findChild<QPushButton*>(QStringLiteral("ananLevelCalReset"));
        check(freqReference && freqStart && levelReference && levelStart && levelReset && freqStart->isEnabled(),
              "calibration controls exist and are available for connected P1/P2");
        freqReference->setValue(14175000);
        freqStart->click();
        check(anan::AnanLinkStatsTestAccess::calibrating(*backend), "frequency measurement starts through GUI extension");
        anan::AnanLinkStatsTestAccess::measurement(*backend,2);
        const double factor = anan::AnanLinkStatsTestAccess::calibration(*backend).frequencyFactor;
        check(std::abs(factor-(1+93.75/14175000)) < 1e-10, "frequency measurement computes and persists reference correction");
        emit model.connectionStateChanged(true);
        app.processEvents();
        levelReference->setValue(14175000);
        levelStart->click();
        check(anan::AnanLinkStatsTestAccess::calibrating(*backend), "level measurement starts through GUI extension");
        anan::AnanLinkStatsTestAccess::measurement(*backend,0);
        const auto cal = anan::AnanLinkStatsTestAccess::calibration(*backend);
        check(cal.levelCalibrated[0] && std::abs(cal.meterOffsetDb[0]-17) < 0.001
              && std::abs(cal.displayOffsetDb[0]+13) < 0.001, "meter and spectrum receive separate reference offsets");
        emit model.connectionStateChanged(true);
        app.processEvents();
        levelReset->click();
        check(!anan::AnanLinkStatsTestAccess::calibration(*backend).levelCalibrated[0]
              && anan::AnanLinkStatsTestAccess::calibration(*backend).frequencyFactor == factor,
              "level reset preserves oscillator correction");
        freqStart->click();
        anan::AnanLinkStatsTestAccess::measurement(*backend,2,true);
        check(!anan::AnanLinkStatsTestAccess::calibrating(*backend)
              && anan::AnanLinkStatsTestAccess::calibration(*backend).frequencyFactor == factor,
              "receive-context changes discard measurements without overwriting saved calibration");
        ant2->click();
        settle();
        firstInput->click();
        settle();
        check(ant2->isChecked() && firstInput->isChecked(), "RX-only selection retains the independently saved ANT bank");
        secondInput->click();
        settle();
        check(!firstInput->isChecked() && secondInput->isChecked() && ant2->isChecked(),
              "external inputs exclude each other without clearing ANT2");
        secondInput->click();
        settle();
        check(!firstInput->isChecked() && !secondInput->isChecked(), "RX-only can be explicitly deselected");
        bandEnable->click();
        settle();
        check(bandEnable->isChecked() && ant2->isChecked(), "enabling per-band routing retains the confirmed table");
        auto* manualBank = dialog.findChild<QComboBox*>(QStringLiteral("ananRxAntennaBank"));
        auto* manualInput = dialog.findChild<QComboBox*>(QStringLiteral("ananRxInput"));
        check(manualBank && manualInput && !manualBank->isEnabled() && !manualInput->isEnabled()
              && manualBank->currentData().toInt() == 2,
              "current controls display the effective band antenna and explain why manual edits are disabled");
        bandEnable->click();
        settle();
        check(manualBank->isEnabled() && manualInput->isEnabled() && manualBank->currentData().toInt() == 1,
              "disabling band routing restores the separately saved manual bank");
        bandEnable->click();
        settle();
        replies.clear();
        const int index = rate->currentIndex();
        QMetaObject::invokeMethod(rate, "activated", Q_ARG(int, index));
        check(replies.count() == 1 && rate->currentData().toInt() == 48000,
              "operator activation reaches the real backend without requiring reconnect");
        anan::AnanLinkStatsTestAccess::connected(*backend, false);
        emit model.connectionStateChanged(false);
        check(!bandEnable->isEnabled() && !ant2->isEnabled(), "disconnect disables the band antenna controls");
        check(!digital1->isEnabled() && !digital4->isEnabled() && !digital1->isChecked()
              && !digital1->accessibleDescription().isEmpty(), "disconnect clears and disables manual digital controls");
        check(!rate->isEnabled() && rate->count() == 0 && !rate->accessibleDescription().isEmpty(),
              "disconnect clears and disables the control with an announced reason");
        anan::AnanLinkStatsTestAccess::connected(*backend, true);
        emit model.connectionStateChanged(true);
        check(rate->isEnabled() && rate->count() == count && window->count() == 1,
              "reconnect repopulates an unchanged window and rate list");
        if (protocol == 2) {
            model.setLineoutGain(31);
            model.setLineoutMute(true);
            check(backend->lineoutGainPercentForTest() == 31 && backend->lineoutMutedForTest(),
                  "typed setup routing retains ANAN's native lineout gain and mute");
            anan::AnanLinkStatsTestAccess::publish(*backend);
            check(model.slices().size() == 1 && !model.panId().isEmpty(),
                  "real ANAN observations establish the original receiver and pan");
            check(model.addSliceOnPan(model.panId(), 14.178),
                  "typed model lifecycle reaches ANAN's native shared-slice adapter");
            QElapsedTimer deadline;
            deadline.start();
            while (model.slices().size() != 2 && deadline.elapsed() < 10000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            check(model.slices().size() == 2 && model.slice(1),
                  "ANAN DSP completion still publishes the new receiver through the model");
            check(model.removeSlice(1) && model.slices().size() == 1 && model.slice(0),
                  "typed removal retains the original ANAN receiver");
        }
    }
}
