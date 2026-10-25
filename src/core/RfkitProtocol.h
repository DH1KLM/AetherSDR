#pragma once

#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace AetherSDR {

//DH1KLM RF-KIT amplifier REST API codec.
//DH1KLM
//DH1KLM Protocol authority: the vendor's own published "RFKIT API and SDK
//DH1KLM Documentation" (OpenAPI document, version 0.9.0), a public manufacturer
//DH1KLM document, so this is a clean-room input under constitution Principle IV.
//DH1KLM See docs/architecture/rfkit-amplifier-design.md for the design note.
//DH1KLM
//DH1KLM Unlike every other amplifier peripheral here the wire is plain HTTP/1.1
//DH1KLM carrying JSON, not a socket protocol with its own framing, so this layer is
//DH1KLM stateless: one parse function per endpoint (bytes in, optional struct out)
//DH1KLM and one body builder per writable endpoint. RfkitConnection owns the
//DH1KLM polling, the request queue and the HTTP plumbing.
//DH1KLM
//DH1KLM   GET  /info                   device identity + firmware versions
//DH1KLM   GET  /power                  forward / reflected / SWR / temp / volts / amps
//DH1KLM   GET  /data                   band, frequency, free-text status
//DH1KLM   GET  /tuner                  tuner mode + L/C network (read-only)
//DH1KLM   GET  /antennas               every antenna with ACTIVE/AVAILABLE/DISABLED
//DH1KLM   GET  /antennas/active        the active antenna
//DH1KLM   PUT  /antennas/active        select an antenna
//DH1KLM   GET  /operate-mode           OPERATE | STANDBY
//DH1KLM   PUT  /operate-mode           set OPERATE | STANDBY
//DH1KLM   GET  /operational-interface  UNIV | CAT | UDP | TCI (+ read-only error)
//DH1KLM   POST /error/reset            clear the amplifier's error state
//DH1KLM
//DH1KLM Everything parsed here arrives from a LAN peer, so constitution Principle
//DH1KLM VII applies: bodies are size-capped, a document that is not the expected
//DH1KLM JSON shape yields nullopt rather than a half-filled struct, and a
//DH1KLM control-state enum (operate mode, operational interface, antenna type)
//DH1KLM that this codec cannot interpret is refused rather than guessed. Fields the
//DH1KLM document marks descriptive only (units, max_value, tuner L/C) default
//DH1KLM quietly when absent, so a firmware that omits one does not blank the applet.
namespace Rfkit {

//DH1KLM The API document's examples use port 8080 (http://localhost:8080/...). The
//DH1KLM Peripherals row exposes it as an editable field.
inline constexpr quint16 kDefaultPort = 8080;

//DH1KLM No endpoint answers with more than a few hundred bytes. A peer that sends
//DH1KLM more is not an RF-KIT, so the body is dropped rather than parsed.
inline constexpr int kMaxBodyBytes = 64 * 1024;

inline constexpr int kMinInternalAntenna = 1;
inline constexpr int kMaxInternalAntenna = 4;
//DH1KLM The external antenna is a single selection. The API document gives
//DH1KLM ExternalAntenna.number (1-16) as readOnly: it names the port of the
//DH1KLM operator's external antenna switch, which the amplifier reports but cannot
//DH1KLM be told to change, so the selection request carries no number. An external
//DH1KLM Antenna therefore always has number 0; the port the amplifier reports is kept
//DH1KLM separately, as AntennaWithState::externalPort. A real RF2K-S (firmware
//DH1KLM G200C267) lists {"type":"EXTERNAL","state":"AVAILABLE"} with no number while
//DH1KLM it is not active; what it reports while it is active has not been observed.
inline constexpr int kExternalAntennaNumber = 0;
inline constexpr int kMinExternalPort = 1;
inline constexpr int kMaxExternalPort = 16;

//DH1KLM Endpoint paths, relative to the device root.
inline constexpr char kPathInfo[]        = "/info";
inline constexpr char kPathPower[]       = "/power";
inline constexpr char kPathData[]        = "/data";
inline constexpr char kPathTuner[]       = "/tuner";
inline constexpr char kPathAntennas[]    = "/antennas";
inline constexpr char kPathAntennaActive[] = "/antennas/active";
inline constexpr char kPathOperateMode[] = "/operate-mode";
inline constexpr char kPathInterface[]   = "/operational-interface";
inline constexpr char kPathErrorReset[]  = "/error/reset";

//DH1KLM ── Value types ──────────────────────────────────────────────────────────
//DH1KLM The API wraps every measurement as {value, unit[, max_value]}. The unit is
//DH1KLM carried through so the applet shows what the device said instead of
//DH1KLM assuming one.

struct IntValue {
    int value{0};
    QString unit;
};

struct FloatValue {
    float value{0.0f};
    QString unit;
};

//DH1KLM maxValue is the device's reported max_value (the present power limit, not a
//DH1KLM meter range); parsed for completeness, not used for scaling. 0 means "not
//DH1KLM reported".
struct IntWithMax {
    int value{0};
    int maxValue{0};
    QString unit;
};

struct FloatWithMax {
    float value{0.0f};
    float maxValue{0.0f};
    QString unit;
};

//DH1KLM ── Responses ────────────────────────────────────────────────────────────

struct Info {
    QString device;
    QString customName;  //DH1KLM the operator-assigned name; may be empty
    int guiVersion{0};
    int controllerVersion{0};
};

struct Power {
    FloatValue temperature;
    FloatValue voltage;
    FloatValue current;
    IntWithMax forward;
    IntWithMax reflected;
    FloatWithMax swr;
};

struct Data {
    IntValue band;
    IntValue frequency;
    QString status;  //DH1KLM free text; the API document does not enumerate it
};

enum class TunerMode { Unknown, Bypass, Manual, AutoTuning, Auto, Off };
enum class TunerSetup { Unknown, NotTuned, Bypass, CL, LC };

struct Tuner {
    TunerMode mode{TunerMode::Unknown};
    TunerSetup setup{TunerSetup::Unknown};
    std::optional<IntValue> l;
    std::optional<IntValue> c;
    std::optional<IntValue> tunedFrequency;
    std::optional<IntValue> segmentSize;
};

enum class OperateMode { Standby, Operate };

enum class OperationalInterface { Univ, Cat, Udp, Tci };

struct InterfaceState {
    //DH1KLM Not named "interface": that is a macro in the Windows SDK headers.
    OperationalInterface active{OperationalInterface::Univ};
    QString error;  //DH1KLM read-only; empty when the device reported none
};

enum class AntennaType { Internal, External };

struct Antenna {
    AntennaType type{AntennaType::Internal};
    int number{1};  //DH1KLM 1-4 for an internal antenna; always 0 for the external one

