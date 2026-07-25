#pragma once

namespace AetherSDR::hl2 {

// The single owner of everything that relates a raw dBFS number to a dBm one.
//
// WHY THIS IS ONE OBJECT
//
// Every LNA gain change shifts the absolute signal reference by exactly the
// same amount. If the panadapter displays dBm and the gain moves, the whole
// trace jumps and the waterfall paints a horizontal band that reads as a real
// on-air event. The fix is to apply an equal and opposite offset in the display
// chain -- and the only way to guarantee the two can never drift apart is to
// keep the gain value and its offset in the same object, which is what this is.
//
// This matters before there is any automatic RF AGC, because a manual gain
// change has the identical problem.
//
// WHAT IS AND IS NOT CALIBRATED
//
// The LNA term is exact: it is the gain we ourselves commanded, so removing it
// is arithmetic, not estimation. A gain change provably cannot move a reported
// dBm value.
//
// The absolute term (fullScaleDbm -- what 0 dBFS corresponds to at the antenna
// with 0 dB of LNA gain) is NOT calibrated here. It is a per-unit property of
// the board, the ADC reference and the front end, and none of the HL2 oracles
// state a figure for it; Quisk and SparkSDR both build per-unit calibration
// tables for the analogous TX power question rather than quoting a constant.
//
// So the default is 0.0, which reproduces exactly what this backend reported
// before this type existed: dBFS on a dBm-labelled axis. That is still wrong,
// but it is the SAME wrong, and it is now wrong in one labelled place with a
// setter, instead of being invisible. Silently substituting an invented
// constant would have made the numbers look calibrated without being so, which
// is worse than an honest offset of zero.
class Hl2DbReference {
public:
    // Gain we commanded on the AD9866 LNA, in dB.
    void setLnaGainDb(double db) noexcept { m_lnaGainDb = db; }
    double lnaGainDb() const noexcept { return m_lnaGainDb; }

    // What 0 dBFS means at the antenna with 0 dB LNA gain. Needs per-unit
    // calibration to be meaningful; 0.0 means "uncalibrated, reporting dBFS".
    void setFullScaleDbm(double dbm) noexcept { m_fullScaleDbm = dbm; }
    double fullScaleDbm() const noexcept { return m_fullScaleDbm; }
    bool isCalibrated() const noexcept { return m_fullScaleDbm != 0.0; }

    // The whole point: subtracting the gain we applied is what keeps a signal
    // of constant strength reading the same dBm across a gain change.
    double toDbm(double dbfs) const noexcept
    {
        return dbfs + m_fullScaleDbm - m_lnaGainDb;
    }

    // Offset form, for applying to a whole spectrum frame without a call per bin.
    double offsetDb() const noexcept { return m_fullScaleDbm - m_lnaGainDb; }

private:
    double m_lnaGainDb = 0.0;
    double m_fullScaleDbm = 0.0;
};

}  // namespace AetherSDR::hl2
