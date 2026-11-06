#include "core/backends/anan/P1Client.h"
#include "core/backends/anan/AnanP1TxControl.h"
#include "core/backends/anan/AnanTxDsp.h"
#include "core/dsp/PureSignalCorrection.h"
#include "core/dsp/PureSignalAttenuator.h"
#include "core/dsp/PureSignalSpectrum.h"
#include "core/backends/anan/AnanFeedbackActivity.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <algorithm>
#include <complex>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <vector>
#include <numbers>

namespace AetherSDR::anan {
struct P1ClientTestAccess {
    static void seed(P1Client& client, std::vector<p1::Packet>& writes)
    {
        client.m_running = client.m_validated = client.m_linkUp = true;
        client.m_config.numAdcs = 2; client.m_config.receivers = 5; client.m_config.rateKsps = 192;
        client.m_receiveStreams = {{192,0},{192,1}};
        client.m_profile = P1Client::Profile{2,5,false,ReceiveFilterKind::LegacyHpf};
        client.m_sendOverride = [&writes](std::span<const std::uint8_t> bytes) {
            if (bytes.size() == p1::kPacketBytes) {
                p1::Packet packet{}; std::copy(bytes.begin(),bytes.end(),packet.begin()); writes.push_back(packet);
            }
            return true;
        };
    }
    static void rate(P1Client& client, int rate) { client.m_config.rateKsps = rate; }
};
struct P2ClientTestAccess {
    static void seed(P2Client& client, std::vector<std::pair<quint16,QByteArray>>& writes)
    {
        client.m_running = client.m_discoveryValidated = client.m_linkUp = true;
        client.m_numAdcs = 2; client.m_firstDdcIndex = 2; client.m_availableDdcs = 7;
        client.m_activeDdcs = {{48,0},{48,1}}; client.m_activeDdcCount = 2;
        client.m_transmitProfile = receiveProfile(4);
        client.m_transmitControl = P2TxControl{}; client.m_transmitControl->frequencyHz = 14e6;
        client.m_controlSendOverride = [&writes](std::span<const std::uint8_t> bytes, quint16 port) {
            writes.emplace_back(port,QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
            return qint64(bytes.size());
        };
    }
    static void limit(P2Client& client, int count) { client.m_availableDdcs = count; }
};
}

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("%s %s\n",condition?"PASS":"FAIL",message);
    if (!condition) { ++failures; }
}

