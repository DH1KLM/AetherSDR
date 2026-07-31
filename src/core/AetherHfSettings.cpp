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

constexpr const char* kMercuryHost        = "host";
constexpr const char* kMercuryCommandPort = "commandPort";
constexpr const char* kMercuryBandwidthHz = "bandwidthHz";
constexpr const char* kMercuryLaunch      = "launch";
constexpr const char* kMercuryBinaryPath  = "binaryPath";
constexpr const char* kMercurySoundSystem = "soundSystem";
constexpr const char* kMercuryCapture     = "captureDevice";
constexpr const char* kMercuryPlayback    = "playbackDevice";
constexpr const char* kMercuryModeIndex   = "modeIndex";
constexpr const char* kMercuryPublic      = "public";
constexpr const char* kMercuryConfigPath  = "configPath";

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

QString AetherHfSettings::defaultMercurySoundSystem()
{
    // Mirrors Mercury's own default for the platform, so leaving this alone
    // gives the same behaviour as running the binary by hand.
#if defined(Q_OS_WIN)
    return QStringLiteral("dsound");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("coreaudio");
#else
    return QStringLiteral("alsa");
#endif
}

bool AetherHfSettings::mercuryAvailable()
{
    // Two ways to be usable, and the engine only needs one of them: either this
    // starts the binary and has been told where it is, or somebody else runs it
    // — on this machine or another — and all this needs is a host to reach.
    if (mercuryLaunch())
        return !mercuryBinaryPath().trimmed().isEmpty();
    return !mercuryHost().trimmed().isEmpty();
}

QString AetherHfSettings::mercuryHost()
{
    const QString h = section(readObj(), kSectionMercury)
                          .value(QLatin1String(kMercuryHost)).toString().trimmed();
    return h.isEmpty() ? kDefaultHost : h;
}

void AetherHfSettings::setMercuryHost(const QString& host)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    const QString h = host.trimmed();
    m[QLatin1String(kMercuryHost)] = h.isEmpty() ? kDefaultHost : h;
    writeSection(kSectionMercury, m);
}

int AetherHfSettings::mercuryCommandPort()
{
    const int p = section(readObj(), kSectionMercury)
                      .value(QLatin1String(kMercuryCommandPort)).toString().toInt();
    return (p >= kMinPort && p <= kMaxPort) ? p : kDefaultMercuryCommandPort;
}

void AetherHfSettings::setMercuryCommandPort(int port)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    const int p = (port >= kMinPort && port <= kMaxPort)
                      ? port : kDefaultMercuryCommandPort;
    m[QLatin1String(kMercuryCommandPort)] = QString::number(p);
    writeSection(kSectionMercury, m);
}

Vara::Bandwidth AetherHfSettings::mercuryBandwidth()
{
    // Mercury accepts the same three tokens VARA does — BW500, BW2300, BW2750 —
    // and rejects anything else, so an unrecognised stored value falls back
    // rather than being sent and drawing a WRONG.
    const QString s = section(readObj(), kSectionMercury)
                          .value(QLatin1String(kMercuryBandwidthHz)).toString();
    bool ok = false;
    const int hz = s.toInt(&ok);
    if (!ok) return Vara::Bandwidth::Bw2300;
    switch (hz) {
    case 500:  return Vara::Bandwidth::Bw500;
    case 2300: return Vara::Bandwidth::Bw2300;
    case 2750: return Vara::Bandwidth::Bw2750;
    default:   return Vara::Bandwidth::Bw2300;
    }
}

int AetherHfSettings::mercuryBandwidthHz()
{
    return static_cast<int>(mercuryBandwidth());
}

void AetherHfSettings::setMercuryBandwidth(Vara::Bandwidth bw)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryBandwidthHz)] = QString::number(static_cast<int>(bw));
    writeSection(kSectionMercury, m);
}

bool AetherHfSettings::mercuryLaunch()
{
    return boolField(section(readObj(), kSectionMercury), kMercuryLaunch, false);
}

