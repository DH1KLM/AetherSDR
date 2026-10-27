#include "core/backends/anan/AnanTxDsp.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string>

using AetherSDR::TxCoordinator;
using AetherSDR::anan::AnanTxDsp;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

AnanTxDsp::Result exchange(AnanTxDsp& dsp, std::span<const float> audio,
    const TxCoordinator::Context& context, std::span<std::complex<float>> output)
{
    QElapsedTimer timer;
    timer.start();
    AnanTxDsp::Result result;
    do {
        result = dsp.process(audio, context, 0, output);
        if (result != AnanTxDsp::Result::WouldBlock) { return result; }
        QThread::msleep(1);
    } while (timer.elapsed() < 1000);
    return result;
}

void sideband(WdspChannel::Mode mode)
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    AnanTxDsp dsp;
    std::string error;
    check(dsp.configure(mode, &error), error.c_str());
    if (!error.empty()) { return; }
    check(dsp.begin(context, 0), "TX DSP accepts live media context");
    std::array<float, AnanTxDsp::kInputSamples> audio{};
    std::array<std::complex<float>, AnanTxDsp::kOutputSamples> iq{};
    std::complex<double> positive{}, negative{};
    std::size_t index = 0;
    double peak = 0;
    for (int block = 0; block < 90; ++block) {
        for (std::size_t i = 0; i < audio.size(); ++i) {
            audio[i] = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 1000
                * (block * audio.size() + i) / AnanTxDsp::kInputRate));
        }
        if (exchange(dsp, audio, context, iq) != AnanTxDsp::Result::Output) {
            check(false, "TX DSP exchanges a bounded USB/LSB audio block");
            return;
        }
        if (block < 40) { continue; }
        for (const std::complex<float>& sample : iq) {
            const double phase = 2 * std::numbers::pi * 1000 * index++ / AnanTxDsp::kOutputRate;
            const std::complex<double> value(sample.real(), sample.imag());
            positive += value * std::polar(1.0, -phase);
            negative += value * std::polar(1.0, phase);
            peak = std::max(peak, std::abs(value));
        }
    }
    const double wanted = std::abs(mode == WdspChannel::Mode::Usb ? negative : positive);
    const double unwanted = std::abs(mode == WdspChannel::Mode::Usb ? positive : negative);
    std::printf("mode=%s peak=%.6f sideband=%.1f dB\n",
        mode == WdspChannel::Mode::Usb ? "USB" : "LSB", peak,
        20 * std::log10(wanted / std::max(unwanted, 1e-20)));
    check(peak > 0.001 && peak <= 0.5, "TX IQ has signal and reserved digital headroom");
    check(wanted > 100 * unwanted, "correct HPSDR wire sideband exceeds opposite by 40 dB");
    check(dsp.end() && dsp.begin(context, 0), "new over discards previous DSP history");
    audio.fill(0);
    double residue = 0;
    for (int block = 0; block < 8; ++block) {
        check(exchange(dsp, audio, context, iq) == AnanTxDsp::Result::Output,
              "restarted silence exchanges normally");
        for (const auto& value : iq) { residue = std::max(residue, static_cast<double>(std::abs(value))); }
    }
    check(residue < 1e-6, "new silent over contains no previous speech tail");
    audio[0] = std::numeric_limits<float>::quiet_NaN();
    check(dsp.process(audio, context, 0, iq) == AnanTxDsp::Result::Refused,
          "NaN audio is refused before DSP exchange");
    audio.fill(0);
    check(dsp.process(audio, context, 0, iq) == AnanTxDsp::Result::Refused,
          "invalid input cannot silently resume without begin");
    check(dsp.begin(context, 0), "explicit begin after refused input");
    producer.invalidate();
    check(dsp.process(audio, context, 0, iq) == AnanTxDsp::Result::Refused,
          "revoked producer drops DSP output");
    check(!dsp.configure(WdspChannel::Mode::Am, &error), "unsupported TX mode fails closed");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    sideband(WdspChannel::Mode::Usb);
    sideband(WdspChannel::Mode::Lsb);
    return failures == 0 ? 0 : 1;
}