void pairedSpectra()
{
    using namespace AetherSDR;
    for (const double side : {-1.0,1.0}) {
        PureSignalSpectrum analyser;
        check(analyser.valid(), "paired spectrum creates a bounded FFT plan without a radio");
        std::array<std::complex<float>,512> tx{}, rx{};
        std::array<std::complex<float>,PureSignalSpectrum::kSamples> fullTx{}, fullRx{};
        const std::array<double,4> levels{-40,-50,-60,-70};
        for (int block = 0; block < 16; ++block) {
            for (std::size_t i = 0; i < tx.size(); ++i) {
                const double phase = -side*2*std::numbers::pi*(block*tx.size()+i)/PureSignalSpectrum::kSampleRate;
                const std::complex<double> reference = std::polar(0.2,phase*700)+std::polar(0.2,phase*1900);
                std::complex<double> observed = reference*0.5;
                for (int k = 0; k < 4; ++k) {
                    const double hz = (k+2)*700-(k+1)*1900;
                    observed += std::polar(0.1*std::pow(10.0,levels[k]/20),phase*hz);
                }
                tx[i] = {float(reference.real()),float(reference.imag())};
                rx[i] = {float(observed.real()),float(observed.imag())};
                fullTx[block*tx.size()+i] = tx[i]; fullRx[block*tx.size()+i] = rx[i];
            }
            check(analyser.feed(tx,rx,true,0), "finite paired spectra accept bounded synchronized samples");
            if (block < 15) { check(!analyser.snapshot().ready, "partial FFT windows cannot invent measured curves"); }
        }
        const auto& result = analyser.snapshot();
        check(result.ready && result.imdValid && !result.possibleClipping
            && result.frequencyHz.size() == result.transmitDbfs.size()
            && result.feedbackDbfs.size() == result.frequencyHz.size(),
            "complete paired data publishes actual spectra and validated two-tone estimates");
        for (int k = 0; k < 4; ++k) {
            std::printf("spectral IMD%d side=%+.0f measured=%.3f expected=%.1f dBc\n",2*k+3,side,result.imdDbc[k],levels[k]);
            check(std::abs(result.imdDbc[k]-levels[k]) < 0.25, "injected IMD products retain their known per-tone ratios");
        }
        double plottedTx = -160, plottedRx = -160;
        for (std::size_t i = 0; i < result.frequencyHz.size(); ++i) {
            if (std::abs(result.frequencyHz[i]-side*700) < 30) {
                plottedTx = std::max(plottedTx,result.transmitDbfs[i]);
                plottedRx = std::max(plottedRx,result.feedbackDbfs[i]);
            }
        }
        check(std::abs(plottedTx-20*std::log10(0.2)) < 0.3 && std::abs(plottedRx+20) < 0.3
            && std::is_sorted(result.frequencyHz.begin(),result.frequencyHz.end()),
            "plotted Hann-window levels preserve digital full scale and RF-side frequency orientation");
        check(analyser.fresh(500) && !analyser.fresh(501), "missing feedback expires the displayed measurement after 500 ms");
        check(analyser.feed(tx,rx,true,600) && !analyser.snapshot().ready,
            "a feedback outage requires an entirely fresh window before publishing again");
        check(analyser.feed(tx,rx,false,700) && !analyser.snapshot().ready && !analyser.snapshot().imdValid,
            "ending the generated two-tone context discards its old measurement window");
        analyser.reset();
        for (int block = 0; block < 16; ++block) {
            std::copy_n(fullTx.begin()+block*tx.size(),tx.size(),tx.begin());
            std::copy_n(fullRx.begin()+block*rx.size(),rx.size(),rx.begin());
            analyser.feed(tx,rx,false,800);
        }
        check(analyser.snapshot().ready && !analyser.snapshot().imdValid,
            "even valid carrier-shaped spectra cannot publish IMD without the generated two-tone context");
        analyser.reset();
        for (int block = 0; block < 16; ++block) {
            std::copy_n(fullTx.begin()+block*tx.size(),tx.size(),tx.begin());
            std::copy_n(fullRx.begin()+block*rx.size(),rx.size(),rx.begin());
            if (block == 0) { rx[0] = {1,0}; }
            analyser.feed(tx,rx,true,900);
        }
        check(analyser.snapshot().ready && analyser.snapshot().possibleClipping && !analyser.snapshot().imdValid,
            "possible sample clipping invalidates otherwise measurable carrier data");
        tx[0] = {std::numeric_limits<float>::quiet_NaN(),0};
        check(!analyser.feed(tx,rx,true,1000) && !analyser.snapshot().ready,
            "nonfinite feedback input clears the spectrum and its measurements");
        tx[0] = fullTx[0];
        check(!analyser.feed(tx,std::span(rx).first(rx.size()-1),true,1100) && !analyser.snapshot().ready,
            "unpaired spectrum input cannot form a measurement window");
    }
}

void spectrumContext()
{
    using namespace AetherSDR;
    anan::AnanTxDsp dsp;
    TxCoordinator coordinator([](const auto&,auto) {});
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    check(dsp.configure(WdspChannel::Mode::Usb) && dsp.preparePureSignalSpectrum() && dsp.begin(context,0),
        "spectrum owner prepares on the DSP planning path and starts with valid original media context");
    const std::array<std::complex<float>,512> samples{};
    for (int block = 0; block < 16; ++block) { dsp.feedPureSignalSpectrum(samples,samples,false,0); }
    check(!dsp.pureSignalSpectrum(0), "unrequested spectrum collection stays inactive");
    dsp.requestPureSignalSpectrum(true,0);
    for (int block = 0; block < 16; ++block) { dsp.feedPureSignalSpectrum(samples,samples,false,0); }
    check(dsp.pureSignalSpectrum(0), "requested paired samples publish through their active DSP owner");
    check(dsp.end() && !dsp.pureSignalSpectrum(0), "ending an over clears its displayed spectrum immediately");
    check(dsp.begin(context,0), "spectrum owner can start a separately admitted over");
    dsp.requestPureSignalSpectrum(true,0);
    for (int block = 0; block < 16; ++block) { dsp.feedPureSignalSpectrum(samples,samples,false,0); }
    check(dsp.pureSignalSpectrum(0), "new over fills its own fresh sample window");
    producer.invalidate();
    check(!dsp.pureSignalSpectrum(0), "retired producer context cannot publish a cached measurement");
}

