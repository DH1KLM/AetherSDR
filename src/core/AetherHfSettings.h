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
    // MercuryV2 is not integrated yet. Only the settings surface exists, so the
    // selector has somewhere to store a choice; the page reports the engine as
    // unavailable rather than pretending otherwise.
    static bool mercuryAvailable();              // false until it is integrated
    static QString mercuryConfigPath();
    static void setMercuryConfigPath(const QString& path);

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
