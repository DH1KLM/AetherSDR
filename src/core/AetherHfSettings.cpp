#include "core/AetherHfSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {
// Single nested-JSON key holding the whole AetherHF configuration (Principle V).
const QString kRootKey = QStringLiteral("AetherHf");

constexpr const char* kSectionVara    = "vara";
constexpr const char* kSectionMercury = "mercury";

constexpr const char* kFieldEnabled       = "enabled";
constexpr const char* kFieldEngine        = "engine";
constexpr const char* kFieldCallsign      = "callsign";
constexpr const char* kFieldEnforceFilter = "enforceFilterWidth";

constexpr const char* kVaraHost        = "host";
constexpr const char* kVaraCommandPort = "commandPort";
constexpr const char* kVaraBandwidthHz = "bandwidthHz";
constexpr const char* kVaraCompression = "compression";
constexpr const char* kVaraLaunchModem = "launchModem";
constexpr const char* kVaraModemPath   = "modemPath";

constexpr const char* kMercuryConfigPath = "configPath";

const QString kDefaultHost = QStringLiteral("127.0.0.1");

// Booleans are stored as "True"/"False" strings to match the shape the rest of
// AppSettings uses, so a settings file stays readable and consistent.
bool boolField(const QJsonObject& o, const char* field, bool fallback)
{
    const QString v = o.value(QLatin1String(field)).toString();
    if (v.isEmpty()) return fallback;
    return v == QLatin1String("True");
}

QString boolText(bool on)
{
    return on ? QStringLiteral("True") : QStringLiteral("False");
}

QString compressionToString(Vara::Compression c)
{
    switch (c) {
    case Vara::Compression::Text:  return QStringLiteral("text");
    case Vara::Compression::Files: return QStringLiteral("files");
    case Vara::Compression::Off:   break;
    }
    return QStringLiteral("off");
}

Vara::Compression compressionFromString(const QString& s)
{
    if (s == QLatin1String("text"))  return Vara::Compression::Text;
    if (s == QLatin1String("files")) return Vara::Compression::Files;
    return Vara::Compression::Off;
}
} // namespace

QString hfEngineToString(HfEngine engine)
{
    return engine == HfEngine::Mercury ? QStringLiteral("mercury")
                                       : QStringLiteral("vara");
}

HfEngine hfEngineFromString(const QString& text)
{
    return text == QLatin1String("mercury") ? HfEngine::Mercury : HfEngine::Vara;
}

QString hfEngineDisplayName(HfEngine engine)
{
    // Shown in the engine selector. The engine names are product names and are
    // not translated.
    return engine == HfEngine::Mercury ? QStringLiteral("Mercury")
                                       : QStringLiteral("VARA");
}

QJsonObject AetherHfSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty()) return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AetherHfSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

QJsonObject AetherHfSettings::section(const QJsonObject& root, const char* name)
{
    return root.value(QLatin1String(name)).toObject();
}

void AetherHfSettings::writeSection(const char* name, const QJsonObject& sub)
{
    QJsonObject root = readObj();
    root[QLatin1String(name)] = sub;
    write(root);
}

// ── Common ──────────────────────────────────────────────────────────────

bool AetherHfSettings::enabled()
{
    return boolField(readObj(), kFieldEnabled, false);
}

void AetherHfSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldEnabled)] = boolText(on);
    write(o);
}

HfEngine AetherHfSettings::engine()
{
    return hfEngineFromString(
        readObj().value(QLatin1String(kFieldEngine)).toString());
}

void AetherHfSettings::setEngine(HfEngine engine)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldEngine)] = hfEngineToString(engine);
    write(o);
}

QString AetherHfSettings::callsign()
{
    return readObj().value(QLatin1String(kFieldCallsign)).toString();
}

void AetherHfSettings::setCallsign(const QString& call)
{
    QJsonObject o = readObj();
    // The modem expects an upper-case callsign; normalising here means the page
    // and any headless caller cannot disagree about it.
    o[QLatin1String(kFieldCallsign)] = call.trimmed().toUpper();
    write(o);
}

bool AetherHfSettings::enforceFilterWidth()
{
    return boolField(readObj(), kFieldEnforceFilter, true);
}

void AetherHfSettings::setEnforceFilterWidth(bool on)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldEnforceFilter)] = boolText(on);
    write(o);
}

// ── VARA engine ─────────────────────────────────────────────────────────

QString AetherHfSettings::varaHost()
{
    const QString h = section(readObj(), kSectionVara)
                          .value(QLatin1String(kVaraHost)).toString();
    return h.isEmpty() ? kDefaultHost : h;
}

