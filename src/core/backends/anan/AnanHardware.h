#pragma once

#include <QString>
#include <QStringList>
#include <QMetaType>

#include <cstdint>
#include <optional>

namespace AetherSDR::anan {

//DH1KLM: Stage 1 introduces an explicit OpenHPSDR hardware identity layer.
//DH1KLM: The board identity is deliberately independent from the radio SKU and
//DH1KLM: from the transport protocol. This is required because the same physical
//DH1KLM: OpenHPSDR board can be associated with more than one radio model and,
//DH1KLM: for the ANAN-200D target, both Protocol 1 and Protocol 2 are valid.

//DH1KLM: OpenHPSDR hardware board identifiers used by the Protocol 1/2
//DH1KLM: discovery layers. Only Orion is enabled by the Stage 1 profile below;
//DH1KLM: the remaining entries are reserved for the later all-board expansion.
enum class AnanBoard : std::uint8_t {
    Unknown = 0,
    Atlas = 1,
    Hermes = 2,
    HermesII = 3,
    Angelia = 4,
    Orion = 5,
    OrionMKII = 6,
    HermesLite = 7,
    Saturn = 10,
    SaturnMKII = 11
};

//DH1KLM: Protocol capability is modeled separately from AnanBoard.
//DH1KLM: This prevents a board type from becoming an implicit protocol selector.
enum class AnanProtocol : std::uint8_t {
    Protocol1 = 1,
    Protocol2 = 2
};

//DH1KLM: Logical radio models are intentionally separate from the FPGA board.
enum class AnanRadioModel : std::uint8_t {
    Unknown = 0,
    ANAN200D
};

//DH1KLM: Stage 1 connection identity. A discovered radio carries both its
//DH1KLM: physical board and the protocol through which it was discovered.
struct AnanIdentity {
    AnanRadioModel model = AnanRadioModel::Unknown;
    AnanBoard board = AnanBoard::Unknown;
    AnanProtocol protocol = AnanProtocol::Protocol1;
    QString displayName;
    QString boardName;
};

//DH1KLM: Stage 1 capability record. The fields are intentionally conservative.
//DH1KLM: They describe what the ANAN-200D/Orion profile needs for discovery and
//DH1KLM: connection selection; they do not yet claim TX support in AetherSDR.
struct AnanCapabilities {
    AnanRadioModel model = AnanRadioModel::Unknown;
    AnanBoard board = AnanBoard::Unknown;

    bool protocol1 = false;
    bool protocol2 = false;

    int adcCount = 0;
    int maxReceivers = 0;

