#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

enum class DaxTxRequestReason {
    HostedDaxBridge,
    TciTxAudio,
    RadeModemTx,
    AetherModemAx25Tx,
    AetherHfModemTx,
    WsprBeacon,
    ExternalDaxRouteOnly,
    GenericAudioRecreate
};

enum class DaxTxPlatform {
    Windows,
    MacOS,
    Linux,
    Other
};

enum class DaxTxMode {
    None,
    HostedDax,
    ExternalDax2
};

struct DaxTxPolicyContext {
    DaxTxRequestReason reason{DaxTxRequestReason::GenericAudioRecreate};
    DaxTxPlatform platform{DaxTxPlatform::Other};
    DaxTxMode mode{DaxTxMode::None};
    bool hostedDaxAvailable{false};
    bool tciDaxTxSupported{false};
};

struct DaxTxPolicyDecision {
    bool allowed{false};
    QString note;
};

inline QString daxTxRequestReasonName(DaxTxRequestReason reason)
{
    switch (reason) {
    case DaxTxRequestReason::HostedDaxBridge:     return QStringLiteral("hosted_dax_bridge");
    case DaxTxRequestReason::TciTxAudio:          return QStringLiteral("tci_tx_audio");
    case DaxTxRequestReason::RadeModemTx:         return QStringLiteral("rade_modem_tx");
    case DaxTxRequestReason::AetherModemAx25Tx:   return QStringLiteral("aethermodem_ax25_tx");
    case DaxTxRequestReason::AetherHfModemTx:      return QStringLiteral("aetherhf_modem_tx");
    case DaxTxRequestReason::WsprBeacon:           return QStringLiteral("wspr_beacon");
    case DaxTxRequestReason::ExternalDaxRouteOnly:return QStringLiteral("external_dax_route_only");
    case DaxTxRequestReason::GenericAudioRecreate:return QStringLiteral("generic_audio_recreate");
    }
    return QStringLiteral("unknown");
}

inline QString daxTxPlatformName(DaxTxPlatform platform)
{
    switch (platform) {
    case DaxTxPlatform::Windows: return QStringLiteral("windows");
    case DaxTxPlatform::MacOS:   return QStringLiteral("macos");
    case DaxTxPlatform::Linux:   return QStringLiteral("linux");
    case DaxTxPlatform::Other:   return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

inline QString daxTxModeName(DaxTxMode mode)
{
    switch (mode) {
    case DaxTxMode::None:         return QStringLiteral("none");
    case DaxTxMode::HostedDax:    return QStringLiteral("hosted_dax");
    case DaxTxMode::ExternalDax2: return QStringLiteral("external_dax2");
    }
    return QStringLiteral("none");
}

inline DaxTxPlatform currentDaxTxPlatform()
{
#if defined(Q_OS_WIN)
    return DaxTxPlatform::Windows;
#elif defined(Q_OS_MAC)
    return DaxTxPlatform::MacOS;
#elif defined(Q_OS_LINUX)
    return DaxTxPlatform::Linux;
#else
    return DaxTxPlatform::Other;
#endif
}

inline bool currentBuildHasHostedDax()
{
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    return true;
#else
    return false;
#endif
}

inline DaxTxMode currentDaxTxMode()
{
#if defined(Q_OS_WIN)
    return DaxTxMode::ExternalDax2;
#elif defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    return DaxTxMode::HostedDax;
#else
    return DaxTxMode::None;
#endif
}

inline DaxTxPolicyContext currentDaxTxPolicyContext(DaxTxRequestReason reason)
{
    const bool hostedDax = currentBuildHasHostedDax();
    return DaxTxPolicyContext{
        reason,
        currentDaxTxPlatform(),
        currentDaxTxMode(),
        hostedDax,
        hostedDax
    };
}

inline DaxTxPolicyDecision evaluateDaxTxPolicy(const DaxTxPolicyContext& context)
{
    switch (context.reason) {
    case DaxTxRequestReason::HostedDaxBridge:
        if (context.mode == DaxTxMode::HostedDax && context.hostedDaxAvailable) {
            return {true, QStringLiteral("hosted_dax_available")};
        }
        if (context.mode == DaxTxMode::ExternalDax2) {
            return {false, QStringLiteral("SmartSDR_DAX2_owns_windows_dax")};
        }
        return {false, QStringLiteral("hosted_dax_unavailable")};

    case DaxTxRequestReason::TciTxAudio:
        // TCI's audio source is the WebSocket from WSJT-X / JTDX / MSHV, not
        // a local audio device.  AetherSDR feeds those packets into a
        // dedicated dax_tx stream that's independent of SmartSDR DAX2 (which
        // owns the Windows DAX *audio devices*, not the radio's dax_tx
        // stream slot — multiple GUI clients can each register their own).
        // Always allow regardless of platform / hosted-DAX availability. (#2276)
        return {true, QStringLiteral("tci_creates_own_dax_tx_stream")};

    case DaxTxRequestReason::RadeModemTx:
        // RADE encodes the mic waveform itself and sends VITA-49 packets
        // directly via sendModemTxAudio() — exactly like TCI, it never
        // touches Windows audio devices.  SmartSDR DAX2 owning the audio
        // device layer is irrelevant: the radio's dax_tx stream slot is
        // independent and AetherSDR must register its own.
        return {true, QStringLiteral("rade_sends_vita49_directly")};

    case DaxTxRequestReason::AetherModemAx25Tx:
        // AetherModem generates an AX.25 AFSK waveform internally and sends
        // VITA-49 packets directly via AudioEngine::sendModemTxAudio(). Like
        // RADE/TCI, it does not claim a Windows audio device, so it needs its
        // own dax_tx stream even on platforms where external DAX owns devices.
        return {true, QStringLiteral("aethermodem_sends_vita49_directly")};

    case DaxTxRequestReason::AetherHfModemTx:
        // AetherModem's AetherHF page synthesises the HF waveform itself —
        // 16-ary orthogonal CPFSK, 512-sample symbols — and sends VITA-49
        // packets through AudioEngine::sendModemTxAudio(), exactly as AX.25,
        // RADE and WSPR do. It claims no OS audio device, so SmartSDR DAX2
        // owning the Windows device layer is irrelevant and it needs its own
        // client-owned dax_tx stream on every platform.
        //
        // This is the NATIVE path. Hosting an external VARA.exe is a different
        // case entirely: that program does claim an OS audio device, which
        // makes it a HostedDaxBridge on Linux and macOS, and on Windows means
        // AetherSDR should create no stream at all because DAX2 owns the device
        // and the modem selects it directly.
        return {true, QStringLiteral("aetherhf_sends_vita49_directly")};

    case DaxTxRequestReason::WsprBeacon:
        // The built-in beacon generates its waveform and VITA-49 payload
        // directly. It does not claim an OS DAX audio device, so it needs its
        // own client-owned dax_tx stream on every platform.
        return {true, QStringLiteral("wspr_sends_vita49_directly")};

    case DaxTxRequestReason::ExternalDaxRouteOnly:
        if (context.mode == DaxTxMode::ExternalDax2) {
            return {false, QStringLiteral("SmartSDR_DAX2_owns_windows_dax")};
        }
        return {false, QStringLiteral("route_only_does_not_require_local_stream")};

    case DaxTxRequestReason::GenericAudioRecreate:
        return {false, QStringLiteral("generic_audio_recreate_not_dax_tx")};
    }

    return {false, QStringLiteral("unknown_reason")};
}

} // namespace AetherSDR