void attenuationPolicy()
{
    using namespace AetherSDR;
    using Action = PureSignalAttenuator::Action;
    PureSignalSnapshot state;
    PureSignalAttenuator policy;
    policy.beginOver(1); state.information[5] = 1; state.information[4] = 220;
    check(policy.observe(state,true,false,10,0) == Action::None, "old calibration result cannot adjust a new over");
    state.information[5] = 2;
    check(policy.observe(state,true,false,10,0) == Action::Pause && policy.targetDb() == 13,
        "excess feedback requests a bounded attenuation increase and a native pause first");
    policy.accepted(Action::Pause,0,2);
    check(policy.observe(state,true,false,10,20) == Action::None,
        "attenuation cannot change before CALCC and IQC acknowledge the pause");
    check(policy.observe(state,true,true,10,30) == Action::Attenuate, "acknowledged pause permits its pending hardware change");
    policy.cancelOver(); policy.beginOver(2);
    check(policy.observe(state,true,true,10,50) == Action::Resume,
        "unkey cancels the old attenuation write even if the next over receives its acknowledgement");
    policy.accepted(Action::Resume,50,2);
    state.information[5] = 3; state.information[4] = 75;
    check(policy.observe(state,true,false,20,60) == Action::Pause && policy.targetDb() == 14,
        "weak valid feedback lowers attenuation using the logarithmic level ratio");
    policy.accepted(Action::Pause,60,3);
    check(policy.observe(state,true,true,20,70) == Action::Attenuate, "native acknowledgement precedes every update");
    policy.accepted(Action::Attenuate,70,3);
    check(policy.observe(state,true,true,14,169) == Action::None
        && policy.observe(state,true,true,14,170) == Action::Resume,
        "changed attenuation settles for 100 ms before calibration resumes");
    policy.accepted(Action::Resume,170,3);
    check(policy.observe(state,true,true,14,180) == Action::None, "one attempt causes at most one attenuation adjustment");
    state.information[5] = 4; state.information[4] = 300; state.information[0] = 1;
    check(policy.observe(state,true,false,14,200) == Action::None, "failed RX-builder results cannot actuate the attenuator");
    state.information[5] = 5; state.information[0] = 0;
    check(policy.observe(state,true,false,14,210) == Action::Pause && policy.targetDb() == 31,
        "overload requests maximum supported attenuation without exceeding the hardware range");
    policy.accepted(Action::Pause,210,5);
    check(policy.observe(state,false,false,14,220) == Action::None
        && policy.observe(state,false,true,14,230) == Action::Resume,
        "disabling automatic attenuation drains its pause without issuing the pending hardware write");
}

QByteArray correctionFixture()
{
    QByteArray result("correction_file_version 2\n");
    double checksum = 0;
    for (const char* name : {"MAG","COS","SIN"}) {
        const double value = name[0] == 'M' ? 0.8 : name[0] == 'C' ? 1 : 0;
        const double low = name[0] == 'M' ? 0.1 : -1.1;
        const double high = name[0] == 'M' ? 2 : 1.1;
        result += "curve " + QByteArray(name) + "\ncurve_ema_alpha 0.2\ncurve_ema_alpha_lo 0.1\ncurve_ema_x_bnd 0.25\ncurve_ema_clip ";
        result += QByteArray::number(low,'g',17) + ' ' + QByteArray::number(high,'g',17);
        result += "\ncurve_ema_count 3\ncurve_ema_warmup 4\ncurve_ema_pts 256\n";
        checksum += 4; checksum += 0.2 + 0.1 + 0.25 + low + high + 3;
        for (int i = 0; i < 256; ++i) { result += QByteArray::number(value,'g',17) + '\n'; checksum += value; }
        result += "n_branches 1\nbranch 0 n_pts 4 t_mid 0.5\n";
        checksum += 1; checksum += 4.5;
        for (int i = 0; i < 4; ++i) {
            const double x = i/3.0;
            result += QByteArray::number(x,'g',17) + ' ' + QByteArray::number(value,'g',17) + '\n';
            checksum += x + value;
        }
    }
    return result + "checksum " + QByteArray::number(checksum,'g',17) + '\n';
}

