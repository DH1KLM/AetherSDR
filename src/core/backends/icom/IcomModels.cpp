#include "core/backends/icom/IcomModels.h"

#include <array>

namespace AetherSDR::icom {
namespace {

// The table.
//
// Only the IC-705 row is `verified`. Its numbers come from Icom's own CI-V
// Reference Guide: 475 waveform points, data range 0..160, division max 1 over
// WLAN and 11 over USB, one receiver, one scope (0x27 0x12 and 0x27 0x13 are
// both fixed at 00), 10 W out.
//
// The rest are cross-referenced hardware facts and are marked unverified. Each
// one needs its own model's CI-V guide read before this backend advertises
// support for it — the shape of the transport is shared, the command table and
// scope geometry are not.
constexpr std::array<IcomModel, 6> kModels{{
    {
        /*civAddress*/ 0xA4, /*name*/ "IC-705",
        /*receivers*/ 1, /*vfos*/ 2,
        /*hasNetwork*/ true, /*hasWifi*/ true,
        /*hasScope*/ true, /*scopePoints*/ 475, /*scopeMaxAmplitude*/ 160,
        /*scopeDivisionsUsb*/ 11,
        /*freqBytes*/ kFreqBytes,
        /*hasTransmit*/ true, /*txPowerMaxWatts*/ 10.0,
        /*tuningMinHz*/ 30'000ULL, /*tuningMaxHz*/ 470'000'000ULL,
        /*verified*/ true,
    },
    {
        0xA2, "IC-9700", 2, 2,
        /*hasNetwork*/ true, /*hasWifi*/ false,
        /*hasScope*/ true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        144'000'000ULL, 1'300'000'000ULL,
        /*verified*/ false,
    },
    {
        0x98, "IC-7610", 2, 1,
        /*hasNetwork*/ true, /*hasWifi*/ false,
        // 689 points and a 0..200 range — BOTH differ from the IC-705, which is
        // exactly why the scope geometry cannot be a compile-time constant.
        /*hasScope*/ true, 689, 200, 15,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 60'000'000ULL,
        /*verified*/ false,
    },
    {
        0x8E, "IC-785x", 2, 1,
        true, false,
        true, 689, 200, 15,
        kFreqBytes,
        true, 200.0,
        30'000ULL, 60'000'000ULL,
        false,
    },
    {
        // NO NETWORK. Reachable only over a local serial port, or over the
        // network through Icom's own RS-BA1 server acting as a front end — in
        // which case this same backend reaches it without any extra work.
        0x94, "IC-7300", 1, 2,
        /*hasNetwork*/ false, /*hasWifi*/ false,
        true, 475, 160, 11,
        kFreqBytes,
        true, 100.0,
        30'000ULL, 74'800'000ULL,
        false,
    },
    {
        // SIX-BYTE FREQUENCIES above 10 GHz. A codec written against a
        // hardcoded 5 misaligns by two bytes and decodes a plausible-looking
        // wrong frequency — which on transmit is an out-of-band emission.
        0xAC, "IC-905", 1, 2,
        true, false,
        true, 475, 160, 11,
        /*freqBytes*/ 6,
        true, 10.0,
        144'000'000ULL, 10'500'000'000ULL,
        false,
    },
}};

// Conservative fallback for an unrecognised address. No scope, no transmit.
constexpr IcomModel kUnknown{
    /*civAddress*/ 0x00, /*name*/ "Unknown Icom",
    /*receivers*/ 1, /*vfos*/ 2,
    /*hasNetwork*/ true, /*hasWifi*/ false,
    /*hasScope*/ false, /*scopePoints*/ 0, /*scopeMaxAmplitude*/ 0,
    /*scopeDivisionsUsb*/ 11,
    /*freqBytes*/ kFreqBytes,
    /*hasTransmit*/ false, /*txPowerMaxWatts*/ 0.0,
    /*tuningMinHz*/ 0, /*tuningMaxHz*/ 0,
    /*verified*/ false,
};

}  // namespace

const IcomModel* modelForCivAddress(std::uint8_t addr)
{
    for (const auto& m : kModels)
        if (m.civAddress == addr)
            return &m;
    return nullptr;
}

std::span<const IcomModel> knownModels() { return kModels; }

const IcomModel& unknownModel() { return kUnknown; }

std::optional<std::uint8_t> parseModelIdReply(const CivFrame& frame)
{
    if (frame.cmd != cmd::kReadId || !frame.hasSub || frame.sub != 0x00)
        return std::nullopt;
    if (frame.data.empty())
        return std::nullopt;
    return frame.data[0];
}

std::span<const CurvePoint> powerCurveFor(const IcomModel& model)
{
    // Only the IC-705 has a measured curve. Every other model returns EMPTY so
    // the caller reports percent — handing back the IC-705's curve for an
    // IC-9700 would produce a watts figure an operator would act on, derived
    // from a different radio's PA.
    if (model.civAddress == 0xA4)
        return powerCurveIc705();
    return {};
}

double s9ReferenceFor(std::uint64_t hz) noexcept
{
    // BAND-dependent, not model-dependent. IARU Region 1: S9 is -73 dBm below
    // 30 MHz and -93 dBm above. Using -73 everywhere reports VHF signals 20 dB
    // hot, which on a 2 m weak-signal band is the entire usable range.
    return usesVhfSReference(hz) ? kS9DbmVhf : kS9DbmHf;
}

}  // namespace AetherSDR::icom
