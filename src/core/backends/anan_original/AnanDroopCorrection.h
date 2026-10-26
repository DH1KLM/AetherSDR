#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace AetherSDR::anan_original {

//DH1KLM Saturn DDC0 (6-stage CIC, R = 10..320, then a 1024-tap decimate-by-8 FIR)
//DH1KLM rolls off amplitude near the span edges in the raw IQ itself. Across the DDC
//DH1KLM output band that is the FIR's transition band, not CIC sin(x)/x droop (CIC adds
//DH1KLM <= 0.34 dB, varying 0.003 dB across rates), so one curve fits all six rates
//DH1KLM (AnanDroopDefaults.h). Changing bin count or reported bandwidth breaks zoom-out
//DH1KLM (AnanBackend::emitPanState()), so the fix is a per-bin dB correction to the FFT
//DH1KLM magnitude. This header owns the data shape and apply math; AnanRxDsp holds the
//DH1KLM live tables (derived defaults overlaid by AnanDroopCalibrator measurements).

inline constexpr int kDroopCorrectionFftSize = 1024;

using DroopCorrectionTable = std::array<float, kDroopCorrectionFftSize>;

//DH1KLM Safe no-op fallback for an unrecognized/uncalibrated rate -- additive
//DH1KLM zero, not a guess.
extern const DroopCorrectionTable& kDroopCorrectionZero;

//DH1KLM Adds the per-bin dB correction into binsDbfs in place. Pure, no I/O, no
//DH1KLM AnanRxDsp state -- unit-testable standalone. A size mismatch leaves
//DH1KLM binsDbfs byte-for-byte unchanged rather than truncating or asserting: a
//DH1KLM table generated for a different fftSize must never silently misalign bin
//DH1KLM k against the wrong correction.
inline void applyDroopCorrectionDb(std::vector<float>& binsDbfs,
                                    const DroopCorrectionTable& table) noexcept
{
    if (binsDbfs.size() != table.size())
        return;
    for (std::size_t i = 0; i < binsDbfs.size(); ++i)
        binsDbfs[i] += table[i];
}

//DH1KLM Value of a kDroopCorrectionFftSize-point curve at output point `i` of
//DH1KLM `points`, by linear interpolation. Both grids run edge to edge over the
//DH1KLM same span with their end points on the span's edges -- the analyzer's
//DH1KLM point grid is laid out that way for any count -- so point i sits at
//DH1KLM fraction i / (points - 1) of the span on either grid.
inline float droopCurveAt(const DroopCorrectionTable& table, std::size_t i,
                          std::size_t points) noexcept
{
    if (points < 2)
        return table[kDroopCorrectionFftSize / 2];
    const double x = static_cast<double>(i) * (kDroopCorrectionFftSize - 1)
        / static_cast<double>(points - 1);
    const auto j = std::min(static_cast<std::size_t>(x),
                            static_cast<std::size_t>(kDroopCorrectionFftSize - 2));
    const float frac = static_cast<float>(x - static_cast<double>(j));
    return table[j] + frac * (table[j + 1] - table[j]);
}

//DH1KLM applyDroopCorrectionDb() for a point count that follows the panel width.
//DH1KLM The tables stay at kDroopCorrectionFftSize points so stored calibrations
//DH1KLM and the derived defaults need no re-sweep; the correction is a smooth
//DH1KLM gain-versus-frequency curve, so reading it between its points is sound.
//DH1KLM At exactly kDroopCorrectionFftSize points this is applyDroopCorrectionDb().
inline void applyDroopCorrectionDbResampled(std::vector<float>& pointsDb,
                                            const DroopCorrectionTable& table) noexcept
{
    const std::size_t n = pointsDb.size();
    if (n == table.size()) {
        applyDroopCorrectionDb(pointsDb, table);
        return;
    }
    if (n < 2)
        return;
    for (std::size_t i = 0; i < n; ++i)
        pointsDb[i] += droopCurveAt(table, i, n);
}

