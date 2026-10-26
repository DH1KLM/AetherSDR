#pragma once

#include "core/backends/anan_original/AnanDroopCorrection.h"
#include "core/backends/anan_original/P2Protocol.h"   // kDdc0RatesKsps
#include "core/RadioSettingsScope.h"

#include <QElapsedTimer>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <array>
#include <functional>
#include <utility>

namespace AetherSDR::anan_original {

//DH1KLM Backend-owned sweep engine. Hooks are an injectable, socket-free boundary to
//DH1KLM ANAN's rate control, DSP bypass and persistence. No shared RadioModel or GUI
//DH1KLM object participates in the sweep; only AnanBackend feeds its spectrum/rate.
class AnanDroopCalibrator : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        std::function<bool()> available;
        std::function<void(int)> requestRate;
        std::function<void(bool)> bypass;
        std::function<QString(const QMap<int, anan_original::DroopCorrectionTable>&)> apply;
    };
    explicit AnanDroopCalibrator(Hooks hooks = {}, QObject* parent = nullptr);
    void setLandedRate(int rateKsps) { m_landedRateKsps = rateKsps; }
    void onSpectrumFrame(const std::vector<float>& binsDbm);

    enum class Phase { Idle, WaitingForRateLanded, Settling, Sampling };

    [[nodiscard]] bool isRunning() const noexcept { return m_phase != Phase::Idle; }
    [[nodiscard]] int  rateIndex() const noexcept { return m_rateIdx; }
    [[nodiscard]] int  totalRates() const noexcept { return static_cast<int>(kRatesKsps.size()); }
    [[nodiscard]] bool hasResult() const { return !m_measuredTables.isEmpty(); }
    [[nodiscard]] const QMap<int, anan_original::DroopCorrectionTable>& measuredTables() const
    {
        return m_measuredTables;
    }

    //DH1KLM ---- pure math (static, unit-testable without a live radio) ----
    using Curve = std::array<float, anan_original::kDroopCorrectionFftSize>;

    //DH1KLM Per-bin MEDIAN across N dB captures (robust to a stray in-band signal in one
    //DH1KLM capture on a live antenna). Computed in linear power and converted back: a
    //DH1KLM no-op for a median, but correct if the reducer ever becomes a mean.
    [[nodiscard]] static Curve medianPowerCurve(const QVector<Curve>& captures);

    //DH1KLM Median of the curve over a central window (default: center +/- 15% of
    //DH1KLM the bins), not the exact center bin -- WDSP's analyzer does no DC
    //DH1KLM removal, so the centre point can carry a DC spike; the median over the
    //DH1KLM window ignores it.
    [[nodiscard]] static float referenceLevel(const Curve& curve,
                                              float windowFraction = 0.15f);

    //DH1KLM correction[k] = clamp(referenceDb - curve[k], 0, capDb). Measured into a
    //DH1KLM dummy load, so `curve` is the noise floor and droop is a deterministic CIC/
    //DH1KLM decimation attenuation applied equally to signal and noise; restoring it is not
    //DH1KLM amplifying noise. Bench edges at 1536 ksps sit 15-20 dB below a 70 dB-capped
    //DH1KLM correction, so the cap is 90 dB; it only bounds a garbage measurement.
    [[nodiscard]] static anan_original::DroopCorrectionTable computeCorrection(
        const Curve& curve, float referenceDb, float capDb = 90.0f);

    //DH1KLM ---- persistence (static, no live radio needed) ----
    static constexpr const char* kFeature = "DroopCalibration";
    static constexpr int kSchemaVersion = 1;

    //DH1KLM Reads whatever was last persisted for this radio (possibly a partial
    //DH1KLM set of rates, or none). Missing/unparseable entries are simply
    //DH1KLM absent from the result -- AnanRxDsp's own per-rate fallback
    //DH1KLM (kDroopCorrectionZero) covers a rate this never measured.
    [[nodiscard]] static QMap<int, anan_original::DroopCorrectionTable> loadTables(
        const RadioSettingsScope& scope);

    //DH1KLM Write half of loadTables(); the float<->JSON codec and validity rules live
    //DH1KLM here only. MERGES into this radio's existing tables, since a partial sweep's
    //DH1KLM Apply carries only the rates it measured (see stop()). Refuses a stored schema
    //DH1KLM newer than this build. Returns "" on success, else the operator-facing reason.
    //DH1KLM Sole caller: AnanBackend::invokeExtension()'s applyDroopTables().
    [[nodiscard]] static QString saveTables(
        const RadioSettingsScope& scope,
        const QMap<int, anan_original::DroopCorrectionTable>& tables);

