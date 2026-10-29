#include "core/backends/anan/AnanTxDsp.h"
#include "core/backends/anan/AnanCarrier.h"

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

void sideband(WdspChannel::Mode mode, int rate = AnanTxDsp::kOutputRate)
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto producer = coordinator.registerProducer(true);
    const auto context = coordinator.mediaContext(producer);
    AnanTxDsp dsp;
    std::string error;
    check(dsp.configure(mode, &error, rate), error.c_str());
    if (!error.empty()) { return; }
    check(dsp.begin(context, 0), "TX DSP accepts live media context");
    std::array<float, AnanTxDsp::kInputSamples> audio{};
    std::array<std::complex<float>, AnanTxDsp::kOutputSamples> storage{};
    const auto iq = std::span(storage).first(dsp.outputSamples());
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
            const double phase = 2 * std::numbers::pi * 1000 * index++ / rate;
            const std::complex<double> value(sample.real(), sample.imag());
            positive += value * std::polar(1.0, -phase);
            negative += value * std::polar(1.0, phase);
            peak = std::max(peak, std::abs(value));
        }
    }
    const bool upper = mode == WdspChannel::Mode::Usb || mode == WdspChannel::Mode::Digu;
    const double wanted = std::abs(upper ? negative : positive);
    const double unwanted = std::abs(upper ? positive : negative);
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
    check(!dsp.configure(WdspChannel::Mode::Wbfm, &error), "receive-only broadcast mode fails closed");
}
void carriersAndModulators()
{
    for (const int rate : {48000, 192000}) {
        AetherSDR::anan::AnanCarrier carrier;
        check(std::abs(carrier.sample(rate)) == 0, "carrier starts silent");
        carrier.key(true);
        float previous = 0;
        for (int i = 0; i < rate / 200; ++i) {
            const auto value = carrier.sample(rate);
            check(value.imag() == 0 && value.real() >= previous && value.real() <= 0.5f,
                  "CW/TUNE carrier rises monotonically at the dial with 6 dB headroom");
            previous = value.real();
        }
        check(previous == 0.5f, "carrier reaches full envelope after five milliseconds");
        carrier.key(false);
        for (int i = 0; i < rate / 200; ++i) {
            const float value = carrier.sample(rate).real();
            check(value <= previous && value >= 0, "CW release envelope falls without a discontinuity");
            previous = value;
        }
        check(previous == 0, "carrier release reaches silence after five milliseconds");
        for (const auto mode : {WdspChannel::Mode::Am, WdspChannel::Mode::Sam,
                 WdspChannel::Mode::Fm, WdspChannel::Mode::Dsb}) {
            TxCoordinator coordinator([](const auto&, auto) {});
            const auto producer = coordinator.registerProducer(true);
            const auto context = coordinator.mediaContext(producer);
            AnanTxDsp dsp;
            std::string error;
            check(dsp.configure(mode, &error, rate), error.c_str());
            check(dsp.begin(context, 0), "modulator starts with live provenance");
            std::array<float, AnanTxDsp::kInputSamples> audio{};
            std::array<std::complex<float>, AnanTxDsp::kOutputSamples> storage{};
            const auto iq = std::span(storage).first(dsp.outputSamples());
            double peak = 0;
            double minimum = 1;
            for (int block = 0; block < 60; ++block) {
                for (std::size_t i = 0; i < audio.size(); ++i) {
                    audio[i] = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 1000
                        * (block * audio.size() + i) / AnanTxDsp::kInputRate));
                }
                if (exchange(dsp, audio, context, iq) != AnanTxDsp::Result::Output) {
                    check(false, "analog/digital modulator produces protocol-rate IQ");
                    break;
                }
                if (block < 40) { continue; }
                for (const auto value : iq) {
                    peak = std::max(peak, static_cast<double>(std::abs(value)));
                    minimum = std::min(minimum, static_cast<double>(std::abs(value)));
                }
            }
            std::printf("modulator=%d rate=%d envelope=%.6f..%.6f\n", static_cast<int>(mode),rate,minimum,peak);
            check(peak > 0.001 && peak <= 0.500001, "modulator has finite output with reserved headroom");
            if (mode == WdspChannel::Mode::Fm) {
                check(minimum > 0.99 * peak, "FM preserves a constant envelope");
            }
        }
    }
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    sideband(WdspChannel::Mode::Usb);
    sideband(WdspChannel::Mode::Lsb);
    sideband(WdspChannel::Mode::Usb, 48000);
    sideband(WdspChannel::Mode::Lsb, 48000);
    sideband(WdspChannel::Mode::Digu);
    sideband(WdspChannel::Mode::Digl);
    carriersAndModulators();
    AnanTxDsp unsupported;
    check(!unsupported.configure(WdspChannel::Mode::Drm), "DRM has no local transmit modem");
    return failures == 0 ? 0 : 1;
}
