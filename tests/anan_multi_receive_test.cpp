#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanBackend.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>
using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace AetherSDR::anan {
struct AnanMultiRxTestAccess {
    static void prepare(AnanBackend& b)
    {
        b.m_streamApplyOverride = [](const auto&, const auto&) { return true; };
        b.m_rateApplyOverride = [](int, int) { return true; };
        b.m_connected = true;
        b.m_discoveryInfoReceived = true;
        b.m_discoveredBoardId = 4;
        b.m_discoveredReceiverCount = 7;
        b.m_pendingParams.expectedBoardId = 4;
        b.m_pendingParams.model = AnanModel::Anan200D;
        b.m_sliceFreqHz = b.m_ddcCenterHz = 14'175'000;
        b.m_pendingParams.ddc0RateKsps = 48;
        b.m_pendingDspConfig.inputSampleRateHz = 48000;
        b.m_pendingDspConfig.blockForOutput = true;
        b.m_receiveLayout.reset(5, b.m_sliceFreqHz, 48);
        emit b.connected();
    }
    static void pcm(AnanBackend& b, int id, int frames, float value)
    {
        PcmProducer producer;
        checkProducer(producer.start(PcmPurpose::Speaker, -1, {24000, PcmLayout::Stereo}));
        auto frame = producer.produce(QVector<float>(frames * 2, value));
        checkProducer(frame.has_value());
        QMetaObject::invokeMethod(b.m_client, [&b, id, frame] {
            b.submitReceiveAudio(id, *frame);
        }, Qt::BlockingQueuedConnection);
    }
    static void resetAudio(AnanBackend& b)
    {
        b.resetReceiveAudioBuffers();
        barrier(b);
    }
    static void checkProducer(bool ok) { if (!ok) { std::abort(); } }
    static void p1(AnanBackend& b) { b.m_pendingParams.protocol = 1; }
    static void profile(AnanBackend& b, int board, int protocol)
    {
        b.m_discoveredBoardId = board;
        b.m_pendingParams.expectedBoardId = board;
        b.m_pendingParams.protocol = protocol;
        b.m_pendingParams.model = AnanModel::Automatic;
    }
    static bool busy(const AnanBackend& b) { return b.m_receiveBuilding; }
    static int stream(const AnanBackend& b, int id)
    {
        const auto* s = b.m_receiveLayout.slice(id);
        return s ? s->stream : -1;
    }
    static double center(const AnanBackend& b, int st)
    {
        return b.m_receiveLayout.streams()[st].centerHz;
    }
    static int rate(const AnanBackend& b, int st)
    {
        return b.m_receiveLayout.streams()[st].rateKsps;
    }
    static bool empty(const AnanBackend& b) { return b.m_additionalRx.empty(); }
    // HERMES §16: physical RF offset is conjugated on the HPSDR wire.
    static void feed(AnanBackend& b, int st, double tone = 1000, int rate = 48000)
    {
        QMetaObject::invokeMethod(
            b.m_client,
            [c = b.m_client, st, tone, rate] {
                std::vector<std::complex<float>> iq(1024);
                for (int block = 0; block < 64; ++block) {
                    for (int i = 0; i < 1024; ++i)
                        iq[i] = std::polar(0.01f, float(-2 * 3.141592653589793 * tone *
                                                        (block * 1024 + i) / rate));
                    emit c->ddcIqReady(st, iq);
                }
            },
            Qt::QueuedConnection);
    }
    static void barrier(AnanBackend& b)
    {
        QMetaObject::invokeMethod(b.m_client, [] {}, Qt::BlockingQueuedConnection);
    }
};
} // namespace AetherSDR::anan
void check(bool ok, const char* why)
{
    if (!ok) {
        std::cerr << "FAIL: " << why << '\n';
        std::exit(1);
    }
}
template <class F> void wait(F done)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < 600000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(done(), "bounded DSP completion");
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-multi-rx"));
    QCoreApplication app(argc, argv);
    AnanBackend b;
    AnanMultiRxTestAccess::prepare(b);
    struct Limits {
        int board, slices, p2;
    };
    for (const auto c : {Limits{1, 4, 4}, Limits{2, 2, 2}, Limits{3, 5, 5}, Limits{4, 5, 5},
                         Limits{5, 5, 5}, Limits{10, 5, 5}, Limits{20, 5, 4}}) {
        AnanMultiRxTestAccess::profile(b, c.board, 2);
        check(b.capabilities().maxSlices == c.slices && b.capabilities().maxPanadapters == c.p2,
              "native P2 model ceilings match implemented resources");
        if (c.board != 10) {
            AnanMultiRxTestAccess::profile(b, c.board, 1);
            check(b.capabilities().maxPanadapters == std::min(4, c.p2),
                  "native P1 model windows obey the four-user-stream ceiling");
        }
    }
    AnanMultiRxTestAccess::profile(b, 4, 2);
    std::map<int, SliceDelta> state;
    std::map<int, int> audio;
    std::map<int, std::vector<float>> samples;
    std::vector<QString> removed;
    QObject::connect(&b, &IRadioBackend::sliceChanged,
                     [&](int id, const SliceDelta& d) { state[id] = d; });
    QObject::connect(&b, &IRadioBackend::sliceAudioFrameReady, [&](int id, const PcmFrame& pcm) {
        audio[id] += pcm.legacyStereo24().size();
        const auto bytes = pcm.legacyStereo24();
        const auto* p = reinterpret_cast<const float*>(bytes.constData());
        samples[id].insert(samples[id].end(), p, p + bytes.size() / sizeof(float));
    });
    QObject::connect(&b, &IRadioBackend::panRemoved,
                     [&](const QString& p) { removed.push_back(p); });
    check(b.capabilities().maxSlices == 5 && b.capabilities().maxPanadapters == 5,
          "Orion limits expose software slices and hardware streams");
    check(b.createSlice(QStringLiteral("anan-0"), 14'178'000), "create shared slice");
    wait([&] { return !AnanMultiRxTestAccess::busy(b); });
    check(AnanMultiRxTestAccess::stream(b, 1) == 0 && state.contains(1),
          "shared slice has its own observed identity");
    check(state[1].letter == QStringLiteral("B"), "second receiver is labelled B");
    std::vector<float> mixedSamples;
    const auto mixConnection = QObject::connect(&b, &IRadioBackend::audioFrameReady,
        [&](const PcmFrame& pcm) {
            const auto bytes = pcm.legacyStereo24();
            const auto* p = reinterpret_cast<const float*>(bytes.constData());
            mixedSamples.insert(mixedSamples.end(), p, p + bytes.size() / sizeof(float));
        });
    AnanMultiRxTestAccess::pcm(b, 0, 1, 0);
    AnanMultiRxTestAccess::pcm(b, 1, 1, 0);
    AnanMultiRxTestAccess::pcm(b, 0, 1, 0);
    QCoreApplication::processEvents();
    mixedSamples.clear();
    AnanMultiRxTestAccess::pcm(b, 0, 6, 0.1f);
    QCoreApplication::processEvents();
    check(mixedSamples.empty(), "backend waits for a late slice instead of padding silence");
    AnanMultiRxTestAccess::pcm(b, 1, 2, 0.2f);
    AnanMultiRxTestAccess::pcm(b, 1, 4, 0.2f);
    QCoreApplication::processEvents();
    check(mixedSamples.size() == 12, "backend emits every matched frame despite a delayed GUI drain");
    for (float sample : mixedSamples) {
        check(std::abs(sample - 0.3f) < 1e-6, "backend mixed output contains both slices continuously");
    }
    QElapsedTimer noInput;
    noInput.start();
    while (noInput.elapsed() < 30) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(mixedSamples.size() == 12, "idle GUI callbacks cannot invent mixed audio frames");
    QObject::disconnect(mixConnection);
    AnanMultiRxTestAccess::resetAudio(b);

    b.setSliceFrequency(1, 14'179'000);
    check(state[1].frequency == 14.179, "second receiver accepts its own tuning intent");
    b.setPanCenter(QStringLiteral("anan-0"), 14'180'000,
                   IRadioBackend::PanCenterIntent::Range);
    check(state[1].frequency == 14.179 &&
              AnanMultiRxTestAccess::center(b, 0) == 14'180'000,
          "shared pan centering preserves the additional VFO");
    b.setSliceFrequency(0, 14'176'000);
    AnanMultiRxTestAccess::barrier(b);
    QElapsedTimer tuneWait;
    tuneWait.start();
    while (tuneWait.elapsed() < 100) { QCoreApplication::processEvents(); QThread::msleep(1); }
    check(AnanMultiRxTestAccess::center(b, 0) == 14'180'000 &&
              state[1].frequency == 14.179,
          "trailing primary tuning preserves the shared capture and peer VFO");
    b.setSliceFrequency(0, 14'175'000);
    b.setSliceFrequency(1, 14'178'000);
    b.setPanCenter(QStringLiteral("anan-0"), 14'175'000,
                   IRadioBackend::PanCenterIntent::Range);
    AnanMultiRxTestAccess::feed(b, 0, 4000);
    AnanMultiRxTestAccess::barrier(b);
    QCoreApplication::processEvents();
    check(audio[1] > 0, "actual additional DSP produces per-slice audio");
    auto tonePower = [&](int id, double hz) {
        const auto& v = samples[id];
        const int frames = int(v.size() / 2), n = std::min(frames, 6000);
        std::complex<double> sum{};
        for (int i = 0; i < n; ++i)
            sum += double(v[2 * (frames - n + i)]) *
                   std::polar(1.0, -2 * 3.141592653589793 * hz * i / 24000);
        return n ? std::norm(sum) / double(n * n) : 0.0;
    };
    check(tonePower(1, 1000) > 1e-9 && tonePower(1, 1000) > 20 * tonePower(1, 1100),
          "shared 3 kHz slice shift demodulates a 4 kHz IQ tone at 1 kHz");
    SliceControlRequest select;
    select.field = SliceControlRequest::Field::ActiveSlice;
    select.value = true;
    check(b.requestSliceControl(1, select) == ReceiveDispatch::Dispatched &&
              state[1].active.value_or(false) && !state[0].active.value_or(true),
          "active receive selection is observed consistently across slices");
    b.setSliceMode(1, QStringLiteral("LSB"));
    b.setSliceFilter(1, -2700, -200);
    b.setSliceAudioGain(1, 35);
    check(state[1].mode == QStringLiteral("LSB") && state[1].filterLow == -2700 &&
              state[1].audioGain == 35,
          "extra slice controls are independent");
    b.setActiveSlice(0);
    b.invokeExtension(QStringLiteral("anan"), QStringLiteral("band.select"), 1,
                      QVariantMap{{QStringLiteral("band"), QStringLiteral("40m")},
                                  {QStringLiteral("sliceId"), 1},
                                  {QStringLiteral("frequencyHz"), 7'100'000.0},
                                  {QStringLiteral("mode"), QStringLiteral("LSB")}});
    check(AnanMultiRxTestAccess::stream(b, 1) == 1 &&
              AnanMultiRxTestAccess::center(b, 0) == 14'175'000,
          "leaving shared capture preserves the other window");
    check(state[0].frequency == 14.175 && state[0].mode == QStringLiteral("USB") &&
              state[0].active.value_or(false) && state[1].frequency == 7.1,
          "band selection addresses B while A remains globally active and unchanged");
    int refusedTarget = 0;
    QObject::connect(&b, &IRadioBackend::extensionError,
                     [&](quint64 id, const QString&) { if (id == 99) { ++refusedTarget; } });
    b.invokeExtension(QStringLiteral("anan"), QStringLiteral("band.select"), 99,
                      QVariantMap{{QStringLiteral("sliceId"), 999},
                                  {QStringLiteral("band"), QStringLiteral("80m")},
                                  {QStringLiteral("frequencyHz"), 3'700'000.0},
                                  {QStringLiteral("mode"), QStringLiteral("LSB")}});
    check(refusedTarget == 1 && state[0].frequency == 14.175 && state[1].frequency == 7.1,
          "missing explicit band target cannot fall back to the active receiver");
    b.setActiveSlice(1);
    check(b.createPanadapter(), "create independent pan");
    wait([&] { return !AnanMultiRxTestAccess::busy(b); });
    check(AnanMultiRxTestAccess::stream(b, 2) == 2, "explicit pan always owns a DDC");
    AnanMultiRxTestAccess::feed(b, 2);
    AnanMultiRxTestAccess::barrier(b);
    QCoreApplication::processEvents();
    b.setPanBandwidth(QStringLiteral("anan-1"), 96000);
    wait([&] { return !AnanMultiRxTestAccess::busy(b); });
    check(AnanMultiRxTestAccess::rate(b, 1) == 96 && AnanMultiRxTestAccess::rate(b, 0) == 48,
          "P2 rates change per capture");
    int spanReports = 0;
    double reportedSpanMhz = 0;
    QObject::connect(&b, &IRadioBackend::panCenterBandwidthChanged,
                     [&](const QString& pan, double, double spanMhz) {
                         if (pan == QStringLiteral("anan-1")) {
                             ++spanReports;
                             reportedSpanMhz = spanMhz;
                         }
                     });
    b.setPanBandwidth(QStringLiteral("anan-1"), 100000);
    check(spanReports > 0 && std::abs(reportedSpanMhz - 0.096) < 1e-9 &&
              !AnanMultiRxTestAccess::busy(b),
          "unchanged snapped rate confirms actual span without a DSP rebuild");
    samples[1].clear();
    AnanMultiRxTestAccess::feed(b, 1, -2000, 96000);
    AnanMultiRxTestAccess::barrier(b);
    QCoreApplication::processEvents();
    check(tonePower(1, 2000) > 1e-9 && tonePower(1, 2000) > 20 * tonePower(1, 2200),
          "independent LSB receiver adopts 96 ksps and demodulates at 2 kHz");
    AnanMultiRxTestAccess::p1(b);
    b.setPanBandwidth(QStringLiteral("anan-2"), 192000);
    wait([&] { return !AnanMultiRxTestAccess::busy(b); });
    check(AnanMultiRxTestAccess::rate(b, 0) == 192 && AnanMultiRxTestAccess::rate(b, 1) == 192 &&
              AnanMultiRxTestAccess::rate(b, 2) == 192,
          "P1 rate change adopts one global rate on every active capture");
    samples[1].clear();
    AnanMultiRxTestAccess::feed(b, 1, -2000, 192000);
    AnanMultiRxTestAccess::barrier(b);
    QCoreApplication::processEvents();
    check(tonePower(1, 2000) > 1e-9 && tonePower(1, 2000) > 20 * tonePower(1, 2200),
          "P1 global rebuild preserves the extra slice's LSB tuning and audio frequency");
    check(b.removeSlice(1) && !removed.empty(), "last slice retires its independent window");
    const int oldAudio = audio[1];
    AnanMultiRxTestAccess::feed(b, 2);
    AnanMultiRxTestAccess::barrier(b);
    QCoreApplication::processEvents();
    check(audio[2] > 0 && audio[1] == oldAudio, "retired DSP cannot publish queued audio");
    check(b.removeSlice(0) && !b.removeSlice(2),
          "original slice may close; final slice remains protected");
    check(b.createSlice(QStringLiteral("anan-2"), 7'101'000), "accept another asynchronous build");
    b.disconnectRadio();
    QCoreApplication::processEvents();
    check(AnanMultiRxTestAccess::empty(b), "disconnect retires pending and live receiver objects");
    std::cout << "ANAN multi-receive DSP/lifecycle/rate checks passed\n";
}
