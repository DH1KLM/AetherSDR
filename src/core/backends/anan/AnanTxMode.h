#pragma once

#include "core/dsp/WdspChannel.h"
#include <optional>

namespace AetherSDR::anan {
struct TxMode {
    WdspChannel::Mode modulator;
    double filterLowHz;
    double filterHighHz;
    double occupiedLowHz;
    double occupiedHighHz;
    bool cw = false;
};

inline std::optional<TxMode> transmitMode(WdspChannel::Mode mode) noexcept
{
    using Mode = WdspChannel::Mode;
    switch (mode) {
    case Mode::Usb:
        return TxMode{Mode::Usb,300,3000,300,3000};
    case Mode::Lsb:
        return TxMode{Mode::Lsb,-3000,-300,-3000,-300};
    case Mode::Digu:
        return TxMode{Mode::Usb,150,3000,150,3000};
    case Mode::Digl:
        return TxMode{Mode::Lsb,-3000,-150,-3000,-150};
    case Mode::Dsb:
        return TxMode{Mode::Dsb,-3000,3000,-3000,3000};
    case Mode::Am: case Mode::Sam:
        return TxMode{Mode::Am,-3000,3000,-3000,3000};
    case Mode::Fm:
        return TxMode{Mode::Fm,-3000,3000,-8000,8000};
    case Mode::Cwl: case Mode::Cwu:
        return TxMode{Mode::Usb,300,3000,-250,250,true};
    case Mode::Drm: case Mode::Spec: case Mode::Wbfm:
        return {};
    }
    return {};
}
} // namespace AetherSDR::anan