void correctionFiles()
{
    using namespace AetherSDR;
    using AetherSDR::anan::AnanTxDsp;
    const QByteArray bytes = correctionFixture();
    check(validatePureSignalCorrection(bytes),"source-format v2 correction and checksum are accepted");
    for (const auto& change : std::array<std::pair<QByteArray,QByteArray>,6>{{
        {"correction_file_version 2","correction_file_version 1"},
        {"curve_ema_pts 256","curve_ema_pts 255"},
        {"n_branches 1","n_branches 2147483647"},
        {"n_pts 4","n_pts 2147483647"},
        {"t_mid 0.5","t_mid nan"},
        {"curve MAG","curve COS"}}}) {
        QByteArray invalid = bytes; invalid.replace(change.first,change.second);
        check(!validatePureSignalCorrection(invalid),"malformed version, geometry, tags or nonfinite values are refused");
    }
    check(!validatePureSignalCorrection(bytes + "trailing"),"trailing correction input is refused");
    QByteArray corrupt = bytes; corrupt.replace("curve_ema_count 3","curve_ema_count 4");
    check(!validatePureSignalCorrection(corrupt),"modified coefficient document fails its checksum");
    check(!validatePureSignalCorrection(QByteArray(1024*1024+1,' ')),"oversized document is refused before tokenization");

    AnanTxDsp restored, copied, reference;
    check(restored.configure(WdspChannel::Mode::Usb) && copied.configure(WdspChannel::Mode::Usb)
        && reference.configure(WdspChannel::Mode::Usb),"coefficient tests build actual independent TXA channels");
    WdspChannel* channel = restored.pureSignalChannel();
    if (!channel || !copied.pureSignalChannel() || !reference.pureSignalChannel()) { return; }
    check(channel->configurePureSignal({},PureSignalMode::Current,192000),"restore target is prepared without calibration processing");
    check(channel->restorePureSignalCorrection(bytes.toStdString()),"restore installs real coefficients without waiting for audio");
    const auto snapshot = channel->pureSignalSnapshot(true);
    check(snapshot && snapshot->correctionAvailable && snapshot->correctionRunning && snapshot->correctionBusy
        && snapshot->measurements[0].empty() && snapshot->correction[1].size() == 512
        && std::abs(snapshot->correction[1][255]-0.8) < 1e-9,
        "restored correction reports its actual pending ramp and curves without inventing measurements");
    check(channel->trySetFilter(400,2900,reference.pureSignalChannel()),
          "corrected and uncorrected native channels accept one paired passband update");
    const auto filtered = channel->pureSignalSnapshot(true);
    check(filtered && snapshot && filtered->correctionAvailable
          && filtered->correctionRunning == snapshot->correctionRunning
          && filtered->correction[1] == snapshot->correction[1],
          "changing TX filters preserves actual installed IQC coefficients and correction state");
    const auto saved = channel->savePureSignalCorrection();
    check(saved && validatePureSignalCorrection(QByteArray::fromStdString(*saved)),"actual IQC coefficients save in valid v2 writer format");
    check(!channel->restorePureSignalCorrection(bytes.toStdString()),"restore cannot overwrite an existing correction channel");
    check(copied.pureSignalChannel()->configurePureSignal({},PureSignalMode::Current,192000)
        && copied.pureSignalChannel()->copyPureSignalCorrectionFrom(*channel),
        "Apply Current snapshots coefficients into another fresh channel");

    TxCoordinator coordinator([](const auto&, auto) {});
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    check(restored.begin(context,0) && reference.begin(context,0),"offline DSP exchanges start with a valid media context");
    double correctedEnergy = 0, referenceEnergy = 0;
    for (int block = 0; block < 80; ++block) {
        std::array<float,AnanTxDsp::kInputSamples> audio{};
        for (std::size_t i = 0; i < audio.size(); ++i) {
            audio[i] = float(0.1 * std::sin(2*std::numbers::pi*1000*(block*audio.size()+i)/AnanTxDsp::kInputRate));
        }
        for (int which = 0; which < 2; ++which) {
            AnanTxDsp& dsp = which == 0 ? restored : reference;
            std::array<std::complex<float>,AnanTxDsp::kOutputSamples> output{};
            QElapsedTimer elapsed; elapsed.start();
            AnanTxDsp::Result result;
            do {
                result = dsp.process(audio,context,0,output);
                if (result == AnanTxDsp::Result::WouldBlock) { QThread::msleep(1); }
            } while (result == AnanTxDsp::Result::WouldBlock && elapsed.elapsed() < 1000);
            if (result != AnanTxDsp::Result::Output) { check(false,"coefficient DSP exchange completes"); return; }
            if (block > 40) {
                for (const auto& value : output) {
                    (which == 0 ? correctedEnergy : referenceEnergy) += std::norm(value);
                }
            }
        }
    }
    check(referenceEnergy > 0 && std::abs(std::sqrt(correctedEnergy/referenceEnergy)-0.8) < 0.02,
        "restored IQC coefficients actually change TXA samples by their specified gain");
    check(channel->pausePureSignalCalibration(), "native calibration pause accepts an explicit control request");
    check(channel->pureSignalCalibrationPaused() == false, "a newly requested reset is not an acknowledgement");
    QElapsedTimer pauseTimer; pauseTimer.start();
    const std::array<std::complex<float>,128> feedback{};
    const std::array<float,AnanTxDsp::kInputSamples> silence{};
    while (channel->pureSignalCalibrationPaused() != true && pauseTimer.elapsed() < 2000) {
        channel->feedPureSignal(feedback,feedback,true);
        std::array<std::complex<float>,AnanTxDsp::kOutputSamples> output{};
        const auto result = restored.process(silence,context,0,output);
        if (result != AnanTxDsp::Result::Output && result != AnanTxDsp::Result::WouldBlock) {
            check(false,"native pause continues admitted DSP exchanges"); break;
        }
        QThread::msleep(1);
    }
    const auto stopped = channel->pureSignalSnapshot(false);
    check(channel->pureSignalCalibrationPaused() == true && stopped
        && !stopped->correctionRunning && !stopped->correctionBusy,
        "pause acknowledges only after the real retained IQC correction and END ramp drain");
    check(channel->resumePureSignalCalibration(PureSignalMode::Automatic),
        "acknowledged native pause resumes the selected calibration mode without keying");
    bool fed = true;
    for (int block = 0; block < 400; ++block) {
        fed = channel->feedPureSignal(feedback,feedback,true) && fed;
    }
    check(fed,"zero-valued paired samples still reach native calibration");
    const auto collecting = channel->pureSignalSnapshot(false);
    check(collecting && collecting->information[15] == 4 && collecting->information[4] == 0
        && !collecting->correctionRunning,
        "native collecting state and zero feedback level do not establish a usable calibration signal");

}

