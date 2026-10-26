#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AetherSDR::anan_original {

//DH1KLM ANAN-G2 panadapter spectrum via WDSP's display analyzer
//DH1KLM (third_party/wdsp/upstream/analyzer.c), configured to deskHPSDR's defaults:
//DH1KLM FFT = next power of two >= output points, min 16384; Kaiser window (PiAlpha
//DH1KLM 14); per-point AVERAGE detector (the main de-graining); overlap sized for one
//DH1KLM full FFT per display frame; operator time average in ms (0 = none, else WDSP
//DH1KLM mode 3 log-recursive, weight exp(-1 / (F * t)) at FFT rate F, or linear);
//DH1KLM levels normalised to one output point's bandwidth.
//DH1KLM Our addition: the running average is SEEDED from the first frame (after
//DH1KLM create and when averaging is re-enabled), because the analyzer is rebuilt on
//DH1KLM every rate change and an unseeded average fades in from -160 dB.
//DH1KLM Input is RAW wire IQ: Spectrum0() swaps I/Q, which is what orients the HPSDR
//DH1KLM convention correctly. Output runs lowest to highest frequency.
//DH1KLM Threading: create() plans FFTs (FFTW_PATIENT) off any real-time thread and
//DH1KLM takes WdspChannel::fftwSetupLock() itself; everything else, destructor
//DH1KLM included, runs on the one feeding thread (SetAnalyzer() resets the input ring
//DH1KLM and cannot race Spectrum0()).
class AnanPanAnalyzer {
public:
    struct Settings {
        int sampleRateHz = 48000;
        int numPoints = 1024;          // output points per frame
        int framesPerSecond = 25;      // display frame rate
        int averageTimeMs = 0;         // time average, ms; 0 = none
        bool logAverage = true;        // log-recursive (true) or linear-recursive
    };

    //DH1KLM Complex samples handed to the analyzer per Spectrum0() call. A power of
    //DH1KLM two, so it divides the analyzer's input ring (2 * kMaxFftSize).
    static constexpr int kBlockSize = 1024;
    //DH1KLM Largest FFT any setting can ask for, and the analyzer's buffer size.
    //DH1KLM XCreateAnalyzer sizes its buffers for this and for WDSP's own maxima
    //DH1KLM (dMAX_PIXOUTS x dMAX_AVERAGE x dMAX_PIXELS window-average frames among
    //DH1KLM them): about 45 MB per analyzer, and two exist briefly during a
    //DH1KLM rate-change rebuild.
    static constexpr int kMaxFftSize = 65536;
    static constexpr int kMinFftSize = 16384;
    //DH1KLM Most output points the analyzer can return (WDSP's dMAX_PIXELS).
    static constexpr int kMaxPoints = 16384;

    //DH1KLM Builds and configures the analyzer in slot `disp` (0..71). Returns
    //DH1KLM nullptr and sets `error` if the slot cannot be created.
    [[nodiscard]] static std::unique_ptr<AnanPanAnalyzer> create(int disp, const Settings& settings,
                                                                std::string* error = nullptr);
    ~AnanPanAnalyzer();
    AnanPanAnalyzer(const AnanPanAnalyzer&) = delete;
    AnanPanAnalyzer& operator=(const AnanPanAnalyzer&) = delete;

    [[nodiscard]] int disp() const noexcept { return m_disp; }
    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }
    [[nodiscard]] int numPoints() const noexcept { return m_settings.numPoints; }
    [[nodiscard]] int framesPerSecond() const noexcept { return m_settings.framesPerSecond; }

    //DH1KLM The frame-rate-dependent settings -- overlap and the averaging weights --
    //DH1KLM re-applied for a new display rate. The FFT size does not change, so
    //DH1KLM nothing is re-planned; the running average is kept.
    void setFramesPerSecond(int fps);

    //DH1KLM The output point count, for a new panel width. Clamped to
    //DH1KLM 2..kMaxPoints. The FFT size cannot change -- every count up to
    //DH1KLM kMaxPoints fits in kMinFftSize -- so nothing is re-planned, but the
    //DH1KLM running average is per point, so the next frame re-seeds it.
    void setNumPoints(int points);

    //DH1KLM The time average, in ms: 0 = none, t > 0 = log-recursive with time
    //DH1KLM constant t. Cheap -- no re-plan, no ring reset. Switching averaging back
    //DH1KLM on re-seeds from the next frame.
    void setAverageTimeMs(int ms);

    //DH1KLM Log-recursive (true, deskHPSDR's default) or linear-recursive (false)
    //DH1KLM averaging. Cheap; re-seeds, since entering either mode resets WDSP's
    //DH1KLM history.
    void setLogAverage(bool on);

    //DH1KLM Stage raw wire IQ and hand the analyzer every whole kBlockSize block.
    //DH1KLM A partial block is carried to the next call.
    void feed(std::span<const std::complex<float>> iq);

    //DH1KLM Drop a partially staged block, returning how many samples went with it
    //DH1KLM (0 = nothing was in flight). The analyzer's own history is left alone.
    std::size_t dropStagedPartial() noexcept;

    //DH1KLM If the analyzer has produced a frame since the last call, copy it into
    //DH1KLM `pointsDb` (resized to numPoints(), dB, lowest frequency first) and
    //DH1KLM return true. Returns false, leaving `pointsDb` untouched, otherwise.
    bool takeFrame(std::vector<float>& pointsDb);

    //DH1KLM The derived analyzer parameters for a given setting -- exposed so tests
    //DH1KLM can pin the formulas without a running analyzer.
    struct Derived {
        int fftSize = 0;
        int overlap = 0;
        int maxWriteahead = 0;
        double avBackmult = 0.0;
        int numAverage = 0;
    };
    [[nodiscard]] static Derived derive(const Settings& settings) noexcept;

private:
    AnanPanAnalyzer(int disp, const Settings& settings);
    void applySettings();

    int m_disp;
    Settings m_settings;
    int m_fftSize = 0;
    double m_avBackmult = 0.0;
    void applyAveraging();
    bool m_seeded = false;
    std::vector<double> m_staged;          // interleaved I,Q -- 2 * kBlockSize
    std::size_t m_stagedCount = 0;         // complex samples in m_staged
    std::vector<float> m_scratch;          // GetPixels target, numPoints
};

}  // namespace AetherSDR::anan_original
