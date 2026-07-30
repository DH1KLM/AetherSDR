#include "core/VaraSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {
// Single nested-JSON key holding the whole VARA configuration (Principle V).
const QString kRootKey = QStringLiteral("Vara");

constexpr const char* kFieldEnabled       = "enabled";
constexpr const char* kFieldHost          = "host";
constexpr const char* kFieldCommandPort   = "commandPort";
constexpr const char* kFieldCallsign      = "callsign";
constexpr const char* kFieldBandwidthHz   = "bandwidthHz";
constexpr const char* kFieldCompression   = "compression";
constexpr const char* kFieldEnforceFilter = "enforceFilterWidth";
constexpr const char* kFieldLaunchModem   = "launchModem";
constexpr const char* kFieldModemPath     = "modemPath";

const QString kDefaultHost = QStringLiteral("127.0.0.1");

// Booleans are stored as "True"/"False" strings to match the shape the rest of
// AppSettings uses, so a settings file stays readable and consistent.
bool boolField(const QJsonObject& o, const char* field, bool fallback)
{
    const QString v = o.value(QLatin1String(field)).toString();
    if (v.isEmpty()) return fallback;
    return v == QLatin1String("True");
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

QJsonObject VaraSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty()) return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void VaraSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

bool VaraSettings::enabled()
{
    return boolField(readObj(), kFieldEnabled, false);
}

void VaraSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldEnabled)] =
        on ? QStringLiteral("True") : QStringLiteral("False");
    write(o);
}

QString VaraSettings::host()
{
    const QString h = readObj().value(QLatin1String(kFieldHost)).toString();
    return h.isEmpty() ? kDefaultHost : h;
}

void VaraSettings::setHost(const QString& host)
{
    QJsonObject o = readObj();
    // An empty host would silently become the default on the next read; store
    // the default explicitly so what was saved is what comes back.
    o[QLatin1String(kFieldHost)] = host.trimmed().isEmpty() ? kDefaultHost
                                                            : host.trimmed();
    write(o);
}

int VaraSettings::commandPort()
{
    const QString v = readObj().value(QLatin1String(kFieldCommandPort)).toString();
    if (v.isEmpty()) return kDefaultCommandPort;
    bool ok = false;
    const int p = v.toInt(&ok);
    if (!ok || p < kMinPort || p > kMaxPort) return kDefaultCommandPort;
    return p;
}

void VaraSettings::setCommandPort(int port)
{
    if (port < kMinPort || port > kMaxPort) port = kDefaultCommandPort;
    QJsonObject o = readObj();
    o[QLatin1String(kFieldCommandPort)] = QString::number(port);
    write(o);
}

QString VaraSettings::callsign()
{
    return readObj().value(QLatin1String(kFieldCallsign)).toString();
}

void VaraSettings::setCallsign(const QString& call)
{
    QJsonObject o = readObj();
    // The modem expects an upper-case callsign; normalising here means the page
    // and any headless caller cannot disagree about it.
    o[QLatin1String(kFieldCallsign)] = call.trimmed().toUpper();
    write(o);
}

Vara::Bandwidth VaraSettings::bandwidth()
{
    const QString v = readObj().value(QLatin1String(kFieldBandwidthHz)).toString();
    bool ok = false;
    const int hz = v.toInt(&ok);
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

void VaraSettings::setBandwidth(Vara::Bandwidth bw)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldBandwidthHz)] =
        QString::number(static_cast<int>(bw == Vara::Bandwidth::Unknown
                                             ? Vara::Bandwidth::Bw2300
                                             : bw));
    write(o);
}

int VaraSettings::bandwidthHz()
{
    return static_cast<int>(bandwidth());
}

Vara::Compression VaraSettings::compression()
{
    return compressionFromString(
        readObj().value(QLatin1String(kFieldCompression)).toString());
}

void VaraSettings::setCompression(Vara::Compression c)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldCompression)] = compressionToString(c);
    write(o);
}

bool VaraSettings::enforceFilterWidth()
{
    return boolField(readObj(), kFieldEnforceFilter, true);
}

void VaraSettings::setEnforceFilterWidth(bool on)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldEnforceFilter)] =
        on ? QStringLiteral("True") : QStringLiteral("False");
    write(o);
}

bool VaraSettings::launchModem()
{
    return boolField(readObj(), kFieldLaunchModem, false);
}

void VaraSettings::setLaunchModem(bool on)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldLaunchModem)] =
        on ? QStringLiteral("True") : QStringLiteral("False");
    write(o);
}

QString VaraSettings::modemPath()
{
    return readObj().value(QLatin1String(kFieldModemPath)).toString();
}

void VaraSettings::setModemPath(const QString& path)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldModemPath)] = path;
    write(o);
}

} // namespace AetherSDR