void feedbackHeadroom()
{
    using namespace AetherSDR;
    using anan::AnanTxDsp;
    AnanTxDsp raw, compensated;
    PureSignalSettings settings;
    settings.hardwarePeak = 0.2899; settings.moxWaitSeconds = 0; settings.amplifierDelayNs = 0;
    check(raw.configure(WdspChannel::Mode::Usb) && compensated.configure(WdspChannel::Mode::Usb),
        "headroom regression uses real independent TXA channels");
    if (!raw.pureSignalChannel() || !compensated.pureSignalChannel()) { return; }
    check(raw.pureSignalChannel()->configurePureSignal(settings,PureSignalMode::Single,192000)
        && compensated.pureSignalChannel()->configurePureSignal(settings,PureSignalMode::Single,192000),
        "native calibration keeps the published P2 hardware peak");
    TxCoordinator coordinator([](const auto&,auto) {});
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    check(raw.begin(context,0) && compensated.begin(context,0),"offline feedback retains admitted media context");
    std::array<std::complex<float>,128> tx{}, rx{};
    std::array<std::complex<float>,AnanTxDsp::kOutputSamples> output{};
    const std::array<float,AnanTxDsp::kInputSamples> silence{};
    QElapsedTimer timer; timer.start();
    int block = 0;
    bool completed = false;
    bool accepted = true;
    while (timer.elapsed() < 5000 && !completed) {
        for (std::size_t i = 0; i < tx.size(); ++i) {
            const double envelope = ((block*tx.size()+i)%4096+0.5)/4096.0;
            tx[i] = {static_cast<float>(0.5*settings.hardwarePeak*envelope),0};
            rx[i] = {static_cast<float>(0.01*envelope),0};
        }
        accepted = raw.pureSignalChannel()->feedPureSignal(tx,rx,true)
            && compensated.feedPureSignalFeedback(tx,rx,0) && accepted;
        raw.process(silence,context,0,output);
        compensated.process(silence,context,0,output);
        const auto state = compensated.pureSignalChannel()->pureSignalSnapshot(false);
        completed = state && state->information[7] > 0 && state->correctionAvailable;
        ++block; QThread::msleep(1);
    }
    const auto before = raw.pureSignalChannel()->pureSignalSnapshot(false);
    const auto after = compensated.pureSignalChannel()->pureSignalSnapshot(false);
    if (before && after) {
        std::printf("Reference proof: raw state=%d attempts=%d; compensated state=%d attempts=%d calibrations=%d peak=%.6f\n",
            before->information[15],before->information[7],after->information[15],after->information[7],
            after->information[5],after->maximumTransmit);
    }
    check(accepted && before && before->information[7] == 0 && before->information[15] == 4,
        "uncompensated half-scale reference cannot fill native upper amplitude buckets");
    check(completed && after && after->information[5] > 0 && after->correctionAvailable,
        "headroom compensation produces a completed native calibration and actual correction");
    check(after && std::abs(after->hardwarePeak-settings.hardwarePeak) < 1e-9
        && after->maximumTransmit > 0.99*settings.hardwarePeak,
        "reference normalization reaches the native peak without changing SetPk");
    check(compensated.end() && !compensated.feedPureSignalFeedback(tx,rx,0),
        "feedback normalization cannot process a retired transmission");
}
}

