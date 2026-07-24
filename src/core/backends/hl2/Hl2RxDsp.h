#pragma once

#include <QObject>

#include <complex>
#include <memory>
#include <vector>

#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

namespace AetherSDR::hl2 {

// The HL2 receive DSP stage: turns raw IQ blocks (from MetisClient::iqBlockReady)
// into demodulated audio (WdspChannel), a panadapter spectrum (Hl2Spectrum), and
// an S-meter. Buffers the odd 126-sample EP6 blocks into WdspChannel's fixed
// processing block. Below the seam; the eventual Hl2Backend owns one and runs it
// on its own thread. RX-only (WdspChannel is a receive channel; nothing keys).
class Hl2RxDsp : public QObject {
    Q_OBJECT

public:
    explicit Hl2RxDsp(QObject* parent = nullptr);
    ~Hl2RxDsp() override;

    struct Config {
        int inputSampleRateHz = 48000;   // HL2 IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's native
        // RX rate (AudioEngine::DEFAULT_SAMPLE_RATE); emitting it directly means
        // the relay hands the engine byte-compatible float32 stereo with no
        // resampling. WDSP does the IF->audio decimation, and every HL2 IQ rate
        // (48/96/192/384 kHz) divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        int fftSize = 1024;              // panadapter FFT size
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // false (live): processIq is non-blocking — real-time input paces WDSP's
        // async worker and audio flows with ~1 block latency. true: processIq
        // waits for each output block (deterministic for a burst/offline feed).
        bool blockForOutput = false;
    };

    // (Re)build the WdspChannel + Hl2Spectrum for this config. Returns false (and
    // sets error, if given) when the WDSP channel cannot be created.
    bool configure(const Config& config, std::string* error = nullptr);
    void setMode(WdspChannel::Mode mode);
    void setFilter(double lowHz, double highHz);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady per FFT
    // frame and audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    void meterUpdate(float dbfs);                           // audio-RMS S-meter

private:
    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<Hl2Spectrum> m_spectrum;
    Config m_config;

    std::vector<std::complex<float>> m_iqBuffer;   // IQ awaiting a full DSP block
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch
};

}  // namespace AetherSDR::hl2
