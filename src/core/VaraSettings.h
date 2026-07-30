#pragma once

#include "core/VaraProtocol.h"

#include <QString>

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the VARA HF modem page, per Constitution Principle V:
// one nested JSON object under a single root key ("Vara"), read and written
// atomically. There are no legacy flat keys to migrate — this feature has never
// shipped — so unlike AutomationBridgeSettings there is no migration path here.
//
// WHAT IS AND IS NOT CONFIGURABLE, AND WHY
//
//   host / commandPort  The modem's TCP host interface. The DATA port is always
//                       the command port plus one; that is the modem's own
//                       convention, not a free choice, so it is derived rather
//                       than stored. A second modem instance on one machine
//                       conventionally uses 8302/8303.
//
//   callsign            Registered with the modem via MYCALL before any link.
//
//   bandwidthHz         500, 2300 or 2750. **This must match the receive filter
//                       width.** A mismatch does not produce an error — the
//                       link simply never completes — which cost real bench
//                       time to diagnose, so enforceFilterWidth defaults to on
//                       and the page is expected to drive the slice filter from
//                       this value.
//
//   compression         VARA can compress text and files. Off is the default
//                       because compression transforms the payload before the
//                       modem's own coding, which matters for anything
//                       inspecting the wire.
//
//   launchModem/path    Only meaningful where AetherSDR hosts an external
//                       VARA.exe. A native implementation ignores both.
//
// DELIBERATELY ABSENT: any encryption setting. VARA can negotiate an encrypted
// link, but 47 CFR §97.113(a)(4) prohibits messages encoded to obscure their
// meaning on amateur allocations, so AetherSDR parses the state and never asks
// for it (see VaraProtocol.h). Offering a switch would invite operators to
// break the rules.
class VaraSettings {
public:
    static constexpr int kDefaultCommandPort = 8300;
    // Below 1024 needs root on Linux and macOS; the connection would fail in a
    // way that looks like the modem simply not being there.
    static constexpr int kMinPort = 1024;
    static constexpr int kMaxPort = 65535;
    static constexpr int kDefaultBandwidthHz = 2300;

    static bool enabled();
    static void setEnabled(bool on);

    static QString host();                       // default "127.0.0.1"
    static void setHost(const QString& host);

    static int commandPort();                    // default kDefaultCommandPort
    static void setCommandPort(int port);
    // The modem's convention, not a setting: data port = command port + 1.
    static int dataPort() { return commandPort() + 1; }

    static QString callsign();
    static void setCallsign(const QString& call);

    static Vara::Bandwidth bandwidth();          // default Bw2300
    static void setBandwidth(Vara::Bandwidth bw);
    static int bandwidthHz();

    static Vara::Compression compression();      // default Off
    static void setCompression(Vara::Compression c);

    // Drive the slice filter from bandwidth() when a session starts. On by
    // default because a mismatch silently prevents any link.
    static bool enforceFilterWidth();            // default true
    static void setEnforceFilterWidth(bool on);

    // External-modem hosting. Ignored by a native implementation.
    static bool launchModem();                   // default false
    static void setLaunchModem(bool on);
    static QString modemPath();
    static void setModemPath(const QString& path);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