//DH1KLM The mean of the straight line through `v`'s points over [a, b], with
//DH1KLM 0 <= a <= b <= v.size() - 1 and v.size() >= 2. Exact per segment (the
//DH1KLM trapezoid of a straight line is its integral), so a straight-line input
//DH1KLM comes back as its value at the window's centre. a == b gives that point.
inline double lineMeanOver(const std::vector<float>& v, double a, double b) noexcept
{
    const std::size_t n = v.size();
    const auto at = [&](double x) {
        const auto j = std::min(static_cast<std::size_t>(x), n - 2);
        const double t = x - static_cast<double>(j);
        return static_cast<double>(v[j]) + t * (static_cast<double>(v[j + 1]) - v[j]);
    };
    if (b <= a)
        return at(a);
    double sum = 0.0;
    for (double x0 = a; x0 < b;) {
        const double x1 = std::min(b, std::floor(x0) + 1.0);
        sum += 0.5 * (at(x0) + at(x1)) * (x1 - x0);
        x0 = x1;
    }
    return sum / (b - a);
}

//DH1KLM The reverse direction, for the calibrator: a frame of any point count read
//DH1KLM onto the kDroopCorrectionFftSize grid. Returns false, leaving `out` untouched,
//DH1KLM for fewer than two points. A wider frame is averaged over each table point's
//DH1KLM cell (narrowed symmetrically at the span ends) so the stored calibration keeps
//DH1KLM the FFT averaging; a narrower or equal frame is linearly interpolated.
inline bool resampleToDroopGrid(const std::vector<float>& pointsDb,
                                DroopCorrectionTable& out) noexcept
{
    const std::size_t n = pointsDb.size();
    if (n < 2)
        return false;
    const double last = static_cast<double>(n - 1);
    const double step = last / (kDroopCorrectionFftSize - 1);
    const double half = n > kDroopCorrectionFftSize ? step / 2.0 : 0.0;
    for (std::size_t k = 0; k < kDroopCorrectionFftSize; ++k) {
        const double x = static_cast<double>(k) * step;
        const double h = std::min({half, x, last - x});
        out[k] = static_cast<float>(lineMeanOver(pointsDb, x - h, x + h));
    }
    return true;
}

//DH1KLM Cosmetic raised-cosine fade over the outermost `tailFraction` of bins each
//DH1KLM side, applied AFTER applyDroopCorrectionDb(): from the boundary value down to
//DH1KLM (boundary - fadeDb). At the true edge the droop is near the ADC floor and too
//DH1KLM noisy for any per-bin correction (a 90 dB cap is no better than 70), so the
//DH1KLM display gets a repeatable roll-off instead of a dark glitch band. WDSP makes
//DH1KLM the same call (SetAnalyzer `clp`, WDSP_Guide Rev 2.00 §7.2); we fade rather
//DH1KLM than clip bins because bin count/bandwidth changes break zoom-out.
inline void applyEdgeFade(std::vector<float>& binsDbfs,
                           float tailFraction = 0.03f,
                           float fadeDb = 12.0f) noexcept
{
    const auto n = binsDbfs.size();
    const auto tailBins = static_cast<std::size_t>(static_cast<float>(n) * tailFraction);
    //DH1KLM Require at least one untouched bin strictly between the two tail
    //DH1KLM zones -- otherwise they'd overlap (or abut with no gap), and
    //DH1KLM rightBoundary below could read a bin the left loop already
    //DH1KLM overwrote.
    if (tailBins < 2 || n <= tailBins * 2)
        return;

    constexpr float kPi = 3.14159265358979323846f;

    //DH1KLM Left edge: bin 0 is the true edge, bin tailBins is the boundary this
    //DH1KLM fade blends FROM (left untouched). u runs 1 (true edge) -> 0
    //DH1KLM (boundary), so the raised-cosine window is 0 right at the boundary
    //DH1KLM (perfect continuity with the untouched region) and 1 at the true
    //DH1KLM edge (full fadeDb applied).
    const float leftBoundary = binsDbfs[tailBins];
    for (std::size_t k = 0; k < tailBins; ++k) {
        const float u = static_cast<float>(tailBins - k) / static_cast<float>(tailBins);
        const float window = 0.5f * (1.0f - std::cos(u * kPi));
        binsDbfs[k] = leftBoundary - fadeDb * window;
    }

    //DH1KLM Right edge: mirror image, boundary at n-1-tailBins.
    const float rightBoundary = binsDbfs[n - 1 - tailBins];
    for (std::size_t k = 0; k < tailBins; ++k) {
        const std::size_t idx = n - 1 - k;
        const float u = static_cast<float>(tailBins - k) / static_cast<float>(tailBins);
        const float window = 0.5f * (1.0f - std::cos(u * kPi));
        binsDbfs[idx] = rightBoundary - fadeDb * window;
    }
}

}  // namespace AetherSDR::anan_original