    QStringList protocol1SampleRatesHz;
    QStringList protocol2SampleRatesHz;
};

//DH1KLM: Returns the canonical human-readable board name.
inline QString ananBoardName(AnanBoard board)
{
    switch (board) {
    case AnanBoard::Atlas:       return QStringLiteral("Atlas");
    case AnanBoard::Hermes:      return QStringLiteral("Hermes");
    case AnanBoard::HermesII:    return QStringLiteral("Hermes II");
    case AnanBoard::Angelia:     return QStringLiteral("Angelia");
    case AnanBoard::Orion:       return QStringLiteral("Orion");
    case AnanBoard::OrionMKII:   return QStringLiteral("Orion MKII");
    case AnanBoard::HermesLite:  return QStringLiteral("Hermes Lite");
    case AnanBoard::Saturn:      return QStringLiteral("Saturn");
    case AnanBoard::SaturnMKII:  return QStringLiteral("Saturn MKII");
    case AnanBoard::Unknown:
    default:                     return QStringLiteral("Unknown");
    }
}

//DH1KLM: Returns the canonical AetherSDR model name used by the picker.
inline QString ananModelName(AnanRadioModel model)
{
    switch (model) {
    case AnanRadioModel::ANAN200D:
        return QStringLiteral("ANAN-200D");
    case AnanRadioModel::Unknown:
    default:
        return QStringLiteral("Unknown ANAN");
    }
}

//DH1KLM: Stage 1 profile for ANAN-200D on the Orion hardware platform.
//DH1KLM: The profile explicitly enables BOTH OpenHPSDR Protocol 1 and Protocol 2.
//DH1KLM: This is the key architectural correction over the existing ANAN-G2-only
//DH1KLM: implementation, where Saturn was effectively treated as the only P2 board.
inline AnanCapabilities anan200dCapabilities()
{
    AnanCapabilities caps;
    caps.model = AnanRadioModel::ANAN200D;
    caps.board = AnanBoard::Orion;

    //DH1KLM: ANAN-200D is supported through both protocol families in Stage 1.
    caps.protocol1 = true;
    caps.protocol2 = true;

    //DH1KLM: Orion provides two ADC paths and up to seven tunable receivers
    //DH1KLM: in the OpenHPSDR Protocol 1 receiver register space.
    caps.adcCount = 2;
    caps.maxReceivers = 7;

    //DH1KLM: Protocol 1 rate list for the Stage 1 ANAN-200D profile is kept
    //DH1KLM: deliberately conservative until the Orion P1 wire path is ported
    //DH1KLM: from Thetis/NereusSDR and verified against real hardware.
    caps.protocol1SampleRatesHz = {
        QStringLiteral("48000"),
        QStringLiteral("96000"),
        QStringLiteral("192000"),
        QStringLiteral("384000")
    };

    //DH1KLM: Protocol 2 rates currently implemented by AetherSDR's P2 layer.
    //DH1KLM: The existing P2 implementation exposes these six rates.
    caps.protocol2SampleRatesHz = {
        QStringLiteral("48000"),
        QStringLiteral("96000"),
        QStringLiteral("192000"),
        QStringLiteral("384000"),
        QStringLiteral("768000"),
        QStringLiteral("1536000")
    };

    return caps;
}

//DH1KLM: Converts the Stage 1 ANAN-200D profile into the discovery identity.
inline AnanIdentity anan200dIdentity(AnanProtocol protocol)
{
    AnanIdentity identity;
    identity.model = AnanRadioModel::ANAN200D;
    identity.board = AnanBoard::Orion;
    identity.protocol = protocol;
    identity.displayName = ananModelName(identity.model);
    identity.boardName = ananBoardName(identity.board);
    return identity;
}

//DH1KLM: Maps a Protocol 1 hardware identifier to the known board enum.
//DH1KLM: The numeric values are isolated here so discovery parsing does not
//DH1KLM: spread magic board IDs throughout AnanDiscovery and P1Client.
inline AnanBoard ananBoardFromProtocol1Id(std::uint8_t id)
{
    switch (id) {
    case 1:  return AnanBoard::Atlas;
    case 2:  return AnanBoard::Hermes;
    case 3:  return AnanBoard::HermesII;
    case 4:  return AnanBoard::Angelia;
    case 5:  return AnanBoard::Orion;
    case 6:  return AnanBoard::OrionMKII;
    case 10: return AnanBoard::Saturn;
    case 11: return AnanBoard::SaturnMKII;
    default: return AnanBoard::Unknown;
    }
}

//DH1KLM: Maps a Protocol 2 discovery board identifier to the board enum.
//DH1KLM: P2 board IDs are intentionally kept separate from P1 IDs because the
//DH1KLM: wire formats do not share one universal discovery identifier namespace.
inline AnanBoard ananBoardFromProtocol2Id(std::uint8_t id)
{
    switch (id) {
    case 10: return AnanBoard::Saturn;
    case 11: return AnanBoard::SaturnMKII;
    default: return AnanBoard::Unknown;
    }
}

//DH1KLM: Stage 1 model resolution. Orion is mapped to ANAN-200D only while the
//DH1KLM: implementation is intentionally limited to the requested first milestone.
inline std::optional<AnanRadioModel> ananModelForBoard(AnanBoard board)
{
    if (board == AnanBoard::Orion)
        return AnanRadioModel::ANAN200D;

    return std::nullopt;
}

//DH1KLM: Prevent accidental protocol selection from a board-only lookup.
//DH1KLM: The caller must choose the protocol from the discovery result.
inline bool ananSupportsProtocol(const AnanCapabilities& caps, AnanProtocol protocol)
{
    if (protocol == AnanProtocol::Protocol1)
        return caps.protocol1;

    if (protocol == AnanProtocol::Protocol2)
        return caps.protocol2;

    return false;
}

} // namespace AetherSDR::anan

//DH1KLM: Register the enum types for Qt queued signal/slot transport used by
//DH1KLM: the forthcoming protocol-aware ANAN discovery implementation.
Q_DECLARE_METATYPE(AetherSDR::anan::AnanBoard)
Q_DECLARE_METATYPE(AetherSDR::anan::AnanProtocol)
Q_DECLARE_METATYPE(AetherSDR::anan::AnanRadioModel)
Q_DECLARE_METATYPE(AetherSDR::anan::AnanIdentity)