int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    {
        AetherSDR::anan::AnanFeedbackActivity activity;
        activity.observe(0.15,0.1,0.14495,0);
        check(activity.active(0),"modulated reference activates feedback indication");
        activity.observe(0,0,0.14495,200);
        check(!activity.active(251),"silent feedback blocks cannot keep a previous green level active");
        activity.observe(0.00001,0.00001,0.14495,300);
        check(!activity.active(300),"low reference noise is not treated as current modulation");
        activity.observe(0.1,0.1,0.14495,400); activity.reset();
        check(!activity.active(400),"new transmission retires prior feedback activity");
    }
    pairedSpectra();
    spectrumContext();
    attenuationPolicy();
    correctionFiles();
    feedbackHeadroom();
    using namespace AetherSDR;
    using namespace AetherSDR::anan;
    PureSignalSettings settings;
    check(settings.valid(),"Pure Signal defaults are valid");
    settings.hardwarePeak = 0;
    check(!settings.valid(),"zero peak cannot reach reciprocal WDSP scaling");
    settings = {}; settings.amplifierDelayNs = std::numeric_limits<double>::quiet_NaN();
    check(!settings.valid(),"nonfinite delay is refused");
    settings = {};
    std::vector<std::pair<quint16,QByteArray>> writes;
    P2Client modern;
    P2ClientTestAccess::seed(modern,writes);
    check(modern.setPureSignalEnabled(true),"P2 reserves feedback pair without keying");
    check(writes.size() == 2 && writes[0].first == 1025 && writes[0].second[23] == 2
        && std::uint8_t(writes[0].second[7]) == 0x0d && writes[0].second[1363] == 2,
        "P2 preserves RX DDC2/3 and synchronizes ADC0 feedback with digital TX reference");
    check(writes.size() == 2 && writes[1].first == 1027 && (writes[1].second[4] & 2) == 0,
        "arming Pure Signal never sets PTT");
    DiversitySettings diversity; diversity.enabled = true;
    check(!modern.setDiversity(diversity),"diversity cannot consume an active Pure Signal pair");
    check(modern.setPureSignalEnabled(false) && writes[writes.size()-2].second[1363] == 0,
        "OFF restores independent receiver routing");
    P2ClientTestAccess::limit(modern,3);
    check(!modern.setPureSignalEnabled(true),"insufficient DDCs refuse Pure Signal without losing RX");

    std::vector<p1::Packet> legacyWrites;
    P1Client legacy;
    P1ClientTestAccess::seed(legacy,legacyWrites);
    check(legacy.setPureSignalEnabled(true),"P1 enables a five-slot 192 kHz feedback path");
    check(!legacyWrites.empty() && legacyWrites.back()[13] == 0x40 && legacyWrites.back()[525] == 0x40
        && !(legacyWrites.back()[11] & 1) && !(legacyWrites.back()[523] & 1),
        "P1 primes both Pure Signal flags with MOX clear");
    check(!legacy.setDiversity(diversity),"P1 diversity cannot overlap reserved feedback");
    check(legacy.setPureSignalEnabled(false),"P1 clears Pure Signal flags");
    P1ClientTestAccess::rate(legacy,48);
    check(!legacy.setPureSignalEnabled(true),"P1 refuses an incompatible feedback rate");

    p1::ReceiveConfig config;
    config.numAdcs = 2; config.receivers = 5; config.rateKsps = 192; config.pureSignal = true;
    P2TxControl tx; tx.frequencyHz = 14e6;
    const auto packet = buildP1Transmit(config,tx,true,0,0x1c,0x0a);
    check(packet && ((*packet)[12]&15) == 8,
        "P1 keyed ADC map selects physical feedback and digital reference");
    check(packet && (*packet)[524] == 0 && (*packet)[525] == 0xd5 && (*packet)[526] == 0x9f && (*packet)[527] == 0x80,
        "P1 feedback VFO uses the TX frequency (14 MHz)");
    config.pureSignalAttenuationDb = 17;
    const auto attenuated = buildP1Transmit(config,tx,true,0,0x14,0x24);
    check(attenuated && (*attenuated)[15] == 0x31,
        "P1 keyed feedback attenuation retains enable bit and requested 17 dB");
    config.pureSignalAttenuationDb = 32;
    check(!buildP1Transmit(config,tx,true,0,0x14,0x24),
        "out-of-range P1 feedback attenuation cannot reach a wire packet");


    AnanTxDsp dsp;
    check(dsp.configure(WdspChannel::Mode::Usb),"real TXA channel constructs without RF transport");
    if (WdspChannel* channel = dsp.pureSignalChannel()) {
        check(channel->configurePureSignal(settings,PureSignalMode::Automatic,192000),
            "fresh WDSP channel accepts validated Pure Signal configuration");
        const auto status = channel->pureSignalSnapshot(true);
        check(status && std::abs(status->hardwarePeak-settings.hardwarePeak) < 1e-9
            && status->measurements[0].empty(),"WDSP reports real peak and no fabricated calibration samples");
        check(!channel->configurePureSignal(settings,PureSignalMode::Off,192000),
            "live correction channel cannot be reused as an unacknowledged OFF boundary");
        const std::array<std::complex<float>,2> samples{};
        check(!channel->feedPureSignal(samples,std::span(samples).first(1),false),
            "unpaired feedback cannot enter CALCC");
    }
    return failures ? 1 : 0;
}
