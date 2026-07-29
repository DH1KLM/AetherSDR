#include "core/VaraProtocol.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR::Vara;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

Message parse(const char* line)
{
    return parseMessage(QString::fromLatin1(line));
}

}  // namespace

int main()
{
    // ── Framing. The modem terminates with a bare CR, packs several messages
    //    into one read, and will split a message across reads. All three have
    //    to work or the parser desynchronises.
    {
        QByteArray buf("OK\rIAMALIVE\rBUSY O");
        const QList<QByteArray> frames = splitFrames(buf);
        report("splitFrames returns the two complete frames",
               frames.size() == 2 && frames.at(0) == "OK" && frames.at(1) == "IAMALIVE");
        report("splitFrames leaves the partial frame buffered", buf == "BUSY O");

        buf.append("N\r");
        const QList<QByteArray> more = splitFrames(buf);
        report("a partial frame completes on the next read",
               more.size() == 1 && more.at(0) == "BUSY ON");
        report("buffer is drained once the frame completes", buf.isEmpty());
    }
    {
        QByteArray buf("OK\r\rWRONG\r");
        const QList<QByteArray> frames = splitFrames(buf);
        report("a doubled terminator does not yield an empty message",
               frames.size() == 2 && frames.at(0) == "OK" && frames.at(1) == "WRONG");
    }
    {
        QByteArray buf("NOTERMINATOR");
        const QList<QByteArray> frames = splitFrames(buf);
        report("a frame with no terminator is held, not emitted",
               frames.isEmpty() && buf == "NOTERMINATOR");
    }

    // ── Bare status messages.
    report("OK", parse("OK").type == MessageType::Ok);
    report("WRONG", parse("WRONG").type == MessageType::Wrong);
    report("IAMALIVE", parse("IAMALIVE").type == MessageType::IAmAlive);
    report("DISCONNECTED", parse("DISCONNECTED").type == MessageType::Disconnected);
    report("PENDING", parse("PENDING").type == MessageType::Pending);
    report("CANCELPENDING", parse("CANCELPENDING").type == MessageType::CancelPending);

    // ── The two link spellings. "ENCRYPTED LINK" puts the words in the
    //    opposite order from "LINK ENCRYPTED"; the modem emits both, and the
    //    exact-match arm must be reached before the "LINK " prefix arm.
    {
        const Message a = parse("ENCRYPTED LINK");
        report("'ENCRYPTED LINK' parses as an encrypted link",
               a.type == MessageType::Link && a.link == LinkState::Encrypted);
        const Message b = parse("UNENCRYPTED LINK");
        report("'UNENCRYPTED LINK' parses as an unencrypted link",
               b.type == MessageType::Link && b.link == LinkState::Unencrypted);
        const Message c = parse("LINK ENCRYPTED");
        report("'LINK ENCRYPTED' parses as an encrypted link",
               c.type == MessageType::Link && c.link == LinkState::Encrypted);
        const Message d = parse("LINK REGISTERED");
        report("'LINK REGISTERED' parses as registered",
               d.type == MessageType::Link && d.link == LinkState::Registered);
    }

    // ── CONNECTED. VARA HF carries a bandwidth; VARA FM does not, so the
    //    field is optional and its absence must not fail the whole message.
    {
        const Message m = parse("CONNECTED KK7GWY W1AW 2300");
        report("CONNECTED yields both callsigns and the bandwidth",
               m.type == MessageType::Connected && m.source == "KK7GWY"
                   && m.destination == "W1AW" && m.bandwidthHz == 2300);
        const Message fm = parse("CONNECTED KK7GWY W1AW");
        report("CONNECTED without a bandwidth still parses (VARA FM)",
               fm.type == MessageType::Connected && fm.source == "KK7GWY"
                   && fm.destination == "W1AW" && fm.bandwidthHz == 0);
        const Message bad = parse("CONNECTED KK7GWY");
        report("a truncated CONNECTED is Unknown, not a half-built link",
               bad.type == MessageType::Unknown && bad.raw == "CONNECTED KK7GWY");
    }

    // ── CQFRAME.
    {
        const Message m = parse("CQFRAME W1AW 2300");
        report("CQFRAME yields the caller and bandwidth",
               m.type == MessageType::CqFrame && m.source == "W1AW" && m.bandwidthHz == 2300);
    }

    // ── Flags.
    report("BUSY ON sets the flag", parse("BUSY ON").flag);
    report("BUSY OFF clears the flag", !parse("BUSY OFF").flag);
    report("BUSY is typed", parse("BUSY ON").type == MessageType::Busy);
    report("PTT ON sets the flag", parse("PTT ON").flag);
    report("PTT OFF clears the flag", !parse("PTT OFF").flag);
    report("PTT is typed", parse("PTT ON").type == MessageType::Ptt);

    // ── BITRATE carries the adaptive speed level. The spacing between the
    //    bracketed level and the number is not fixed, so the parser must not
    //    depend on a single space.
    {
        const Message m = parse("BITRATE (11)  7536 bps");
        report("BITRATE yields level and bps with doubled spacing",
               m.type == MessageType::Bitrate && m.speedLevel == 11
                   && m.bitsPerSecond == 7536);
        const Message one = parse("BITRATE (1) 60 bps");
        report("BITRATE parses the slowest level",
               one.type == MessageType::Bitrate && one.speedLevel == 1
                   && one.bitsPerSecond == 60);
        // Observed on v4.9.0: a trailing direction token, undocumented anywhere.
        const Message tx = parse("BITRATE (4)  175 bps TX");
        report("BITRATE with a TX suffix parses level, bps and direction",
               tx.type == MessageType::Bitrate && tx.speedLevel == 4
                   && tx.bitsPerSecond == 175
                   && tx.bitrateDirection == BitrateDirection::Transmit);
        const Message rx = parse("BITRATE (4)  175 bps RX");
        report("BITRATE with an RX suffix parses direction",
               rx.bitrateDirection == BitrateDirection::Receive);
        report("BITRATE without a suffix leaves direction Unspecified",
               parse("BITRATE (11) 7536 bps").bitrateDirection
                   == BitrateDirection::Unspecified);

        const Message bad = parse("BITRATE garbage");
        report("a malformed BITRATE is Unknown rather than level 0",
               bad.type == MessageType::Unknown);
    }

    // ── Buffer accounting and its near-namesake.
    {
        const Message m = parse("BUFFER 1024");
        report("BUFFER yields the outstanding byte count",
               m.type == MessageType::Buffer && m.count == 1024);
        const Message c = parse("CLEANTXBUFFER BUFFEREMPTY");
        report("CLEANTXBUFFER BUFFEREMPTY does not get mistaken for BUFFER",
               c.type == MessageType::CleanTxBuffer && c.cleanTx == CleanTxResult::BufferEmpty);
        const Message ok = parse("CLEANTXBUFFER OK");
        report("CLEANTXBUFFER OK does not get mistaken for a bare OK",
               ok.type == MessageType::CleanTxBuffer && ok.cleanTx == CleanTxResult::Ok);
        const Message failed = parse("CLEANTXBUFFER FAILED");
        report("CLEANTXBUFFER FAILED",
               failed.type == MessageType::CleanTxBuffer
                   && failed.cleanTx == CleanTxResult::Failed);
    }

    // ── Scalars and lists.
    {
        const Message sn = parse("SN 12.5");
        report("SN yields a signal-to-noise value",
               sn.type == MessageType::SignalToNoise && sn.value > 12.4 && sn.value < 12.6);
        const Message neg = parse("SN -3.5");
        report("SN parses a negative value",
               neg.type == MessageType::SignalToNoise && neg.value < -3.4 && neg.value > -3.6);
        const Message v = parse("VERSION 4.8.6");
        report("VERSION yields the version text",
               v.type == MessageType::Version && v.text == "4.8.6");
        const Message r = parse("REGISTERED KK7GWY W1AW");
        report("REGISTERED yields every callsign",
               r.type == MessageType::Registered && r.callsigns.size() == 2
                   && r.callsigns.at(0) == "KK7GWY" && r.callsigns.at(1) == "W1AW");
        const Message e = parse("ENCRYPTION DISABLED");
        report("ENCRYPTION DISABLED",
               e.type == MessageType::Encryption && e.encryption == EncryptionState::Disabled);
        const Message er = parse("ENCRYPTION READY");
        report("ENCRYPTION READY",
               er.type == MessageType::Encryption && er.encryption == EncryptionState::Ready);
    }

    // ── Observed on real VARA HF v4.9.0 but absent from every published
    //    description of the interface. Two words, so it must not be mistaken
    //    for a prefixed form.
    report("MISSING SOUNDCARD is typed, not Unknown",
           parse("MISSING SOUNDCARD").type == MessageType::MissingSoundcard);

    // ── An unrecognised message must survive intact for logging, not vanish.
    {
        const Message m = parse("SOMETHINGNEW 42");
        report("an unknown message keeps its raw text",
               m.type == MessageType::Unknown && m.raw == "SOMETHINGNEW 42");
    }

    // ── Command formatting. These assert exact bytes because both details
    //    have bitten real implementations: the terminator is a bare CR (a
    //    trailing LF is rejected), and the bandwidth command has NO space.
    report("commands terminate with a bare CR, not CRLF", cmdVersion() == QByteArray("VERSION\r"));
    report("BW2300 has no space after BW",
           cmdBandwidth(Bandwidth::Bw2300) == QByteArray("BW2300\r"));
    report("BW500", cmdBandwidth(Bandwidth::Bw500) == QByteArray("BW500\r"));
    report("BW2750", cmdBandwidth(Bandwidth::Bw2750) == QByteArray("BW2750\r"));
    report("MYCALL joins callsigns with single spaces",
           cmdMyCall(QStringList{"KK7GWY", "W1AW"}) == QByteArray("MYCALL KK7GWY W1AW\r"));
    report("LISTEN ON", cmdListen(true) == QByteArray("LISTEN ON\r"));
    report("LISTEN OFF", cmdListen(false) == QByteArray("LISTEN OFF\r"));
    report("LISTEN CQ", cmdListenCq() == QByteArray("LISTEN CQ\r"));
    report("CONNECT", cmdConnect("KK7GWY", "W1AW") == QByteArray("CONNECT KK7GWY W1AW\r"));
    report("DISCONNECT", cmdDisconnect() == QByteArray("DISCONNECT\r"));
    report("ABORT", cmdAbort() == QByteArray("ABORT\r"));
    report("CQFRAME", cmdCqFrame("KK7GWY", Bandwidth::Bw2300)
                          == QByteArray("CQFRAME KK7GWY 2300\r"));
    report("COMPRESSION OFF", cmdCompression(Compression::Off) == QByteArray("COMPRESSION OFF\r"));
    report("COMPRESSION TEXT",
           cmdCompression(Compression::Text) == QByteArray("COMPRESSION TEXT\r"));
    report("COMPRESSION FILES",
           cmdCompression(Compression::Files) == QByteArray("COMPRESSION FILES\r"));
    report("CHAT ON", cmdChat(true) == QByteArray("CHAT ON\r"));
    report("CHAT OFF", cmdChat(false) == QByteArray("CHAT OFF\r"));
    report("CLEANTXBUFFER", cmdCleanTxBuffer() == QByteArray("CLEANTXBUFFER\r"));

    // ── Round trip: a CONNECTED we could plausibly have caused.
    {
        QByteArray wire("CONNECTED KK7GWY W1AW 2300\rBITRATE (9) 4410 bps\r");
        const QList<QByteArray> frames = splitFrames(wire);
        report("a two-message read frames and parses end to end",
               frames.size() == 2
                   && parseMessage(QString::fromLatin1(frames.at(0))).type
                          == MessageType::Connected
                   && parseMessage(QString::fromLatin1(frames.at(1))).speedLevel == 9);
    }

    if (g_failed == 0) {
        std::printf("\nAll VARA protocol tests passed.\n");
    } else {
        std::printf("\n%d VARA protocol test(s) FAILED.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
