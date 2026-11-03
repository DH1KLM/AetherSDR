#include "gui/AnanControllersPage.h"
#include "models/FrontPanelModel.h"
#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan/AnanImplementation.h"
#include "core/AppSettings.h"
#include "gui/RadioSetupDialog.h"
#include "gui/AnanTransmitPowerPage.h"
#include <QTableWidget>
#include <QLabel>
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QEvent>
#include <QRadioButton>
#include <QDoubleSpinBox>
#include <QDir>
#include <QPixmap>
#include <QPushButton>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <QSemaphore>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>
#include <atomic>
#include <functional>

namespace AetherSDR {
class FrontPanelTestAccess {
public:
    static void prepare(FrontPanelModel& panel, const QString& action) {
        panel.m_retry.stop();
        QObject::disconnect(&panel.m_model, &RadioModel::capabilitiesChanged, &panel, nullptr);
        QCoreApplication::removePostedEvents(&panel, QEvent::MetaCall);
        panel.m_identified = true;
        panel.m_bindings.insert(QStringLiteral("p44"), QJsonObject{
            {QStringLiteral("action"), action}, {QStringLiteral("target"), 0}});
    }
    static void feed(FrontPanelModel& panel, const QByteArray& bytes) { panel.consume(bytes); }
    static void close(FrontPanelModel& panel) { panel.closeTransport(); }
};
}
using namespace AetherSDR;
namespace AetherSDR::anan {
struct TxSessionTrace {
    std::atomic<int> keyOn{0}, lastKey{0}, drive{0}, primaryAdc{-1}, keyReleases{0};
    std::atomic<quint32> frequencyWord{0}, alex{0};
};
struct AnanClientTestAccess {
    static bool configured(const AnanClient& client) { return client.m_transmitConfigured; }
    static P2Client& p2(AnanClient& client) { return *client.m_p2; }
};
struct P2ClientTestAccess {
    static void attach(P2Client& client, TxSessionTrace& trace) {
        client.m_running = true; client.m_linkUp = true; client.m_discoveryValidated = true;
        client.m_expectedBoardId = 5; client.m_model = AnanModel::Anan200D; client.m_overrideDetection = true;
        client.m_numAdcs = 2; client.m_firstDdcIndex = 2;
        client.m_streamLimit = 5;
        client.m_ddcRateSendOverride = [&trace](const auto& packet) {
            trace.primaryAdc.store(packet[17+6*2]);
            return qint64(packet.size());
        };
        client.m_receiveFilterKind = ReceiveFilterKind::LegacyHpf;
        client.m_activeDdcs = {{48,0}}; client.m_activeDdcCount = 1;
        client.m_transmitProfile = p2TransmitProfile(5,AnanModel::Anan200D,true);
        client.m_generalPacket = buildGeneral(3,true); client.m_receiveDisablePa = true;
        client.m_controlSendOverride = [&trace](auto bytes, quint16 port) {
            if (port == kHighPriorityPort && bytes.size() == 1444) {
                const bool key = (bytes[4] & 2) != 0;
                if (trace.lastKey.exchange(key) == 1 && !key) { ++trace.keyReleases; }
                if (key) { ++trace.keyOn; }
                trace.drive.store(bytes[345]);
                const auto word = [&](int offset) { return (quint32(bytes[offset])<<24) | (quint32(bytes[offset+1])<<16)
                    | (quint32(bytes[offset+2])<<8) | bytes[offset+3]; };
                trace.frequencyWord.store(word(329)); trace.alex.store(word(1432));
            }
            return static_cast<qint64>(bytes.size());
        };
    }
};
class AnanLinkStatsTestAccess {
public:
    static void prepare(AnanBackend& backend, int protocol, bool redPitaya, int board = 5,
                        AnanModel model = AnanModel::Automatic, bool overrideDetection = false)
    {
        backend.m_connected = true;
        backend.m_discoveryInfoReceived = true;
        backend.m_discoveredBoardId = board;
        backend.m_pendingParams.expectedBoardId = board;
        backend.m_radioSerial = QStringLiteral("sample-rate-ui-test-%1-%2").arg(protocol).arg(redPitaya);
        backend.m_pendingParams.protocol = protocol;
        backend.m_pendingParams.overrideDetection = overrideDetection;
        backend.m_pendingParams.redPitayaP1 = redPitaya;
        backend.m_pendingParams.model = redPitaya ? AnanModel::RedPitaya
            : board == 10 ? AnanModel::AnanG2 : AnanModel::Anan7000;
        if (model != AnanModel::Automatic) { backend.m_pendingParams.model = model; }
        backend.m_pendingParams.ddc0RateKsps = 48;
        backend.m_pendingDspConfig.inputSampleRateHz = 48000;
        backend.m_sliceFreqHz = backend.m_ddcCenterHz = 14'175'000;
        backend.m_receiveLayout.reset(5, backend.m_sliceFreqHz, 48);
        backend.m_streamApplyOverride = [](const auto&, const auto&) { return true; };
        backend.loadHardwareFilters(board);
    }
    static void blockPlanner(AnanBackend& backend, QSemaphore& entered, QSemaphore& release)
    {
        QMetaObject::invokeMethod(backend.m_dspBuildContext,[&entered,&release] { entered.release(); release.acquire(); },Qt::QueuedConnection);
    }
    static bool plannedChannel(AnanBackend& backend)
    {
        bool present = false;
        QMetaObject::invokeMethod(backend.m_dspBuildContext,[] {},Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(backend.m_client,[&] { present = backend.m_client->transmitMode().has_value(); },Qt::BlockingQueuedConnection);
        return present;
    }
    static bool planReady(const AnanBackend& backend) { return backend.m_transmitPlanReady; }
    static bool receiveBusy(const AnanBackend& backend) { return backend.m_receiveBuilding; }
    static void forgetViewBand(AnanBackend& backend) { backend.m_currentViewBand.clear(); }
    static double center(const AnanBackend& backend) { return backend.m_ddcCenterHz; }
    static bool ctun(const AnanBackend& backend) { return backend.m_ctun; }
    static double savedGain(const AnanBackend& backend) {
        return AnanSettings::transmitPower(backend.m_radioSerial,static_cast<int>(backend.m_pendingParams.model),
            backend.m_discoveredBoardId,backend.m_pendingParams.protocol).value(QStringLiteral("gainsDb")).toList().at(5).toDouble();
    }
    static bool transportPrepared(AnanBackend& backend)
    {
        bool present = false;
        QMetaObject::invokeMethod(backend.m_client,[&] { present = AnanClientTestAccess::configured(*backend.m_client); },Qt::BlockingQueuedConnection);
        return present;
    }
    static void injectTransport(AnanBackend& backend, TxSessionTrace& trace) {
        backend.m_streamApplyOverride = {};
        QMetaObject::invokeMethod(backend.m_client,[&] {
            P2ClientTestAccess::attach(AnanClientTestAccess::p2(*backend.m_client),trace);
        },Qt::BlockingQueuedConnection);
    }
    static void blockIo(AnanBackend& backend, QSemaphore& entered, QSemaphore& release) {
        QMetaObject::invokeMethod(backend.m_client,[&] { entered.release(); release.acquire(); },Qt::QueuedConnection);
    }
    static void drainIo(AnanBackend& backend) {
        QMetaObject::invokeMethod(backend.m_client,[] {},Qt::BlockingQueuedConnection);
    }
    static QString sessionDiagnostic(AnanBackend& backend) {
        const auto stored = AnanSettings::transmitPower(backend.m_radioSerial,static_cast<int>(backend.m_pendingParams.model),
            backend.m_discoveredBoardId,backend.m_pendingParams.protocol);
        const auto limit = stored.value(QStringLiteral("limitsWatts")).toList().value(5);
        return QStringLiteral("PA diagnostic: band=%1 VFO=%2 stored20m=%3 type=%4 parsed=%5")
            .arg(backend.receiveAntennaBandIndex()).arg(backend.m_sliceFreqHz,0,'f',0)
            .arg(limit.toDouble()).arg(QString::fromLatin1(limit.metaType().name())).arg(parseTransmitPower(stored).has_value());
    }
    static void publish(AnanBackend& backend) { backend.emitPanState(); backend.emitSliceState(); }
    static void connected(AnanBackend& backend, bool connected) { backend.m_connected = connected; }
    static void restoreTx(AnanBackend& backend) { backend.restoreTransmitSession(); }
    static void meters(AnanBackend& backend) { backend.defineMeters(); }
    static void retireTx(AnanBackend& backend) { backend.cancelTransmitPreparation(); }
    static void faultTx(AnanBackend& backend) {
        QMetaObject::invokeMethod(backend.m_client,[client = backend.m_client] {
            emit client->transmitFault(QStringLiteral("Injected transport interruption"));
        },Qt::QueuedConnection);
    }
    static void calibrationMode(AnanBackend& backend)
    {
        backend.m_mode = QStringLiteral("AM");
        backend.m_filterLowHz = -500; backend.m_filterHighHz = 500;
    }
    static void measurement(AnanBackend& backend, int binOffset, bool alterContext = false)
    {
        if (!backend.m_receiveCalibrationRun) { return; }
        // Inject recorded raw meter power and FFT frames; no peer or radio traffic.
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
    for (const auto [protocol,board,modelId,red,first,second,adcs] : {
        std::tuple{1,5,anan::AnanModel::RedPitaya,true,0,2,2},
        std::tuple{1,4,anan::AnanModel::Anan200D,false,0,2,2},
        std::tuple{1,1,anan::AnanModel::Hermes,false,0,1,1},
        std::tuple{2,3,anan::AnanModel::Anan100D,false,2,3,2},
        std::tuple{2,4,anan::AnanModel::Anan200D,false,2,3,2},
        std::tuple{2,5,anan::AnanModel::AnvelinaPro3,false,2,3,2},
        std::tuple{1,3,anan::AnanModel::Anan100D,false,0,2,2},
        std::tuple{1,5,anan::AnanModel::Anan7000,false,0,2,2},
        std::tuple{1,5,anan::AnanModel::Anan8000,false,0,2,2},
        std::tuple{1,5,anan::AnanModel::AnvelinaPro3,false,0,2,2},
        std::tuple{2,5,anan::AnanModel::Anan7000,false,2,3,2},
        std::tuple{2,5,anan::AnanModel::Anan8000,false,2,3,2},
        std::tuple{2,1,anan::AnanModel::Hermes,false,0,1,1},
        std::tuple{1,1,anan::AnanModel::Anan10,false,0,1,1},
        std::tuple{2,1,anan::AnanModel::Anan10,false,0,1,1},
        std::tuple{1,2,anan::AnanModel::Anan10E,false,0,1,1},
        std::tuple{2,2,anan::AnanModel::Anan10E,false,0,1,1},
        std::tuple{1,1,anan::AnanModel::Anan100,false,0,1,1},
        std::tuple{2,1,anan::AnanModel::Anan100,false,0,1,1},
        std::tuple{1,2,anan::AnanModel::Anan100B,false,0,1,1},
        std::tuple{2,2,anan::AnanModel::Anan100B,false,0,1,1},
        std::tuple{2,10,anan::AnanModel::AnanG2,false,0,1,2},
        std::tuple{2,10,anan::AnanModel::AnanG21K,false,0,1,2},
        std::tuple{2,20,anan::AnanModel::AnanG2E,false,0,1,1},
        std::tuple{2,5,anan::AnanModel::RedPitaya,false,2,3,2}}) {
        RadioModel receiverModel;
        auto owned = std::make_unique<anan::AnanBackend>();
        auto* backend = owned.get();
        anan::AnanLinkStatsTestAccess::prepare(*backend,protocol,red,board,modelId);
        receiverModel.setBackendForTest(std::move(owned),QStringLiteral("anan"));
        check(backend->createPanadapter(),"ADC diagram fixture creates a second independent capture");
        QElapsedTimer pending; pending.start();
        while (anan::AnanLinkStatsTestAccess::receiveBusy(*backend) && pending.elapsed() < 5000) { app.processEvents(); QThread::msleep(5); }
        check(!anan::AnanLinkStatsTestAccess::receiveBusy(*backend),"ADC diagram captures are ready");
        QSignalSpy replies(backend,&IRadioBackend::extensionResult);
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.get"),93001,{});
        const auto snapshot = replies.last().at(1).toMap();
        const auto mapping = snapshot.value(QStringLiteral("ddcMap")).toList();
        check(mapping[0].toInt() == first && mapping[1].toInt() == second,
            "model/protocol RX1 and RX2 positions match Thetis or the retained G2 bench route");
        std::unique_ptr<QWidget> diagram(createAnanAdcPage(&receiverModel,nullptr));
        diagram->resize(1000,500); diagram->show();
        auto* rx1 = diagram->findChild<QLabel*>(QStringLiteral("ananDdc%1Receivers").arg(first));
        auto* rx2 = diagram->findChild<QLabel*>(QStringLiteral("ananDdc%1Receivers").arg(second));
        pending.restart();
        while ((!rx1->text().contains(QStringLiteral("RX1")) || !rx2->text().contains(QStringLiteral("RX2"))) && pending.elapsed() < 2500) { app.processEvents(); QThread::msleep(5); }
        check(rx1->text().contains(QStringLiteral("RX1 (Slice A)")) && rx2->text().contains(QStringLiteral("RX2 (Slice B)")),
            "graphical RX cards sit below their actual physical DDC columns");
        auto* adc0 = diagram->findChild<QRadioButton*>(QStringLiteral("ananDdc%1Adc0").arg(second));
        adc0->click();
        check(rx2->text().contains(QStringLiteral("ADC0")),"physical DDC radio button updates the correct RX card");
        diagram->findChild<QPushButton*>(QStringLiteral("ananAdcReset"))->click();
        check(rx1->text().contains(QStringLiteral("ADC0")) && rx2->text().contains(adcs == 2 ? QStringLiteral("ADC1") : QStringLiteral("ADC0")),
            "Thetis reset restores model-specific RX1/RX2 ADC defaults");
        const QString shots = qEnvironmentVariable("AETHER_ADC_SCREENSHOT_DIR");
        if (!shots.isEmpty() && ((red && protocol == 1) || (modelId == anan::AnanModel::AnvelinaPro3 && protocol == 2))) {
            check(QDir().mkpath(shots),"ADC screenshot directory exists");
            app.processEvents();
            check(diagram->grab().save(QDir(shots).filePath(red ? QStringLiteral("red-pitaya-p1.png") : QStringLiteral("anvelina-p2.png"))),
                "save actual ADC widget for visual review");
        }
    }
    {
        anan::TxSessionTrace trace;
        RadioModel model;
        check(model.rebuildBackendForTest(QStringLiteral("anan")), "production ANAN bindings are available without a radio");
        auto* backend = static_cast<anan::AnanBackend*>(model.backend());
        anan::AnanLinkStatsTestAccess::prepare(*backend,2,false,5,anan::AnanModel::Anan200D,true);
        anan::AnanLinkStatsTestAccess::publish(*backend);
        check(backend->requestTxSlice(0,true) == ReceiveDispatch::Dispatched
              && !backend->capabilities().canTransmit,
              "selecting primary TX before preparation changes no key admission");
        std::unique_ptr<QWidget> page(createAnanTransmitPowerPage(&model,nullptr));
        page->show();
        auto* table = page->findChild<QTableWidget*>(QStringLiteral("ananTxPowerTable"));
        QElapsedTimer deadline; deadline.start();
        while (table->rowCount() != 11 && deadline.elapsed() < 2500) {
            app.processEvents(); QThread::msleep(1);
        }
        check(table->isEnabled() && table->rowCount() == 11, "real PA page loads eleven band references");
        check(table->height() < 500, "PA table ends after its rows rather than filling the page");
        for (const char* removed : {"ananTxPowerBand","ananTxPowerWatts","ananTxPowerPreview","ananTxDspPrepare","ananTxDspCancel","ananTxSessionEnable","ananTxSessionDisable"}) {
            check(!page->findChild<QWidget*>(QString::fromLatin1(removed)), "normal Setup has no preview or manual TX lifecycle controls");
        }
        anan::AnanLinkStatsTestAccess::injectTransport(*backend,trace);
        const auto await = [&](const std::function<bool()>& predicate) {
            QElapsedTimer timer; timer.start();
            while (!predicate() && timer.elapsed() < 5000) { app.processEvents(); QThread::msleep(5); }
            return predicate();
        };
        auto* save = page->findChild<QPushButton*>(QStringLiteral("ananTxPowerApply"));
        auto* antenna = page->findChild<QComboBox*>(QStringLiteral("ananTxSessionAntenna"));
        static_cast<QDoubleSpinBox*>(table->cellWidget(5,1))->setValue(50);
        static_cast<QDoubleSpinBox*>(table->cellWidget(5,2))->setValue(10);
        antenna->setCurrentIndex(1); save->click();
        check(await([&] { return backend->capabilities().canTransmit; }), "saved PA profile automatically prepares and enables TX");
        check(backend->capabilities().maxSlices > 1 && backend->capabilities().maxPanadapters > 1,
              "automatic TX readiness does not hide receive expansion");
        check(trace.keyOn.load() == 0 && model.transmitModel().rfPower() == 0,
              "automatic readiness never keys and starts with zero RF power");
        check(model.requestLocalShowTxInWaterfall(true) && model.transmitModel().showTxInWaterfall(), "local TX waterfall control remains available");
        anan::AnanLinkStatsTestAccess::meters(*backend);
        check(model.meterModel().updateValueByName(QStringLiteral("TX"),QStringLiteral("COMPPEAK"),3.0f)
              && std::abs(model.meterModel().compPeak()-3.0f) < 0.01f,
              "ANAN compression definition routes the shared host ClientComp reading to Phone/CW");
        check(backend->requestTransmitControl({TransmitControlRequest::Field::ProcessorEnabled,ProcessorSetting{true,1}},TunePowerContext::Deferred)
              == ReceiveDispatch::LocalOnly,"PROC follows the existing host voice chain rather than a radio-side command");
        check(backend->capabilities().txMonitorControl.has_value(),"shared ANAN path provides local monitor control");
        model.transmitModel().setSbMonitor(true);
        check(model.transmitModel().sbMonitor(),"MON switch is echoed by the local monitor owner");
        model.transmitModel().setSbMonitor(false);
        check(!model.transmitModel().sbMonitor(),"MON off disables local monitoring");
        RadioSetupDialog hardwareDialog(&model);
        hardwareDialog.show();
        model.transmitModel().setRfPower(25);
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1; }), "MOX remains the explicit TX trigger");
        check(trace.drive.load() == 11 && (trace.alex.load() & 0x07000000u) == (1u<<25), "automatic readiness captures ANT2 and the saved band power limit");
        const int releases = trace.keyReleases.load();
        model.transmitModel().setTxFilterLow(250);
        model.transmitModel().setTxFilterHigh(2850);
        check(await([&] {
            const auto voice = backend->currentOperatingState().extension.value(QStringLiteral("txSetpoints")).toObject()
                .value(QStringLiteral("ananVoiceFilter")).toObject();
            return voice.value(QStringLiteral("lowHz")).toInt() == 250
                && voice.value(QStringLiteral("highHz")).toInt() == 2850;
        }), "Phone/CW controls apply and acknowledge a live ANAN voice filter");
        check(trace.lastKey.load() == 1 && trace.keyReleases.load() == releases
              && backend->capabilities().canTransmit,
              "live filter application preserves keyed TX without a key-off edge");
        hardwareDialog.selectTab(QStringLiteral("ANAN Hardware"));
        QElapsedTimer hardwarePoll; hardwarePoll.start();
        while (hardwarePoll.elapsed() < 1300) { app.processEvents(); QThread::msleep(5); }
        check(backend->capabilities().canTransmit && trace.lastKey.load() == 1,
              "visible ANAN Hardware polling preserves active TX and readiness");
        check(await([&] { return !save->isEnabled(); }), "full PA profile edits are locked while keyed");
        auto* liveGain = static_cast<QDoubleSpinBox*>(table->cellWidget(5,1));
        check(liveGain->isEnabled() && !table->cellWidget(5,2)->isEnabled(),"only active-band gain is editable during TX");
        liveGain->setValue(55);
        check(await([&] { return trace.drive.load() < 11 && trace.lastKey.load() == 1; }),"live gain recalculates drive without stopping TX");
        check(anan::AnanLinkStatsTestAccess::savedGain(*backend) == 55,"live gain is saved for the exact device profile");
        liveGain->setValue(50);
        check(await([&] { return trace.drive.load() == 11 && trace.lastKey.load() == 1; }),"live gain can return to its calibration value");
        QSignalSpy gainReplies(backend,&IRadioBackend::extensionResult);
        QSignalSpy gainErrors(backend,&IRadioBackend::extensionError);
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("txpower.get"),91001,{});
        const auto gainState = gainReplies.last().at(1).toMap();
        QVariantMap gainRequest{{QStringLiteral("identity"),gainState.value(QStringLiteral("identity"))},
            {QStringLiteral("sessionIdentity"),QStringLiteral("stale-session")},
            {QStringLiteral("band"),5},{QStringLiteral("gainDb"),40.0}};
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("txpower.gain"),91002,gainRequest);
        check(gainErrors.count() == 1 && trace.drive.load() == 11,"stale live gain cannot change drive");
        gainRequest[QStringLiteral("sessionIdentity")] = gainState.value(QStringLiteral("session")).toMap().value(QStringLiteral("identity"));
        gainRequest[QStringLiteral("gainDb")] = true;
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("txpower.gain"),91003,gainRequest);
        check(gainErrors.count() == 2 && anan::AnanLinkStatsTestAccess::savedGain(*backend) == 50,
            "boolean gain cannot overwrite the calibrated active profile");
        model.transmitModel().setMox(false);
        check(await([&] { return trace.lastKey.load() == 0 && save->isEnabled(); }), "RX allows PA edits without a manual TX-disable step");
        hardwareDialog.hide();
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1; }),"fault fixture is explicitly keyed");
        const int beforeFault = trace.keyOn.load();
        anan::AnanLinkStatsTestAccess::faultTx(*backend);
        check(await([&] { return trace.lastKey.load() == 0 && !model.isRadioTransmitting()
            && backend->capabilities().canTransmit; }),
              "transport interruption stops and restores readiness without saving PA again");
        check(trace.keyOn.load() == beforeFault && !model.transmitModel().isTransmitting(),
              "readiness recovery never retries a transmission or preserves old MOX intent");
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1; }),"a new operator MOX works after recovered readiness");
        model.transmitModel().setMox(false);
        check(await([&] { return trace.lastKey.load() == 0; }),"recovered MOX releases normally");
        const int beforeTune = trace.keyOn.load();
        int capabilityDrops = 0;
        const auto retuneObserver = QObject::connect(backend,&IRadioBackend::capabilitiesChanged,&app,[&] {
            if (!backend->capabilities().canTransmit) { ++capabilityDrops; }
        });
        backend->setSliceFrequency(0,14'180'000);
        check(backend->capabilities().canTransmit && model.txSlice() != nullptr,
              "idle in-band tuning preserves admission and selected TX slice synchronously");
        check(await([&] { return backend->capabilities().canTransmit; }), "receive retuning automatically refreshes TX readiness");
        check(capabilityDrops == 0,"in-band VFO changes do not restart host audio or blank the TX footer");
        check(trace.keyOn.load() == beforeTune && model.transmitModel().rfPower() == 25, "retuning preserves operator power without automatically transmitting");
        QObject::disconnect(retuneObserver);
        model.transmitModel().startTune();
        check(await([&] { return trace.lastKey.load() == 1; }), "TUNE reaches the internal carrier path");
        check(await([&] { return model.isRadioTransmitting(); }),"TUNE publishes a local TX edge for waterfall and meters");
        check(trace.frequencyWord.load() == anan::phaseWord(14'180'000),
              "next explicit TUNE uses the latest VFO rather than the previously armed frequency");
        model.transmitModel().stopTune();
        check(await([&] { return trace.lastKey.load() == 0; }), "TUNE off releases its carrier");
        check(await([&] { return !model.isRadioTransmitting(); }),"TUNE release restores RX display state");
        backend->setSliceMode(0,QStringLiteral("CWU"));
        check(await([&] { return backend->capabilities().canTransmit; }), "mode change automatically prepares the matching TX DSP");
        model.transmitModel().setCwBreakIn(true); model.transmitModel().setCwDelay(20);
        model.sendCwKey(true);
        check(await([&] { return trace.lastKey.load() == 1; }), "CW keys after automatic preparation");
        check(await([&] { return model.isRadioTransmitting(); }),"CW publishes a local TX edge for waterfall and meters");
        model.sendCwKey(false);
        check(await([&] { return trace.lastKey.load() == 0; }), "CW release respects hang and tail");
        backend->setSliceMode(0,QStringLiteral("DRM"));
        check(!backend->capabilities().canTransmit, "unsupported TX mode remains closed");
        backend->setSliceMode(0,QStringLiteral("USB"));
        check(await([&] { return backend->capabilities().canTransmit; }), "return to a supported mode restores readiness without buttons");
        {
            std::unique_ptr<QWidget> controllers(createAnanControllersPage(&model, nullptr));
            controllers->show(); app.processEvents();
            auto* nativeGroup = controllers->findChild<QGroupBox*>(QStringLiteral("ananFrontPanelGroup"));
            auto* types = controllers->findChild<QComboBox*>(QStringLiteral("ananFrontPanelType"));
            auto* diy = controllers->findChild<QCheckBox*>(QStringLiteral("ananDiyVitalSensors"));
            auto* applySensors = controllers->findChild<QPushButton*>(QStringLiteral("ananVitalSensorsApply"));
            check(nativeGroup && nativeGroup->isVisible() && types && types->count() == 2,
                  "ANAN controller setup offers USB Andromeda and TCP G2-V2");
            auto* serial = controllers->findChild<QComboBox*>(QStringLiteral("ananPanelSerialPort"));
            auto* firmware = controllers->findChild<QLabel*>(QStringLiteral("ananPanelFirmware"));
            auto* standard = controllers->findChild<QPushButton*>(QStringLiteral("ananStandardMapping"));
            const QJsonObject standardBindings = FrontPanelModel::standardAndromedaBindings();
            const auto mappedAction = [&](const QString& key) {
                return standardBindings.value(key).toObject().value(QStringLiteral("action")).toString();
            };
            check(standard && mappedAction(QStringLiteral("p44")) == QStringLiteral("lock")
                  && mappedAction(QStringLiteral("p47")) == QStringLiteral("mox")
                  && mappedAction(QStringLiteral("p48")) == QStringLiteral("tune")
                  && mappedAction(QStringLiteral("e12")) == QStringLiteral("drive"),
                  "standard Andromeda lock MOX TUNE and drive match Thetis physical numbering");
            check(mappedAction(QStringLiteral("p21")) == QStringLiteral("softkey1")
                  && mappedAction(QStringLiteral("p49")).startsWith(QLatin1String("unavailable:"))
                  && mappedAction(QStringLiteral("h47")) == QStringLiteral("none"),
                  "menu softkeys are bound while unsupported PureSignal and long MOX remain distinct");
            const int keysBeforePreset = trace.keyOn.load();
            const QVariantMap savedPanel = model.frontPanel().state();
            if (standard) { standard->click(); }
            check(trace.keyOn.load() == keysBeforePreset
                  && model.frontPanel().state().value(QStringLiteral("bindings")) == savedPanel.value(QStringLiteral("bindings")),
                  "loading standard mapping stages only and never keys or persists");
            check(serial && !serial->isEditable() && serial->findData(QStringLiteral("__custom__")) >= 0,
                  "panel uses shared described-port selector with explicit custom path");
            auto& panel = model.frontPanel();
            QVariantMap legacy = panel.state();
            legacy.insert(QStringLiteral("enabled"), false);
            QJsonObject oldBindings = standardBindings;
            oldBindings.insert(QStringLiteral("p30"), QJsonObject{{"action", "unavailable:Band up"}, {"target", 0}});
            legacy.insert(QStringLiteral("bindings"), oldBindings.toVariantMap());
            check(panel.configure(legacy)
                  && QJsonObject::fromVariantMap(panel.state().value(QStringLiteral("bindings")).toMap())
                      .value(QStringLiteral("p30")).toObject().value(QStringLiteral("action")).toString() == QStringLiteral("bandUp"),
                  "completed placeholder migrates without losing the remaining saved panel configuration");
            FrontPanelTestAccess::feed(panel, "ZZZS0101006;");
            check(panel.state().value(QStringLiteral("hardwareVersion")).toInt() == 1
                  && panel.state().value(QStringLiteral("firmwareVersion")).toInt() == 6
                  && firmware && firmware->text() == QStringLiteral("h/w=1  s/w=6"),
                  "Andromeda identity exposes actual hardware and firmware in setup");
            FrontPanelTestAccess::close(panel);
            check(firmware && firmware->text() == QStringLiteral("Not identified"),
                  "disconnected panel does not display stale firmware");
            check(diy && applySensors && !backend->capabilities().hasPaCurrentTelemetry,
                  "200D override has no native 7000 current sensor claim");
            if (diy && applySensors) {
                diy->setChecked(true); applySensors->click();
                check(await([&] { return backend->capabilities().hasPaCurrentTelemetry; }),
                      "explicit DIY enable activates voltage/current capability without keying");
                diy->setChecked(false); applySensors->click();
                check(await([&] { return !backend->capabilities().hasPaCurrentTelemetry; }),
                      "DIY disable withdraws unsupported current display");
            }
        }
        app.processEvents();
        {
            auto& panel = model.frontPanel();
            FrontPanelTestAccess::prepare(panel, QStringLiteral("mox"));
            const int beforePanel = trace.keyOn.load();
            FrontPanelTestAccess::feed(panel, "ZZTX1;ZZZS;ZZZE000;");
            check(trace.keyOn.load() == beforePanel, "queries and malformed panel frames cannot key TX");
            panel.setLearning(true);
            FrontPanelTestAccess::feed(panel, "ZZZP441;");
            check(trace.keyOn.load() == beforePanel, "panel learn mode suppresses TX input");
            panel.setLearning(false);
            FrontPanelTestAccess::feed(panel, "ZZZP441;");
            check(await([&] { return trace.lastKey.load() == 1; }), "fresh physical panel MOX uses the producer admission gate");
            FrontPanelTestAccess::feed(panel, "ZZZP440;ZZZP441;");
            check(await([&] { return trace.lastKey.load() == 0; }), "panel MOX toggle releases its own captured request");
            FrontPanelTestAccess::feed(panel, "ZZZP440;");
            QSignalSpy desktop(&panel, &FrontPanelModel::desktopActionRequested);
            FrontPanelTestAccess::feed(panel, "ZZZP301;ZZZP300;ZZZP321;ZZZP320;");
            check(desktop.count() == 2 && desktop.at(0).at(0).toString() == QStringLiteral("bandUp")
                  && desktop.at(1).at(0).toString() == QStringLiteral("filterUp"),
                  "physical band and filter buttons reach the desktop operating paths");
            FrontPanelTestAccess::feed(panel, "ZZZP291;ZZZP290;ZZZP301;ZZZP300;");
            check(panel.state().value(QStringLiteral("shift")).toBool()
                  && desktop.last().at(0).toString() == QStringLiteral("band:160m"),
                  "Thetis shifted band grid starts at physical button30, 160m");
            FrontPanelTestAccess::feed(panel, "ZZZP291;ZZZP290;ZZZP111;ZZZP110;ZZZE111;");
            check(panel.state().value(QStringLiteral("selectingMultifunction")).toBool()
                  && panel.state().value(QStringLiteral("multifunction")).toString() == QStringLiteral("agcT"),
                  "multifunction press and rotation select a supported control without applying it");
            FrontPanelTestAccess::feed(panel, "ZZZP111;ZZZP110;");
            check(!panel.state().value(QStringLiteral("selectingMultifunction")).toBool(), "second multifunction press confirms selection");
            FrontPanelTestAccess::feed(panel, "ZZZP211;ZZZP210;ZZZP211;ZZZP210;ZZZP211;ZZZP210;");
            check(panel.state().value(QStringLiteral("menu")).toString() == QStringLiteral("TX"), "softkey21 reaches the Thetis TX page");
            FrontPanelTestAccess::feed(panel, "ZZZP251;ZZZP250;");
            check(await([&] { return trace.lastKey.load() == 1; }), "TX menu MOX uses the same admitted producer path");
            FrontPanelTestAccess::feed(panel, "ZZZP251;ZZZP250;");
            check(await([&] { return trace.lastKey.load() == 0; }), "TX menu MOX releases its captured request");
            anan::AnanLinkStatsTestAccess::forgetViewBand(*backend);
            const auto bandViewBridge = QObject::connect(&panel,&FrontPanelModel::desktopActionRequested,&app,
                [&](const QString& action,int id,int) {
                    if (action != QLatin1String("bandView")) { return; }
                    backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("band.center"),91020,
                        QVariantMap{{QStringLiteral("panId"),QStringLiteral("anan-0")},{QStringLiteral("sliceId"),id},
                            {QStringLiteral("lowHz"),14'000'000.0},{QStringLiteral("highHz"),14'350'000.0}});
                });
            FrontPanelTestAccess::feed(panel, "ZZZP451;ZZZP450;");
            check(await([&] { return panel.state().value(QStringLiteral("ctun")).toBool(); }),
                  "CTUN button uses the existing backend operation and reflects its confirmed state");
            const double ctunCenter = anan::AnanLinkStatsTestAccess::center(*backend);
            backend->setSliceFrequency(0,14'181'000);
            check(await([&] { return anan::AnanLinkStatsTestAccess::ctun(*backend)
                && anan::AnanLinkStatsTestAccess::center(*backend) == ctunCenter; }),
                "first VFO step after panel CTUN on retains CTUN and capture center even without a prior band view");
            FrontPanelTestAccess::feed(panel, "ZZZP451;ZZZP450;");
            check(await([&] { return !panel.state().value(QStringLiteral("ctun")).toBool(); }), "second CTUN press restores normal tuning");
            check(await([&] { return backend->capabilities().canTransmit; }), "CTUN changes do not strand idle TX readiness");
            QObject::disconnect(bandViewBridge);
            FrontPanelTestAccess::prepare(panel, QStringLiteral("pttHold"));
            FrontPanelTestAccess::feed(panel, "ZZZP441;");
            check(await([&] { return trace.lastKey.load() == 1; }), "panel held PTT is admitted explicitly");
            FrontPanelTestAccess::close(panel);
            check(await([&] { return trace.lastKey.load() == 0; }), "panel disconnect cancels its held PTT");
            const int stoppedPanel = trace.keyOn.load();
            FrontPanelTestAccess::feed(panel, "ZZZP441;"); app.processEvents();
            check(trace.keyOn.load() == stoppedPanel, "an unidentified disconnected panel cannot replay an old key");
        }
        check(backend->createSlice(QStringLiteral("anan-0"),14'182'000),"second RX slice opens beside TX source A");
        check(await([&] { return backend->capabilities().canTransmit; }),"TX readiness returns with two receive slices");
        backend->setActiveSlice(1);
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1; }),"MOX works with two RX slices and RX B selected");
        check(trace.frequencyWord.load() == anan::phaseWord(14'181'000),"TX remains on Slice A rather than active RX B");
        model.transmitModel().setMox(false);
        check(await([&] { return trace.lastKey.load() == 0; }),"multi-RX MOX releases normally");
        model.transmitModel().startTune();
        check(await([&] { return trace.lastKey.load() == 1; }),"TUNE also works with multiple receivers");
        model.transmitModel().stopTune();
        check(await([&] { return trace.lastKey.load() == 0; }),"multi-RX TUNE releases normally");
        model.slice(1)->setTxSlice(true);
        check(await([&] { return model.txSlice() == model.slice(1) && backend->capabilities().canTransmit; }),
            "operator can select RX B as the ready transmit source");
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1 && trace.frequencyWord.load() == anan::phaseWord(14'182'000); }),
            "selected TX B keys at its own frequency");
        model.transmitModel().setMox(false);
        check(await([&] { return trace.lastKey.load() == 0; }),"TX B releases normally");
        model.slice(0)->setTxSlice(true);
        check(await([&] { return model.txSlice() == model.slice(0) && backend->capabilities().canTransmit; }),
            "operator can restore TX A without keying");
        QSignalSpy adcReplies(backend,&IRadioBackend::extensionResult);
        QSignalSpy adcErrors(backend,&IRadioBackend::extensionError);
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.get"),92001,{});
        check(!adcReplies.empty(),"ADC routing reports the connected hardware");
        const auto adcState = adcReplies.last().at(1).toMap();
        check(adcState.value(QStringLiteral("firstDdc")).toInt() == 2,"P2 UI uses physical DDC2 for primary ANAN-200D RX");
        auto assignments = adcState.value(QStringLiteral("assignments")).toList();
        assignments[0] = 1;
        QVariantMap adcRequest{{QStringLiteral("identity"),adcState.value(QStringLiteral("identity"))},
            {QStringLiteral("assignments"),assignments}};
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.set"),92002,adcRequest);
        check(adcReplies.last().at(0).toULongLong() == 92002
            && adcReplies.last().at(1).toMap().value(QStringLiteral("assignments")).toList()[0].toInt() == 1,
            "ADC assignment changes live with multiple receivers");
        check(await([&] { return trace.primaryAdc.load() == 1; }),"P2 live ADC1 routing reaches physical DDC2 packet row");
        std::unique_ptr<QWidget> adcPage(createAnanAdcPage(&model,nullptr)); adcPage->show();
        auto* primaryAdc = adcPage->findChild<QRadioButton*>(QStringLiteral("ananDdc2Adc1"));
        check(await([&] { return primaryAdc && primaryAdc->isChecked(); }),"ADC page displays the active physical DDC assignment");
        model.transmitModel().setMox(true);
        check(await([&] { return trace.lastKey.load() == 1; }),"ADC routing does not strand multi-RX TX readiness");
        const int errorsBeforeAdc = adcErrors.count();
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.set"),92003,adcRequest);
        check(adcErrors.count() == errorsBeforeAdc+1 && trace.lastKey.load() == 1,"routing changes during TX are refused without interrupting TX");
        model.transmitModel().setMox(false);
        check(await([&] { return trace.lastKey.load() == 0; }),"ADC fixture releases MOX");
        assignments[0] = 2; adcRequest[QStringLiteral("assignments")] = assignments;
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.set"),92004,adcRequest);
        check(adcErrors.count() == errorsBeforeAdc+2,"nonexistent ADC2 cannot be assigned");
        assignments[0] = 0; adcRequest[QStringLiteral("assignments")] = assignments;
        backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("adc.set"),92005,adcRequest);
        check(adcReplies.last().at(0).toULongLong() == 92005,"ADC0 can be restored without reconnecting");
        const QString beforeBandAntenna = model.slice(0)->rxAntenna();
        const auto bandView = [&] {
            backend->invokeExtension(QStringLiteral("anan"),QStringLiteral("band.center"),92006,
                QVariantMap{{QStringLiteral("panId"),QStringLiteral("0xe1000000")},{QStringLiteral("sliceId"),0},
                    {QStringLiteral("lowHz"),14'000'000.0},{QStringLiteral("highHz"),14'350'000.0}});
        };
        bandView();
        check(await([&] { return backend->capabilities().canTransmit; }),"B band view prepares with multiple slices");
        bandView();
        check(await([&] { return backend->capabilities().canTransmit; }),"B restores the previous view with multiple slices");
        check(model.slice(0)->rxAntenna() == beforeBandAntenna && trace.primaryAdc.load() == 0,
            "B on/off preserves the commanded RX antenna and physical ADC selection");
        check(backend->removeSlice(1),"extra receiver closes normally");
        check(await([&] { return save->isEnabled(); }), "idle profile can be edited");
        static_cast<QDoubleSpinBox*>(table->cellWidget(5,2))->setValue(0); save->click();
        check(await([&] { return !backend->capabilities().canTransmit; }), "zero band power limit still prevents TX");
    }
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("ANAN Hardware"));
        auto* implementation = dialog.findChild<QComboBox*>(QStringLiteral("ananImplementation"));
        check(implementation && implementation->isEnabled() && implementation->count() == 2,
              "setup exposes both independent Saturn choices while disconnected");
        implementation->setCurrentIndex(1);
        auto* rate = dialog.findChild<QComboBox*>(QStringLiteral("saturnOriginalRate"));
        check(rate && !rate->isHidden(), "original configuration page exists");
        const int extendedRate = anan::AnanSettings::ddc0RateKsps();
        rate->setCurrentIndex(rate->findData(384));
        check(anan::AnanImplementation::originalOptions().value(QStringLiteral("ddc0RateKsps")).toInt() == 384
              && anan::AnanSettings::ddc0RateKsps() == extendedRate,
              "original Setup writes only original configuration");
        check(anan::AnanImplementation::preferred({}) == anan::AnanImplementation::originalId(),
              "Setup implementation choice is persisted for the next connection");
        implementation->setCurrentIndex(0);
    }
    {
        RadioModel model;
        auto owned = std::make_unique<anan::AnanBackend>();
        auto* backend = owned.get();
        anan::AnanLinkStatsTestAccess::prepare(*backend, 2, false, 10);
        model.setBackendForTest(std::move(owned), QStringLiteral("anan"));
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("ANAN Hardware"));
        dialog.show();
        auto* offset = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ananAutoAgcOffset"));
        QElapsedTimer wait;
        wait.start();
        while (offset && !offset->isEnabled() && wait.elapsed() < 2000) {
            app.processEvents();
            QThread::msleep(10);
        }
        check(offset && offset->isEnabled() && offset->value() == 20.0,
              "Auto AGC-T setup reads the actual session offset before enabling edits");
        auto* micBias = dialog.findChild<QComboBox*>(QStringLiteral("ananMicBias"));
        auto* micInput = dialog.findChild<QComboBox*>(QStringLiteral("ananMicInput"));
        auto* micApply = dialog.findChild<QPushButton*>(QStringLiteral("ananMicApply"));
        wait.restart();
        while (micBias && !micBias->isEnabled() && wait.elapsed() < 2000) {
            app.processEvents(); QThread::msleep(10);
        }
        check(micBias && micBias->isEnabled() && micInput && micInput->isEnabled() && micApply,
              "Saturn P2 setup exposes live microphone wiring and XLR choices");
        micBias->setCurrentIndex(1);
        micInput->setCurrentIndex(1);
        micApply->click();
        QSignalSpy micReplies(backend, &IRadioBackend::extensionResult);
        backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("mic.get"), 9986, {});
        const QVariantMap micOptions = micReplies.isEmpty() ? QVariantMap{}
            : micReplies.last().at(1).toMap().value(QStringLiteral("options")).toMap();
        check(micOptions.value(QStringLiteral("bias")).toBool() && micOptions.value(QStringLiteral("xlr")).toBool(),
              "Apply microphone options reaches the validated backend and saves the selected wiring");
        offset->setValue(30.0);
        QSignalSpy replies(backend, &IRadioBackend::extensionResult);
        backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("autoagc.get"), 9987, {});
        const QVariantList sliceOptions = replies.isEmpty() ? QVariantList{}
            : replies.last().at(1).toMap().value(QStringLiteral("slices")).toList();
        check(!sliceOptions.isEmpty() && sliceOptions.first().toMap().value(QStringLiteral("offsetDb")).toDouble() == 30.0,
              "Auto AGC-T setup writes the selected slice's validated offset");
    }
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
        // Independent manual pins combine without changing the HF/SWL master.
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
        // Both protocols expose the same fenced RX calibration path and persisted factor.
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
    for (const auto [protocol, selectedModel, board, available] : {
             std::tuple{1, anan::AnanModel::Anan7000, 5, false},
             std::tuple{2, anan::AnanModel::AnanG2, 10, true},
             std::tuple{2, anan::AnanModel::Anan7000, 5, true},
             std::tuple{2, anan::AnanModel::Anan8000, 5, true},
             std::tuple{2, anan::AnanModel::Anan100D, 3, false},
             std::tuple{2, anan::AnanModel::Anan200D, 4, false},
             std::tuple{2, anan::AnanModel::OrionMkII, 5, false},
             std::tuple{2, anan::AnanModel::AnanG2E, 20, false}}) {
        anan::AnanSettings::setSpeakerAmpMuted(false);
        RadioModel model;
        auto owned = std::make_unique<anan::AnanBackend>();
        auto* backend = owned.get();
        anan::AnanLinkStatsTestAccess::prepare(*backend, protocol, false, board, selectedModel);
        model.setBackendForTest(std::move(owned), QStringLiteral("anan"));
        RadioSetupDialog dialog(&model);
        dialog.selectTab(QStringLiteral("ANAN Hardware"));
        auto* mute = dialog.findChild<QCheckBox*>(QStringLiteral("ananSpeakerAmpMute"));
        check(mute && mute->isEnabled() == available,
              "amplifier mute stays visible and follows the P2 model capability");
        if (available) {
            mute->click();
            app.processEvents();
            check(anan::AnanSettings::speakerAmpMuted() && mute->isChecked(),
                  "G2 amplifier mute reaches backend and persists without a second filter owner");
            mute->click();
            app.processEvents();
            check(!anan::AnanSettings::speakerAmpMuted(), "G2 amplifier unmutes through the same control");
            QSignalSpy errors(backend, &IRadioBackend::extensionError);
            QSignalSpy replies(backend, &IRadioBackend::extensionResult);
            backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("hw.get"), 9872, {});
            check(!replies.isEmpty(), "G2 publishes the current hardware identity");
            QVariantMap options = replies.last().at(1).toMap();
            options.insert(QStringLiteral("speakerAmpMuted"), QStringLiteral("true"));
            backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 9873, options);
            options.insert(QStringLiteral("speakerAmpMuted"), true);
            options.insert(QStringLiteral("identity"), QStringLiteral("stale-device"));
            backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 9874, options);
            check(errors.count() == 2 && !anan::AnanSettings::speakerAmpMuted(),
                  "ill-typed and stale-device amplifier requests cannot change saved mute");
        }
    }

}
