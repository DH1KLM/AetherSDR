#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <cstddef>
#include <string_view>
#include <span>
#include <vector>

namespace AetherSDR::anan {
//DH1KLM: Logical model IDs are Thetis HPSDRModel values, not discovery IDs.
// Sources and qualification limits: docs/architecture/anan-model-profiles.md.
enum class AnanModel : int {
    Automatic = -1, Hpsdr = 0, Hermes = 1, Anan10 = 2, Anan10E = 3,
    Anan100 = 4, Anan100B = 5, Anan100D = 6, Anan200D = 7,
    OrionMkII = 8, Anan7000 = 9, Anan8000 = 10, AnanG2 = 11,
    AnanG21K = 12, AnvelinaPro3 = 13, HermesLite2 = 14,
    RedPitaya = 15, AnanG2E = 16
};
// Classic ALEX's extra 30 dB bank is in the ADC0 path only. MkII reuses
// its bits for input relays; model overrides therefore select this policy.
[[nodiscard]] constexpr bool hasClassicAlexAttenuator(AnanModel model, int board) noexcept
{
    if (model == AnanModel::Automatic) { return board >= 1 && board <= 4; }
    return model == AnanModel::Hermes || model == AnanModel::Anan100
        || model == AnanModel::Anan100B || model == AnanModel::Anan100D
        || model == AnanModel::Anan200D;
}
struct ReceiveAttenuation {
    int totalDb;
    int adcDb;
    int alexDb;
};
[[nodiscard]] constexpr ReceiveAttenuation receiveAttenuation(int requestedDb, bool classicAlex) noexcept
{
    const int maximum = classicAlex ? 61 : 31;
    const int total = requestedDb < 0 ? 0 : requestedDb > maximum ? maximum : requestedDb;
    const int alex = classicAlex && total > 31 ? 30 : 0;
    return {total, total - alex, alex};
}
[[nodiscard]] constexpr ReceiveAttenuation receiveAttenuationPreset(int step, bool classicAlex) noexcept
{
    const int maximum = classicAlex ? 5 : 3;
    step = step < 0 ? 0 : step > maximum ? maximum : step;
    if (!classicAlex) { return receiveAttenuation(step * 10, false); }
    if (step <= 3) { return {step * 10, 0, step * 10}; }
    return {step * 10, 20, (step - 2) * 10};
}
struct ReceiveTuning {
    double centerHz;
    double shiftHz;
};
// Thetis console.cs uses 92% of the sampled span, including filter margins.
// Far tuning jumps and narrower spans recenter; in-window tuning leaves the NCO fixed.
[[nodiscard]] inline std::optional<ReceiveTuning> receiveTuning(bool ctun,
    double centerHz, double sliceHz, double rateHz, double lowHz, double highHz, double bfoHz) noexcept
{
    if (!std::isfinite(centerHz) || !std::isfinite(sliceHz) || !std::isfinite(rateHz)
        || !std::isfinite(lowHz) || !std::isfinite(highHz) || !std::isfinite(bfoHz)
        || sliceHz < 0 || sliceHz > 61'440'000 || centerHz < 0 || centerHz > 61'440'000
        || rateHz <= 0 || lowHz >= highHz) { return std::nullopt; }
    if (!ctun) { return ReceiveTuning{sliceHz, -bfoHz}; }
    const double half = rateHz * 0.46;
    const double offset = sliceHz - centerHz;
    const double lowerMargin = std::min(0.0, lowHz + bfoHz);
    const double upperMargin = std::max(0.0, highHz + bfoHz);
    if (offset + lowerMargin < -half || offset + upperMargin > half) {
        centerHz = sliceHz;
    }
    return ReceiveTuning{centerHz, sliceHz - centerHz - bfoHz};
}
struct ReceiveBandView {
    double centerHz;
    int rateKsps;
    bool wholeBand;
    double spanHz; // pre-edge-crop display span, independently of the capture rate
};
// Display-only envelope, not permission to transmit. Uses 90% for the band
// inside the 92% usable capture, leaving room for the edge markers.
[[nodiscard]] inline std::optional<ReceiveBandView> receiveBandView(double lowHz,
    double highHz, double sliceHz, int maximumRateKsps,
    double filterLowHz, double filterHighHz, double bfoHz) noexcept
{
    if (!std::isfinite(lowHz) || !std::isfinite(highHz) || lowHz < 0
        || highHz > 61'440'000 || lowHz >= highHz || sliceHz < lowHz || sliceHz > highHz
        || !receiveTuning(true, sliceHz, sliceHz, 48'000, filterLowHz, filterHighHz, bfoHz)) {
        return std::nullopt;
    }
    const double lowerMargin = std::min(0.0, filterLowHz + bfoHz);
    const double upperMargin = std::max(0.0, filterHighHz + bfoHz);
    const double neededSpan = std::max(highHz, sliceHz + upperMargin)
        - std::min(lowHz, sliceHz + lowerMargin);
    constexpr std::array rates{48, 96, 192, 384, 768, 1536};
    int selected = 0;
    for (int rate : rates) {
        if (rate > maximumRateKsps) { break; }
        selected = rate;
        if (neededSpan <= rate * 1000.0 * 0.90) { break; }
    }
    if (!selected) { return std::nullopt; }
    const double half = selected * 1000.0 * 0.46;
    // Clamp the desired band centre so both the tuned signal and passband
    // remain in the usable capture. Never move the receive frequency.
    const double minimumCenter = std::max(0.0, sliceHz + upperMargin - half);
    const double maximumCenter = std::min(61'440'000.0, sliceHz + lowerMargin + half);
    if (minimumCenter > maximumCenter) { return std::nullopt; }
    const double center = std::clamp((lowHz + highHz) * 0.5, minimumCenter, maximumCenter);
    const bool whole = lowHz >= center - half && highHz <= center + half;
    const double captureHz = selected * 1000.0;
    const double visibleHz = 2.0 * std::max(center - lowHz, highHz - center) * 1.02;
    // The shared pan crops 4% on each side. Account for it exactly once,
    // leaving 1% of the band width outside each red boundary where possible.
    const double spanHz = whole ? std::min(captureHz, visibleHz / 0.92) : captureHz;
    return ReceiveBandView{center, selected, whole, spanHz};
}

// Resample the central display window without changing the raw IQ or audio.
// Bin n/2 remains DC; output preserves bin count and absolute dB levels.
[[nodiscard]] inline std::vector<float> bandViewSpectrum(std::span<const float> bins, double fraction)
{
    if (bins.size() < 2 || !std::isfinite(fraction) || fraction <= 0 || fraction >= 1) {
        return {bins.begin(), bins.end()};
    }
    std::vector<float> result(bins.size());
    const double middle = static_cast<double>(bins.size()) * 0.5;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const double source = std::clamp(middle + (static_cast<double>(i) - middle) * fraction,
                                        0.0, static_cast<double>(bins.size() - 1));
        const auto left = static_cast<std::size_t>(source);
        const auto right = std::min(left + 1, bins.size() - 1);
        const double weight = source - static_cast<double>(left);
        result[i] = static_cast<float>(bins[left] * (1.0 - weight) + bins[right] * weight);
    }
    return result;
}

struct AnanModelInfo {
    AnanModel model;
    std::string_view name;
    int nativeBoardId;
};
inline constexpr std::array<AnanModelInfo, 18> kAnanModels{{
    {AnanModel::Automatic, "Automatic physical-board profile", -1},
    {AnanModel::Hpsdr, "HPSDR / Atlas / Metis", 0},
    {AnanModel::Hermes, "Hermes", 1},
    {AnanModel::Anan10, "ANAN-10", 1},
    {AnanModel::Anan10E, "ANAN-10E", 2},
    {AnanModel::Anan100, "ANAN-100", 1},
    {AnanModel::Anan100B, "ANAN-100B", 2},
    {AnanModel::Anan100D, "ANAN-100D", 3},
    {AnanModel::Anan200D, "ANAN-200D", 4},
    {AnanModel::OrionMkII, "Orion MkII", 5},
    {AnanModel::Anan7000, "ANAN-7000DLE", 5},
    {AnanModel::Anan8000, "ANAN-8000DLE", 5},
    {AnanModel::AnanG2, "ANAN-G2", 10},
    {AnanModel::AnanG21K, "ANAN-G2 1K", 10},
    {AnanModel::AnvelinaPro3, "Anvelina Pro 3", 5},
    {AnanModel::HermesLite2, "Hermes-Lite 2", 6},
    {AnanModel::RedPitaya, "Red Pitaya", 5},
    {AnanModel::AnanG2E, "ANAN-G2E", 20}
}};
[[nodiscard]] constexpr const AnanModelInfo* ananModelInfo(AnanModel model) noexcept
{
    for (const auto& entry : kAnanModels) {
        if (entry.model == model) { return &entry; }
    }
    return nullptr;
}
[[nodiscard]] constexpr bool modelMatchesBoard(AnanModel model, int board) noexcept
{
    const auto* info = ananModelInfo(model);
    if (!info) { return false; }
    if (model == AnanModel::Automatic) { return true; }
    if (model == AnanModel::RedPitaya) { return board == 1 || board == 5; }
    if (model == AnanModel::Anan10 || model == AnanModel::Anan10E
        || model == AnanModel::Anan100 || model == AnanModel::Anan100B) {
        return board == 1 || board == 2;
    }
    return info->nativeBoardId == board;
}
//DH1KLM: Thetis setup.cs RXAntChk1Name/RXAntChk2Name. Generic Orion MkII
// uses SetAntBits' EXT wire names; an explicit DIY mapping uses its model.
[[nodiscard]] constexpr std::array<std::string_view, 4> receiveInputNames(AnanModel model, int board) noexcept
{
    if (model == AnanModel::Automatic) {
        model = board == 1 || board == 2 ? AnanModel::Hermes
            : board == 3 || board == 4 ? AnanModel::Anan200D : AnanModel::Anan7000;
    }
    if (model == AnanModel::Hermes || model == AnanModel::Anan10 || model == AnanModel::Anan10E) {
        return {"ANT1/2/3", "RX1", "RX2", "XVTR RX"};
    }
    if (model == AnanModel::Anan100 || model == AnanModel::Anan100B
        || model == AnanModel::Anan100D || model == AnanModel::Anan200D
        || model == AnanModel::OrionMkII) {
        return {"ANT1/2/3", "EXT2", "EXT1", "XVTR RX"};
    }
    return {"ANT1/2/3", "BYPASS", "EXT1", "XVTR RX"};
}
[[nodiscard]] constexpr int maximumP1RateKsps(AnanModel model) noexcept
{
    return model == AnanModel::RedPitaya || model == AnanModel::HermesLite2 ? 384 : 192;
}
struct ReceiveAntennaSelection {
    std::array<std::string_view, 3> ports{};
    std::size_t count = 0;
    std::string_view current{};
};
//DH1KLM: Generic RX antenna controls represent the active source, not an inactive ADC0 bank.
[[nodiscard]] constexpr ReceiveAntennaSelection receiveAntennaSelection(
    int numAdcs, int adc, int input, int antenna, const std::array<std::string_view, 4>& names) noexcept
{
    if (numAdcs < 1 || numAdcs > 2 || adc < 0 || adc >= numAdcs
        || input < 0 || input > 3 || antenna < 1 || antenna > 3) { return {}; }
    if (adc == 1) { return {{{"RX2", {}, {}}}, 1, "RX2"}; }
    if (input != 0) { return {{{names[input], {}, {}}}, 1, names[input]}; }
    constexpr std::array<std::string_view, 3> bank{"ANT1", "ANT2", "ANT3"};
    return {bank, bank.size(), bank[antenna - 1]};
}
} // namespace AetherSDR::anan
