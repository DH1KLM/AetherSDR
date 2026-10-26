#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

//DH1KLM The ANAN receiver's own audio stage: mute, AF gain and left/right balance,
//DH1KLM applied to demodulated stereo before it leaves the backend. It lives here, not
//DH1KLM in AudioEngine: the engine expects radio audio with per-slice pan already
//DH1KLM applied (docs/architecture/audio-pipeline.md), and its setRxVolume()/setMuted()
//DH1KLM are master controls. Socket-, Qt- and DSP-free so the law is testable.
namespace AetherSDR::anan_original {

//DH1KLM Silence, and the gain range in dB below unity. A dB law because a fader is a
//DH1KLM perceptual control; 40 dB keeps usable resolution at every position while
//DH1KLM still reaching "audibly off" at the bottom.
inline constexpr double kSliceAudioRangeDb = 40.0;

//DH1KLM 0 -> silence, 100 -> unity, 50 -> -20 dB. Exactly zero at 0 (not -40 dB,
//DH1KLM which is still audible on a strong signal): a fader at its stop is silent.
[[nodiscard]] inline float sliceAudioAmplitude(int gainPercent) noexcept
{
    if (gainPercent <= 0) {
        return 0.0f;
    }
    if (gainPercent >= 100) {
        return 1.0f;
    }
    const double db = -kSliceAudioRangeDb + (kSliceAudioRangeDb / 100.0) * gainPercent;
    return static_cast<float>(std::pow(10.0, 0.05 * db));
}

//DH1KLM Balance on the slice model's 0 left / 50 centre / 100 right scale. Attenuates
//DH1KLM the opposite channel and never boosts, so panning cannot clip; same law as the
//DH1KLM client's own pan stage so loudness matches across receiver types.
[[nodiscard]] inline float sliceAudioLeftPanGain(int panPercent) noexcept
{
    return panPercent >= 50 ? static_cast<float>(100 - panPercent) / 50.0f : 1.0f;
}

[[nodiscard]] inline float sliceAudioRightPanGain(int panPercent) noexcept
{
    return panPercent <= 50 ? static_cast<float>(panPercent) / 50.0f : 1.0f;
}

//DH1KLM Quantise one sample to the stream's signed 16-bit wire format. Clamped before
//DH1KLM scaling: the resampler overshoots on transients and an unclamped cast wraps
//DH1KLM sign. The non-finite guard must be separate from the clamp -- std::clamp
//DH1KLM returns v unless v < lo or hi < v, and every NaN comparison is false -- and
//DH1KLM covers infinity too, which clamps correctly but would emit a full-scale click.
[[nodiscard]] inline std::int16_t sliceAudioToInt16(float v) noexcept
{
    if (!std::isfinite(v)) {
        return 0;
    }
    return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
}

//DH1KLM Apply all three to interleaved L,R float32 in place. Mute zeroes the output
//DH1KLM without touching gain, so unmuting restores the operator's level.
inline void applySliceAudioInPlace(float* stereo, std::size_t frames, bool muted,
                                  int gainPercent, int panPercent) noexcept
{
    if (stereo == nullptr || frames == 0) {
        return;
    }
    if (muted) {
        for (std::size_t i = 0; i < frames * 2; ++i) {
            stereo[i] = 0.0f;
        }
        return;
    }
    const float amplitude = sliceAudioAmplitude(gainPercent);
    const float left = amplitude * sliceAudioLeftPanGain(panPercent);
    const float right = amplitude * sliceAudioRightPanGain(panPercent);
    //DH1KLM Unity on both sides is the overwhelmingly common case (gain 100, centred),
    //DH1KLM and this runs on every audio block, so skip the multiply rather than
    //DH1KLM multiply by 1.0f forty-eight thousand times a second.
    if (left == 1.0f && right == 1.0f) {
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        stereo[i * 2]     *= left;
        stereo[i * 2 + 1] *= right;
    }
}

}  // namespace AetherSDR::anan_original
