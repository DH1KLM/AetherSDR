#pragma once

#include "core/backends/anan_original/AnanDroopCorrection.h"

namespace AetherSDR::anan_original {

//DH1KLM Compiled-in droop-correction defaults for the ANAN-G2. connectRadio() seeds
//DH1KLM these, then overlays whatever the in-app sweep persisted for THIS radio.
//DH1KLM Derived from the Saturn gateware, not measured, so one curve fits every unit:
//DH1KLM ADC 122.88 MSPS -> CIC (6 stages, dd 1, R = 10..320) -> FIR (1024 taps, /8,
//DH1KLM 18-bit coefficients) -> 122.88e6 / (R * 8), i.e. 48..1536 ksps. The FIR
//DH1KLM transition band is the droop (flat +/-0.2 dB to 8% in from each edge, -6 dB at
//DH1KLM 5%, -15 at 4%, -30 at 3%, -54 at 2%); the CIC term varies 0.003 dB across rates,
//DH1KLM so defaultDroopTableForRate() returns the same table for every valid rate and
//DH1KLM only rejects invalid ones. Hardware check (G2, gateware 27): mean bias <= 0.13
//DH1KLM dB, RMS 0.46-0.72 dB at every rate. Clamped to [0, 90] dB, the same cap as
//DH1KLM AnanDroopCalibrator::computeCorrection() (the raw curve reaches 209 dB).
//DH1KLM Full derivation: tools/derive_droop_from_gateware.py.

//DH1KLM Trustworthy only from table point 14 inward: the gateware quantizes FIR taps
//DH1KLM to 18 bits (DDC_Block_fir_compiler_0_0.xci), which floors the real response
//DH1KLM near -125 dB, while this unquantized derivation runs to -209 dB (+94.8 dB error
//DH1KLM at bin 0, -2.2 at 11, <1 dB from 14 in, exact from 25). Points 0-13 are ~1.4%
//DH1KLM of the span, hidden only because applyEdgeFade() overwrites 3% (tailFraction
//DH1KLM 0.03, in another file). If that tail drops below ~0.014, re-derive against
//DH1KLM quantized taps. Applied regardless of reported gateware: slightly wrong beats
//DH1KLM uncorrected, and a sweep overrides it. Regenerate with
//DH1KLM tools/derive_droop_from_gateware.py.
inline constexpr int kDefaultsGatewareVersion = 27;

//DH1KLM The shipped table for one of the six valid DDC0 rates, or nullptr for any
//DH1KLM other rate. Every valid rate returns the SAME table -- see the note above.
[[nodiscard]] const DroopCorrectionTable* defaultDroopTableForRate(int rateKsps) noexcept;

//DH1KLM The six rates this ships defaults for, for callers that want to seed them
//DH1KLM all without hardcoding the list a second time.
[[nodiscard]] const std::array<int, 6>& defaultDroopRatesKsps() noexcept;

}  // namespace AetherSDR::anan_original
