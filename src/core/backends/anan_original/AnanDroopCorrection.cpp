#include "core/backends/anan_original/AnanDroopCorrection.h"

namespace AetherSDR::anan_original {

namespace {
constexpr DroopCorrectionTable kZeroTable{};
}  // namespace

const DroopCorrectionTable& kDroopCorrectionZero = kZeroTable;

}  // namespace AetherSDR::anan_original
