#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan_original/AnanBackend.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <limits>
#include <future>
#include <thread>
#include <random>

using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace AetherSDR::anan {
class AnanAutoAgcTestAccess {
public:
    static void prepare(AnanBackend& b, int protocol)
    {
        b.m_connected = true;
        b.m_pendingParams.protocol = protocol;
        b.m_receiveLayout.reset(2, 14'100'000, 48);
        const int id = *b.m_receiveLayout.add(7'100'000, -1, true);
        auto r = std::make_shared<AnanBackend::AdditionalRx>();
        r->id = id;
        r->ready = true;
        b.m_additionalRx[id] = r;
    }
    static quint64 revision(const AnanBackend& b, int id)
    { return id == 0 ? b.m_primaryAutoAgcRevision : b.m_additionalRx.at(id)->autoAgcRevision; }
    static int threshold(const AnanBackend& b, int id)
    { return id == 0 ? b.m_agcThreshold : b.m_additionalRx.at(id)->threshold; }
    static void applied(AnanBackend& b, int id, quint64 revision, double gain)
    { b.onAutoAgcApplied(id, revision, gain); }
    static void disconnected(AnanBackend& b) { b.m_connected = false; }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("anan-auto-agc"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) { ++failures; }
    };
    for (const int points : {512, 1024, 4096, 8192}) {
        const float floor = static_cast<float>(-100.0 - 10.0 * std::log10(double(points) / 4096.0));
        std::vector<float> bins(points, floor);
        const auto normalized = AnanAutoAgc::referenceFloor(bins);
        check(normalized && std::abs(*normalized + 100.0) < 1e-4, "display width cannot move the referenced noise floor");
        AnanAutoAgc tracker;
        std::optional<double> threshold;
        for (int n = 0; n < 5; ++n) { threshold = tracker.feed(bins, 500, 20); }
        check(threshold && std::abs(*threshold + 80.0) < 1e-4, "settled reference plus offset reaches the requested threshold");
        check(!tracker.feed(bins, 100, 20), "updates are bounded to 500 ms");
        bins[points / 4] = std::numeric_limits<float>::quiet_NaN();
        check(!tracker.feed(bins, 500, 20), "invalid input cannot drive the gain");
        bins[points / 4] = floor;
        check(!tracker.feed(bins, 500, 20), "invalid data requires a fresh settling period");
        check(!tracker.feed(bins, 2000, 20), "stale data is discarded");
    }
    for (const int protocol : {1, 2}) {
        AnanBackend b;
        AnanAutoAgcTestAccess::prepare(b, protocol);
        check(b.capabilities().autoAgcThresholdControl.has_value(), "P1 and P2 declare an implemented automatic threshold verb");
        check(b.requestSliceAgcThresholdMode(0, AgcThresholdMode::Automatic) == ReceiveDispatch::Dispatched,
              "primary receiver accepts auto mode");
        check(b.requestSliceAgcThresholdMode(1, AgcThresholdMode::Automatic) == ReceiveDispatch::Dispatched,
              "non-active second-stream receiver accepts its own auto mode");
        const quint64 first = AnanAutoAgcTestAccess::revision(b, 0);
        const int otherBefore = AnanAutoAgcTestAccess::threshold(b, 1);
        AnanAutoAgcTestAccess::applied(b, 0, first, 50.0);
        check(AnanAutoAgcTestAccess::threshold(b, 0) == 50
            && AnanAutoAgcTestAccess::threshold(b, 1) == otherBefore, "one slice's noise cannot overwrite the other slice");
        b.requestSliceAgcThresholdMode(0, AgcThresholdMode::Manual);
        AnanAutoAgcTestAccess::applied(b, 0, first, 120.0);
        check(AnanAutoAgcTestAccess::threshold(b, 0) == 50, "late automatic readback cannot undo manual mode");
        b.requestSliceAgcThresholdMode(0, AgcThresholdMode::Automatic);
        AnanAutoAgcTestAccess::applied(b, 0, first, 120.0);
        check(AnanAutoAgcTestAccess::threshold(b, 0) == 50, "old auto activation cannot overwrite a newer activation");
        QSignalSpy errors(&b, &IRadioBackend::extensionError);
        b.invokeExtension(QStringLiteral("anan"), QStringLiteral("autoagc.set"), 9,
            QVariantMap{{QStringLiteral("slice"), 0}, {QStringLiteral("offsetDb"), 10.0}, {QStringLiteral("identity"), QStringLiteral("stale")}});
        check(errors.size() == 1, "stale setup writes are refused");
        AnanAutoAgcTestAccess::disconnected(b);
        check(b.requestSliceAgcThresholdMode(0, AgcThresholdMode::Automatic) == ReceiveDispatch::Unsupported,
              "disconnected receivers cannot start automatic gain changes");
    }
    anan_original::AnanBackend original;
    check(!original.capabilities().autoAgcThresholdControl, "original tropo1234 implementation is unchanged");
    {
        AnanPanAnalyzer::Settings config;
        config.averageTimeMs = 10000;
        std::string error;
        auto analyzer = AnanPanAnalyzer::create(60, config, &error);
        check(bool(analyzer), "shared FFT exposes a second measurement output");
        if (analyzer) {
            std::mt19937 random(123);
            std::normal_distribution<float> gaussian(0.0f, 0.001f);
            std::vector<std::complex<float>> iq(40960);
            for (auto& sample : iq) { sample = {gaussian(random), gaussian(random)}; }
            analyzer->feed(iq);
            std::vector<float> noise, display;
            QElapsedTimer wait;
            wait.start();
            while (!analyzer->takeNoiseFrame(noise) && wait.elapsed() < 5000) { QThread::msleep(2); }
            check(!noise.empty(), "noise data is available even if no display frame is consumed");
            check(analyzer->takeFrame(display), "noise polling does not consume the display output");
            for (auto& sample : iq) { sample *= 10.0f; }
            analyzer->feed(iq);
            wait.restart();
            bool fresh = false;
            while (!(fresh = analyzer->takeNoiseFrame(noise)) && wait.elapsed() < 5000) { QThread::msleep(2); }
            analyzer->takeFrame(display);
            const auto rawFloor = AnanAutoAgc::referenceFloor(noise), visibleFloor = AnanAutoAgc::referenceFloor(display);
            check(fresh && rawFloor && visibleFloor && *rawFloor > *visibleFloor + 5.0,
                  "long display averaging cannot delay the noise measurement");
        }
    }
    std::optional<double> gainAt48;
    for (const int rate : {48000, 96000}) {
        WdspChannel::Config config;
        config.inputSampleRate = rate;
        std::string error;
        auto channel = WdspChannel::create(config, &error);
        check(bool(channel), "WDSP channel constructs for the threshold scale check");
        if (!channel) { continue; }
        const double threshold = -100.0 + 10.0 * std::log10(double(rate) / 48000.0);
        const auto gain = channel->setAutoAgcThreshold(threshold, 4096);
        check(gain && *gain >= -20.0 && *gain <= 120.0, "WDSP applies and reads a bounded actual gain");
        if (rate == 48000) { gainAt48 = gain; }
        else { check(gainAt48 && gain && std::abs(*gainAt48 - *gain) < 1e-6, "same noise density yields the same gain across sample rates"); }
        {
            auto planner = WdspChannel::fftwSetupLock();
            std::promise<bool> response;
            auto future = response.get_future();
            std::thread control([&] { response.set_value(!channel->setAutoAgcThreshold(threshold, 4096)); });
            const bool returned = future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
            planner.unlock();
            control.join();
            check(returned && future.get(), "auto control defers while FFTW planning is busy");
        }
        check(!channel->setAutoAgcThreshold(std::numeric_limits<double>::quiet_NaN(), 4096), "invalid thresholds cannot enter WDSP");
    }
    {
        AnanRxDsp dsp;
        AnanRxDsp::Config config;
        std::string error;
        const bool configured = dsp.configure(config, &error);
        check(configured, "production receive DSP configures for automatic tracking");
        if (configured) {
            QSignalSpy applied(&dsp, &AnanRxDsp::autoAgcApplied);
            dsp.setAutoAgc(true, 20.0, 77);
            std::mt19937 random(456);
            std::normal_distribution<float> gaussian(0.0f, 0.001f);
            std::vector<std::complex<float>> iq(1024);
            QElapsedTimer runtime;
            runtime.start();
            while (applied.isEmpty() && runtime.elapsed() < 4500) {
                for (auto& sample : iq) { sample = {gaussian(random), gaussian(random)}; }
                dsp.processIqBlock(iq);
                QThread::msleep(20);
            }
            check(!applied.isEmpty() && applied.last().at(0).toULongLong() == 77,
                  "real IQ to FFT to WDSP flow applies and reports an automatic gain");
            const int before = applied.size();
            dsp.setAutoAgc(false, 20.0, 78);
            for (int n = 0; n < 30; ++n) { dsp.processIqBlock(iq); QThread::msleep(20); }
            check(applied.size() == before, "manual mode stops gain changes in the production DSP lane");
        }
    }
    return failures == 0 ? 0 : 1;
}