public slots:
    //DH1KLM Begins a 6-rate sweep from Idle. No-op if already running or the
    //DH1KLM radio has no active panadapter yet.
    void start();

    //DH1KLM Aborts a running sweep: stops the poll timer (nothing else is awaited), lifts
    //DH1KLM the DSP bypass, best-effort restores the pre-sweep rate (restoreRate=false on
    //DH1KLM disconnect/destruction) and returns to Idle. Rates already measured are KEPT;
    //DH1KLM hasResult()/applyResult() work with a partial set.
    void stop(bool restoreRate = true);

    //DH1KLM Applies and persists the staged result through the owning backend.
    //DH1KLM Emits error(reason) on refusal and finished(true) only on success.
    void applyResult();

    //DH1KLM Drops any measured (but not yet applied) tables. No-op while running.
    void clear();

signals:
    void started();
    void progress(int rateIndex, int totalRates, int percent);
    void rateSampled(int rateKsps, int sampleCount);
    void finished(bool applied);
    void error(const QString& reason);

private slots:
    void advance();   // one poll tick of the phase state machine

private:
    void requestCurrentRate();
    void finishSweep(bool applied, bool restoreRate = true);
    void abortSweep(const QString& reason);
    [[nodiscard]] int currentTargetRateKsps() const;

    //DH1KLM Bypass both correction and cosmetic edge fade while measuring the
    //DH1KLM backend's spectrum. The ANAN owner queues this before requesting a rate.
    void setDspBypass(bool bypassed);

    //DH1KLM The sweep covers every rate the radio has, in the order it has them --
    //DH1KLM see anan_original::kDdc0RatesKsps for why the set is spelled in exactly one place.
    static constexpr auto& kRatesKsps = anan_original::kDdc0RatesKsps;
    static constexpr int kSamplesPerRate = 8;
    static constexpr int kSampleSpacingMs = 300;
    static constexpr int kRateWaitTimeoutMs = 90'000;  // "~a minute cold" + margin
    //DH1KLM The analyzer reseeds its average from the first frame at a new rate
    //DH1KLM (AnanPanAnalyzer), so this only covers the first frames landing: 500 ms is a
    //DH1KLM dozen frames at 25 fps.
    static constexpr int kPostLandSettleMs = 500;
    static constexpr int kPollIntervalMs = 200;
    //DH1KLM Bound on Sampling without frames (hidden/paused panadapter, short frames,
    //DH1KLM network drop); otherwise isRunning() latches, the correction stays bypassed
    //DH1KLM and the radio stays at the sweep rate. Generous for low spectrum FPS.
    static constexpr int kSampleStallTimeoutMs = 15'000;

    Hooks m_hooks;
    int m_landedRateKsps = 0;
    QTimer m_pollTimer;
    Phase m_phase = Phase::Idle;
    int m_rateIdx = -1;
    qint64 m_phaseStartedAtMs = 0;
    qint64 m_lastSampleAtMs = 0;
    QElapsedTimer m_clock;
    int m_originalRateKsps = 0;

    bool m_haveLatestFrame = false;
    Curve m_latestFrame{};
    QVector<Curve> m_pendingCaptures;
    QMap<int, anan_original::DroopCorrectionTable> m_measuredTables;
};

}  // namespace AetherSDR