    bool operator==(const Antenna& other) const
    {
        return type == other.type && number == other.number;
    }
};

enum class AntennaState { Active, Available, Disabled };

struct AntennaWithState {
    Antenna antenna;
    AntennaState state{AntennaState::Available};
    //DH1KLM External entry only: the external switch port the amplifier reported
    //DH1KLM (1-16), or 0 when it sent none or one outside that range.
    int externalPort{0};
};

//DH1KLM The device's error body, sent with a 400 or 409: {"status":..,"reason":..}.
struct ApiError {
    QString status;
    QString reason;
};

//DH1KLM One snapshot of everything the connection has learned. An unset optional
//DH1KLM means "the device has not answered that endpoint yet", which is not the
//DH1KLM same as a reading of zero, and the applet renders it as "—".
struct Status {
    std::optional<Info> info;
    std::optional<Power> power;
    std::optional<Data> data;
    std::optional<Tuner> tuner;
    std::optional<OperateMode> operate;
    std::optional<InterfaceState> interfaceState;
    std::optional<QVector<AntennaWithState>> antennas;
};

//DH1KLM ── Parsers ──────────────────────────────────────────────────────────────
//DH1KLM Each takes the raw HTTP body. nullopt means the body was oversized, not
//DH1KLM JSON, not the expected shape, or carried a control value this codec cannot
//DH1KLM interpret.

std::optional<Info> parseInfo(const QByteArray& body);
std::optional<Power> parsePower(const QByteArray& body);
std::optional<Data> parseData(const QByteArray& body);
std::optional<Tuner> parseTuner(const QByteArray& body);
std::optional<OperateMode> parseOperateMode(const QByteArray& body);
std::optional<InterfaceState> parseInterface(const QByteArray& body);
std::optional<Antenna> parseAntenna(const QByteArray& body);
//DH1KLM The /antennas list: a bare array, or the {"antennas":[...]} object a real
//DH1KLM RF2K-S sends. Entries that are malformed are skipped; the call fails only if
//DH1KLM the document is neither.
std::optional<QVector<AntennaWithState>> parseAntennas(const QByteArray& body);
std::optional<ApiError> parseApiError(const QByteArray& body);

//DH1KLM ── Request bodies ───────────────────────────────────────────────────────
//DH1KLM Compact JSON. buildAntennaBody returns an empty array for an antenna the
//DH1KLM device cannot have (an internal number outside 1-4, an external number other
//DH1KLM than 0), so such a request never reaches the wire. The external body carries
//DH1KLM no number, because the API document marks ExternalAntenna.number readOnly.

QByteArray buildOperateModeBody(OperateMode mode);
QByteArray buildAntennaBody(const Antenna& antenna);

//DH1KLM ── Presentation helpers ─────────────────────────────────────────────────

//DH1KLM Temperature in degrees Celsius, converting when the device reports °F.
float temperatureCelsius(const FloatValue& temperature);
//DH1KLM "14.200 MHz" from a frequency in Hz, kHz or MHz; "<value> <unit>" for any
//DH1KLM other unit, so an unexpected unit is shown rather than mis-scaled.
QString formatFrequency(const IntValue& frequency);
//DH1KLM "20 m": the value and the unit the device gave it.
QString formatBand(const IntValue& band);
//DH1KLM "1".."4" for the internal antennas, "EXT" for the external one.
QString antennaLabel(const Antenna& antenna);
QString tunerModeLabel(TunerMode mode);
QString tunerSetupLabel(TunerSetup setup);
QString interfaceLabel(OperationalInterface iface);

}  //DH1KLM namespace Rfkit
}  //DH1KLM namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Rfkit::Status)
