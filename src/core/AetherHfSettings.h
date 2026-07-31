#pragma once

#include "core/VaraProtocol.h"

#include <QString>

class QJsonObject;

namespace AetherSDR {

// Which HF data engine AetherHF drives.
//
// The engine is a choice inside AetherHF, not a separate feature: an operator
// picks one within the page rather than choosing between differently named
// tabs. Adding a third means adding an enumerator and a settings section, not
// another tab.
enum class HfEngine {
    Vara,      // the VARA HF modem, over its TCP host interface
    Mercury,   // MercuryV2
};

QString hfEngineToString(HfEngine engine);
HfEngine hfEngineFromString(const QString& text);
QString hfEngineDisplayName(HfEngine engine);

// Owned configuration for AetherHF, per Constitution Principle V: one nested
// JSON object under a single root key ("AetherHf").
//
// SHAPE. Settings common to every engine sit at the top of that object;
// engine-specific settings live in a named sub-object, so adding an engine
// cannot collide with an existing one and removing one leaves no orphan keys:
//
//   {
//     "engine": "vara",
//     "callsign": "KK7GWY",
//     "enforceFilterWidth": "True",
//     "vara":    { "host": ..., "commandPort": ..., "bandwidthHz": ..., ... },
//     "mercury": { ... }
//   }
//
// The callsign is shared deliberately. It identifies the station, not the
// modem, and an operator who switches engines has not changed who they are.
//
// DELIBERATELY ABSENT: any encryption setting. VARA can negotiate an encrypted
// link, but 47 CFR §97.113(a)(4) prohibits messages encoded to obscure their
// meaning on amateur allocations, so AetherSDR parses the state and never asks
// for it. Offering a switch would invite operators to break the rules.
class AetherHfSettings {
public:
    static constexpr int kDefaultVaraCommandPort = 8300;
    // Mercury's own default base port is also 8300, but defaulting both engines
    // to it would mean the two collide the moment someone runs them together on
    // one machine — which is exactly what comparing them requires. 8400 keeps
    // that case working out of the box; Mercury takes the base port with -p.
    static constexpr int kDefaultMercuryCommandPort = 8400;
    // Below 1024 needs root on Linux and macOS; the connection would fail in a
    // way that looks like the modem simply not being there.
    static constexpr int kMinPort = 1024;
    static constexpr int kMaxPort = 65535;
    static constexpr int kDefaultBandwidthHz = 2300;

    // ── Common ──────────────────────────────────────────────────────────
    static bool enabled();
    static void setEnabled(bool on);

    static HfEngine engine();                    // default Vara
    static void setEngine(HfEngine engine);

    static QString callsign();                   // shared across engines
    static void setCallsign(const QString& call);

    // Drive the slice filter from the engine's bandwidth when a session starts.
    // On by default because a filter narrower than the negotiated bandwidth
    // prevents a link from forming and nothing reports why.
    static bool enforceFilterWidth();            // default true
    static void setEnforceFilterWidth(bool on);

    // ── VARA engine ─────────────────────────────────────────────────────
    static QString varaHost();                   // default "127.0.0.1"
    static void setVaraHost(const QString& host);

    static int varaCommandPort();                // default kDefaultVaraCommandPort
    static void setVaraCommandPort(int port);
    // The modem's convention, not a setting: data port = command port + 1.
    static int varaDataPort() { return varaCommandPort() + 1; }

    static Vara::Bandwidth varaBandwidth();      // default Bw2300
    static void setVaraBandwidth(Vara::Bandwidth bw);
    static int varaBandwidthHz();

    static Vara::Compression varaCompression();  // default Off
    static void setVaraCompression(Vara::Compression c);

    // External-modem hosting. A native implementation ignores both.
    static bool varaLaunchModem();               // default false
    static void setVaraLaunchModem(bool on);
    static QString varaModemPath();
    static void setVaraModemPath(const QString& path);

    // ── Mercury engine ──────────────────────────────────────────────────
    // MercuryV2 (Rhizomatica, GPL-3.0) is a separate process that speaks the
    // same VARA-compatible TCP host protocol on the same port layout — control
    // on the base port, data on base+1. So the same client drives it, and what
    // is Mercury-specific is where the binary lives, how it is launched, and the
    // few commands VARA has no equivalent for.
    static QString mercuryHost();                // default "127.0.0.1"
    static void setMercuryHost(const QString& host);

    static int mercuryCommandPort();             // default kDefaultMercuryCommandPort
    static void setMercuryCommandPort(int port);
    // Mercury's convention, matching VARA: data port = base port + 1.
    static int mercuryDataPort() { return mercuryCommandPort() + 1; }

    static Vara::Bandwidth mercuryBandwidth();   // default Bw2300
    static void setMercuryBandwidth(Vara::Bandwidth bw);
    static int mercuryBandwidthHz();

    // Launching is optional: an operator may already run Mercury themselves, or
    // run it on another machine entirely.
    static bool mercuryLaunch();                 // default false
    static void setMercuryLaunch(bool on);
    static QString mercuryBinaryPath();
    static void setMercuryBinaryPath(const QString& path);

    // Passed on Mercury's command line when this launches it. Mercury opens the
    // sound devices itself rather than being handed audio, so these are how it
    // gets pointed at the same virtual cables the rig is on.
    static QString mercurySoundSystem();         // default per platform
    static void setMercurySoundSystem(const QString& system);
    static QString mercuryCaptureDevice();
    static void setMercuryCaptureDevice(const QString& device);
    static QString mercuryPlaybackDevice();
    static void setMercuryPlaybackDevice(const QString& device);
    static int mercuryModeIndex();               // default 1 (DATAC3)
    static void setMercuryModeIndex(int index);

    // Mercury's own config file (-C). Optional: it covers settings this page
    // does not surface, so an operator with a tuned configuration keeps it.
    static QString mercuryConfigPath();
    static void setMercuryConfigPath(const QString& path);

    // Mercury-only. VARA has no equivalent: accept calls addressed to any
    // callsign rather than only our own.
    static bool mercuryPublic();                 // default false
    static void setMercuryPublic(bool on);

    // True once the engine can actually be started: either this launches the
    // binary and knows where it is, or the operator runs it and this only has
    // to reach a host.
    static bool mercuryAvailable();

    // What Mercury itself defaults to for the platform: alsa on Linux, dsound
    // on Windows, coreaudio on macOS.
    static QString defaultMercurySoundSystem();

    // ── The selected engine, without the caller branching ───────────────
    // The page drives whichever engine is chosen, and every one of these is a
    // property the connection needs. Branching on the engine at each call site
    // is how the two get to disagree.
    static QString activeHost();
    static int activeCommandPort();
    static int activeDataPort();
    static Vara::Bandwidth activeBandwidth();

    // The bandwidth the selected engine will use, for the filter comparison.
    // Returns 0 when the engine cannot report one.
    static int activeBandwidthHz();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static QJsonObject section(const QJsonObject& root, const char* name);
    static void writeSection(const char* name, const QJsonObject& sub);
};

} // namespace AetherSDR