void AetherHfSettings::setMercuryLaunch(bool on)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryLaunch)] = boolText(on);
    writeSection(kSectionMercury, m);
}

QString AetherHfSettings::mercuryBinaryPath()
{
    return section(readObj(), kSectionMercury)
        .value(QLatin1String(kMercuryBinaryPath)).toString();
}

void AetherHfSettings::setMercuryBinaryPath(const QString& path)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryBinaryPath)] = path.trimmed();
    writeSection(kSectionMercury, m);
}

QString AetherHfSettings::mercurySoundSystem()
{
    const QString s = section(readObj(), kSectionMercury)
                          .value(QLatin1String(kMercurySoundSystem)).toString().trimmed();
    return s.isEmpty() ? defaultMercurySoundSystem() : s;
}

void AetherHfSettings::setMercurySoundSystem(const QString& system)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercurySoundSystem)] = system.trimmed();
    writeSection(kSectionMercury, m);
}

QString AetherHfSettings::mercuryCaptureDevice()
{
    return section(readObj(), kSectionMercury)
        .value(QLatin1String(kMercuryCapture)).toString();
}

void AetherHfSettings::setMercuryCaptureDevice(const QString& device)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryCapture)] = device.trimmed();
    writeSection(kSectionMercury, m);
}

QString AetherHfSettings::mercuryPlaybackDevice()
{
    return section(readObj(), kSectionMercury)
        .value(QLatin1String(kMercuryPlayback)).toString();
}

void AetherHfSettings::setMercuryPlaybackDevice(const QString& device)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryPlayback)] = device.trimmed();
    writeSection(kSectionMercury, m);
}

int AetherHfSettings::mercuryModeIndex()
{
    const QJsonObject m = section(readObj(), kSectionMercury);
    const QString raw = m.value(QLatin1String(kMercuryModeIndex)).toString();
    if (raw.isEmpty())
        return 1;                       // Mercury's own default, DATAC3
    bool ok = false;
    const int v = raw.toInt(&ok);
    return (ok && v >= 0) ? v : 1;
}

void AetherHfSettings::setMercuryModeIndex(int index)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryModeIndex)] = QString::number(index < 0 ? 1 : index);
    writeSection(kSectionMercury, m);
}

QString AetherHfSettings::mercuryConfigPath()
{
    return section(readObj(), kSectionMercury)
        .value(QLatin1String(kMercuryConfigPath)).toString();
}

void AetherHfSettings::setMercuryConfigPath(const QString& path)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryConfigPath)] = path.trimmed();
    writeSection(kSectionMercury, m);
}

bool AetherHfSettings::mercuryPublic()
{
    return boolField(section(readObj(), kSectionMercury), kMercuryPublic, false);
}

void AetherHfSettings::setMercuryPublic(bool on)
{
    QJsonObject m = section(readObj(), kSectionMercury);
    m[QLatin1String(kMercuryPublic)] = boolText(on);
    writeSection(kSectionMercury, m);
}

// ── The selected engine ─────────────────────────────────────────────────

QString AetherHfSettings::activeHost()
{
    switch (engine()) {
    case HfEngine::Mercury: return mercuryHost();
    case HfEngine::Vara:    break;
    }
    return varaHost();
}

int AetherHfSettings::activeCommandPort()
{
    switch (engine()) {
    case HfEngine::Mercury: return mercuryCommandPort();
    case HfEngine::Vara:    break;
    }
    return varaCommandPort();
}

int AetherHfSettings::activeDataPort()
{
    return activeCommandPort() + 1;
}

Vara::Bandwidth AetherHfSettings::activeBandwidth()
{
    switch (engine()) {
    case HfEngine::Mercury: return mercuryBandwidth();
    case HfEngine::Vara:    break;
    }
    return varaBandwidth();
}

int AetherHfSettings::activeBandwidthHz()
{
    switch (engine()) {
    case HfEngine::Vara:    return varaBandwidthHz();
    case HfEngine::Mercury: return mercuryBandwidthHz();
    }
    return 0;
}

} // namespace AetherSDR
