#pragma once

#include <array>
#include <string_view>

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
[[nodiscard]] constexpr int maximumP1RateKsps(AnanModel model) noexcept
{
    return model == AnanModel::RedPitaya || model == AnanModel::HermesLite2 ? 384 : 192;
}
} // namespace AetherSDR::anan
