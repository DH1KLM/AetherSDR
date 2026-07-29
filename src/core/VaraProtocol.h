#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR::Vara {

// VARA modem host protocol — pure framing, parsing and command formatting,
// with no I/O. VaraClient owns the sockets; everything here is testable
// without one.
//
// PROVENANCE: this vocabulary is documented in docs/varac-cleanroom-design.md
// §3.5. It comes from EA5HVK's published specification (§3.6 "VARA uses a TCP
// link to connect with others applications") plus independent open-source
// implementations of the same public interface. Nothing here is derived from
// inspecting a proprietary binary.
//
// TRANSPORT. Two TCP sockets to the modem: a command port (8300 by default)
// and a data port (8301). A second modem instance conventionally uses
// 8302/8303. The command channel is ASCII, each message terminated by a bare
// CR ('\r') — NOT CRLF. Several messages routinely arrive in one read, and a
// read can split mid-message, so framing must be buffered (see splitFrames).
// The data port is an opaque bidirectional byte stream: the ARQ payload, with
// no framing of its own imposed by this layer.
//
// REPLIES ARE POSITIONAL. The modem answers a command with a bare "OK" or
// "WRONG" that names nothing. Correlation is by order alone: the reply belongs
// to the oldest command still awaiting one. Any client that pipelines commands
// MUST keep a FIFO of outstanding commands and pop it on each OK/WRONG, or it
// will silently attribute results to the wrong request. VaraClient does this.
//
// TRANSMIT. Several commands cause the modem to key the radio — CONNECT,
// CQFRAME, and writing to the data port all transmit. Per Constitution
// Principle VI nothing here transmits on its own: there is no auto-connect, no
// beacon, and no periodic CQ. Transmission happens only when a caller
// explicitly asks for it. LISTEN is receive-only and safe.
//
// DELIBERATE OMISSIONS. Two commands in the modem's vocabulary are not
// exposed here, both on purpose:
//   * TUNE — keys a tuning carrier. A transmit-keying control in the sense of
//     the automation bridge's TX guard; it needs the same operator-intent
//     treatment and a UI affordance before it is worth having.
//   * ENCRYPTION — VARA can negotiate an encrypted link. On amateur
//     allocations 47 CFR §97.113(a)(4) prohibits messages encoded to obscure
//     their meaning, so AetherSDR does not offer it. The ENCRYPTION and
//     ...LINK notifications are still *parsed*, so we can observe and report
//     the state a peer selects; we simply never ask for it.

enum class Bandwidth {
    Unknown = 0,
    Bw500 = 500,
    Bw2300 = 2300,
    Bw2750 = 2750,
};

enum class Compression {
    Off,
    Text,
    Files,
};

enum class LinkState {
    Unknown,
    Registered,
    Unregistered,
    Encrypted,
    Unencrypted,
};

enum class CleanTxResult {
    Unknown,
    BufferEmpty,
    Failed,
    Ok,
};

enum class EncryptionState {
    Unknown,
    Disabled,
    Ready,
};

enum class MessageType {
    Unknown,         // unrecognised — surfaced rather than dropped
    Ok,              // acks the oldest outstanding command
    Wrong,           // rejects the oldest outstanding command
    IAmAlive,        // modem keepalive
    Pending,         // an inbound connection attempt is being decoded
    CancelPending,   // ...and it did not complete
    Connected,       // source, destination, bandwidthHz
    Disconnected,
    Busy,            // flag: channel busy detector
    Ptt,             // flag: modem asserts/releases PTT
    Version,         // text
    Registered,      // callsigns registered for full-speed operation
    SignalToNoise,   // value, dB
    Tune,            // value
    Buffer,          // count: bytes still queued for transmission
    CleanTxBuffer,   // cleanTx
    Link,            // link
    Bitrate,         // speedLevel (1-11) and bitsPerSecond
    Encryption,      // encryption
    CqFrame,         // source, bandwidthHz
    // Observed on VARA HF v4.9.0 (see docs/varac-cleanroom-design.md §3.6) but
    // absent from every published description of the interface: the modem
    // repeats this roughly every six seconds while it has no usable audio
    // device. It means "no soundcard selected OR none found" — the modem does
    // not distinguish the two, so a first-run install with empty
    // VARA.ini [Soundcard] entries looks identical to a broken audio stack.
    // Fatal to a session: no link can be established until it stops.
    MissingSoundcard,
};

// One decoded message. Only the fields relevant to `type` are populated; the
// rest keep their defaults. `raw` always holds the original line, so an
// Unknown message (a newer modem than we know about) can still be logged.
struct Message {
    MessageType type = MessageType::Unknown;
    QString raw;

    bool flag = false;      // Busy, Ptt
    QString source;         // Connected, CqFrame
    QString destination;    // Connected
    int bandwidthHz = 0;    // Connected, CqFrame
    QStringList callsigns;  // Registered
    double value = 0.0;     // SignalToNoise, Tune
    int count = 0;          // Buffer
    int speedLevel = 0;     // Bitrate — the 1..11 level; see below
    int bitsPerSecond = 0;  // Bitrate
    QString text;           // Version
    LinkState link = LinkState::Unknown;
    CleanTxResult cleanTx = CleanTxResult::Unknown;
    EncryptionState encryption = EncryptionState::Unknown;
};

// Consume whole CR-terminated frames from `buffer`, leaving any trailing
// partial message in place for the next read. Empty frames are dropped (a
// doubled CR is not a message).
QList<QByteArray> splitFrames(QByteArray& buffer);

// Decode one already-framed line. Never fails: an unrecognised line comes back
// as MessageType::Unknown with `raw` set.
Message parseMessage(const QString& line);

// Command builders. Each returns the exact bytes to write to the command
// socket, CR included.
QByteArray cmdVersion();
QByteArray cmdMyCall(const QStringList& callsigns);  // caller must pass 1..5
QByteArray cmdListen(bool on);
QByteArray cmdListenCq();
QByteArray cmdConnect(const QString& source, const QString& destination);
QByteArray cmdDisconnect();
QByteArray cmdAbort();
QByteArray cmdBandwidth(Bandwidth bandwidth);
QByteArray cmdCqFrame(const QString& callsign, Bandwidth bandwidth);
QByteArray cmdCompression(Compression compression);
QByteArray cmdChat(bool on);
QByteArray cmdCleanTxBuffer();

// The modem accepts at most this many callsigns on one MYCALL.
inline constexpr int kMaxMyCallCallsigns = 5;

// Default host ports. A second instance conventionally sits on 8302/8303.
inline constexpr quint16 kDefaultCommandPort = 8300;
inline constexpr quint16 kDefaultDataPort = 8301;

// Helpers for logging and for the probe tool.
QString toString(MessageType type);
QString toString(Bandwidth bandwidth);

}  // namespace AetherSDR::Vara