void AetherHfSettings::setVaraHost(const QString& host)
{
    QJsonObject v = section(readObj(), kSectionVara);
    // An empty host would silently become the default on the next read; store
    // the default explicitly so what was saved is what comes back.
    v[QLatin1String(kVaraHost)] =
        host.trimmed().isEmpty() ? kDefaultHost : host.trimmed();
    writeSection(kSectionVara, v);
}

int AetherHfSettings::varaCommandPort()
{
    const QString s = section(readObj(), kSectionVara)
                          .value(QLatin1String(kVaraCommandPort)).toString();
    if (s.isEmpty()) return kDefaultVaraCommandPort;
    bool ok = false;
    const int p = s.toInt(&ok);
    if (!ok || p < kMinPort || p > kMaxPort) return kDefaultVaraCommandPort;
    return p;
}

void AetherHfSettings::setVaraCommandPort(int port)
{
    if (port < kMinPort || port > kMaxPort) port = kDefaultVaraCommandPort;
    QJsonObject v = section(readObj(), kSectionVara);
    v[QLatin1String(kVaraCommandPort)] = QString::number(port);
    writeSection(kSectionVara, v);
}

Vara::Bandwidth AetherHfSettings::varaBandwidth()
{
    const QString s = section(readObj(), kSectionVara)
                          .value(QLatin1String(kVaraBandwidthHz)).toString();
    bool ok = false;
    const int hz = s.toInt(&ok);
    if (!ok) return Vara::Bandwidth::Bw2300;
    switch (hz) {
    case 500:  return Vara::Bandwidth::Bw500;
    case 2300: return Vara::Bandwidth::Bw2300;
    case 2750: return Vara::Bandwidth::Bw2750;
    default:   break;
    }
    // An unrecognised width is worse than useless: the link would never form
    // and nothing would say why. Fall back to the one the modem defaults to.
    return Vara::Bandwidth::Bw2300;
}

void AetherHfSettings::setVaraBandwidth(Vara::Bandwidth bw)
{
    QJsonObject v = section(readObj(), kSectionVara);
    v[QLatin1String(kVaraBandwidthHz)] = QString::number(
        static_cast<int>(bw == Vara::Bandwidth::Unknown ? Vara::Bandwidth::Bw2300
                                                        : bw));
    writeSection(kSectionVara, v);
}

int AetherHfSettings::varaBandwidthHz()
{
    return static_cast<int>(varaBandwidth());
}

Vara::Compression AetherHfSettings::varaCompression()
{
    return compressionFromString(section(readObj(), kSectionVara)
                                    .value(QLatin1String(kVaraCompression))
                                    .toString());
}

void AetherHfSettings::setVaraCompression(Vara::Compression c)
{
    QJsonObject v = section(readObj(), kSectionVara);
    v[QLatin1String(kVaraCompression)] = compressionToString(c);
    writeSection(kSectionVara, v);
}

bool AetherHfSettings::varaLaunchModem()
{
    return boolField(section(readObj(), kSectionVara), kVaraLaunchModem, false);
}

void AetherHfSettings::setVaraLaunchModem(bool on)
{
    QJsonObject v = section(readObj(), kSectionVara);
    v[QLatin1String(kVaraLaunchModem)] = boolText(on);
    writeSection(kSectionVara, v);
}

QString AetherHfSettings::varaModemPath()
{
    return section(readObj(), kSectionVara)
        .value(QLatin1String(kVaraModemPath)).toString();
}

void AetherHfSettings::setVaraModemPath(const QString& path)
{
    QJsonObject v = section(readObj(), kSectionVara);
    v[QLatin1String(kVaraModemPath)] = path;
    writeSection(kSectionVara, v);
}

// ── Mercury engine ──────────────────────────────────────────────────────

bool AetherHfSettings::mercuryAvailable()
{
    // MercuryV2 is not integrated. This returns false rather than being absent
    // so the page has one place to ask, and so the day it is integrated the
    // answer changes here rather than in the UI.
    return false;
}

QString AetherHfSettings::mercuryConfigPath()
{
    return section(readObj(), kSectionMercury)
        .value(QLatin1String(kMercuryConfigPath)).toString();
}

void AetherHfSettings::setMercuryConfigPath(const QString& path)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryConfigPath)] = path;
    writeSection(kSectionMercury, m);
}

int AetherHfSettings::activeBandwidthHz()
{
    switch (engine()) {
    case HfEngine::Vara:    return varaBandwidthHz();
    case HfEngine::Mercury: break;
    }
    // Mercury cannot report a bandwidth yet. Zero means "unknown", which the
    // filter comparison treats as nothing to warn about — better than warning
    // against a number that was invented.
    return 0;
}

} // namespace AetherSDR
