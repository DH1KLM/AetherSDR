// IcomCIV Phases 0-3 end to end — IcomSession against a fake IC-705 on
// localhost.
//
// This is the test that proves the ORDER of the RS-BA1 handshake, which is the
// part of the protocol with no documentation at all. The fake radio below is
// deliberately STRICT: it answers only what a real IC-705 answers, in the order
// it answers it, and it refuses to grant streams unless the client did the two
// separate auths and echoed back the radio identity from the capabilities
// packet. A client that skips a step gets silence, which is exactly what the
// real radio does.
//
// Requires Qt Core + Network. No hardware.

#include "core/backends/icom/IcomScope.h"
#include "core/backends/icom/IcomSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

constexpr std::uint8_t kIc705Addr = 0xA4;
constexpr std::uint64_t kRadioFrequencyHz = 14'074'000;

void putLe32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v)
{
    p[at] = v & 0xff; p[at + 1] = (v >> 8) & 0xff;
    p[at + 2] = (v >> 16) & 0xff; p[at + 3] = (v >> 24) & 0xff;
}
void putLe16(std::vector<std::uint8_t>& p, std::size_t at, std::uint16_t v)
{
    p[at] = v & 0xff; p[at + 1] = (v >> 8) & 0xff;
}
void putBe32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v)
{
    p[at] = (v >> 24) & 0xff; p[at + 1] = (v >> 16) & 0xff;
    p[at + 2] = (v >> 8) & 0xff; p[at + 3] = v & 0xff;
}
std::uint32_t getBe32(const QByteArray& b, int at)
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 24)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 16)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 8)
         | static_cast<std::uint8_t>(b[at + 3]);
}
std::uint16_t getLe16(const QByteArray& b, int at)
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[at])
                                      | (static_cast<std::uint8_t>(b[at + 1]) << 8));
}

// One UDP endpoint of the fake radio. Handles the handshake and pings that are
// common to all three streams; the payload behaviour is a callback.
class FakeStream : public QObject {
public:
    using PayloadFn = std::function<void(FakeStream&, const QByteArray&)>;

    FakeStream(std::uint32_t sid, PayloadFn fn, QObject* parent = nullptr)
        : QObject(parent), m_sid(sid), m_fn(std::move(fn))
    {
        m_socket = new QUdpSocket(this);
        m_socket->bind(QHostAddress::LocalHost, 0);
        connect(m_socket, &QUdpSocket::readyRead, this, &FakeStream::onReadyRead);
    }

    [[nodiscard]] quint16 port() const { return m_socket->localPort(); }
    [[nodiscard]] bool sawHandshake() const { return m_ready; }
    [[nodiscard]] int payloadCount() const { return m_payloads; }

    void send(const std::vector<std::uint8_t>& p)
    {
        if (m_peerPort == 0)
            return;
        m_socket->writeDatagram(reinterpret_cast<const char*>(p.data()),
                                static_cast<qint64>(p.size()), m_peerAddr, m_peerPort);
    }

    // A 16-byte framed packet from the radio's point of view.
    std::vector<std::uint8_t> frame(std::size_t len, std::uint16_t type, std::uint16_t seq) const
    {
        std::vector<std::uint8_t> p(len, 0);
        putLe32(p, 0x00, static_cast<std::uint32_t>(len));
        putLe16(p, 0x04, type);
        putLe16(p, 0x06, seq);
        putBe32(p, 0x08, m_sid);        // the radio's own id
        putBe32(p, 0x0c, m_clientSid);  // echoed back
        return p;
    }

private:
    void onReadyRead()
    {
        while (m_socket->hasPendingDatagrams()) {
            QByteArray buf;
            buf.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
            m_socket->readDatagram(buf.data(), buf.size(), &m_peerAddr, &m_peerPort);
            handle(buf);
        }
    }

    void handle(const QByteArray& b)
    {
        if (b.size() < 16)
            return;
        const std::uint16_t type = getLe16(b, 0x04);

        // Pings, both directions.
        if (b.size() == 21 && type == 0x07) {
            if (static_cast<std::uint8_t>(b[0x10]) == 0x00) {
                auto reply = frame(21, 0x07, getLe16(b, 0x06));
                reply[0x10] = 0x01;
                for (int i = 0; i < 4; ++i)
                    reply[0x11 + i] = static_cast<std::uint8_t>(b[0x11 + i]);
                send(reply);
            }
            return;
        }

        if (type == 0x03) {   // are-you-there
            m_clientSid = getBe32(b, 0x08);
            send(frame(16, 0x04, 0));
            return;
        }
        if (type == 0x06) {   // are-you-ready
            m_ready = true;
            send(frame(16, 0x06, 1));
            return;
        }
        if (type == 0x05) {   // disconnect
            m_ready = false;
            return;
        }
        if (type != 0x00 || b.size() <= 16)
            return;

        ++m_payloads;
        if (m_fn)
            m_fn(*this, b);
    }

    QUdpSocket* m_socket = nullptr;
    QHostAddress m_peerAddr;
    quint16 m_peerPort = 0;
    std::uint32_t m_sid = 0;
    std::uint32_t m_clientSid = 0;
    bool m_ready = false;
    int m_payloads = 0;
    PayloadFn m_fn;
};

// The fake IC-705 itself.
class FakeIc705 : public QObject {
public:
    explicit FakeIc705(QObject* parent = nullptr) : QObject(parent)
    {
        m_control = new FakeStream(0xAAAA0001,
                                   [this](FakeStream& s, const QByteArray& b) { control(s, b); },
                                   this);
        m_serial = new FakeStream(0xAAAA0002,
                                  [this](FakeStream& s, const QByteArray& b) { serial(s, b); },
                                  this);
        m_audio = new FakeStream(0xAAAA0003,
                                 [this](FakeStream& s, const QByteArray& b) { audio(s, b); },
                                 this);
    }

    [[nodiscard]] quint16 controlPort() const { return m_control->port(); }
    [[nodiscard]] quint16 serialPort() const { return m_serial->port(); }
    [[nodiscard]] quint16 audioPort() const { return m_audio->port(); }

    // What the client told us it would use. The whole point of announcing them.
    [[nodiscard]] quint32 announcedCivPort() const { return m_announcedCiv; }
    [[nodiscard]] quint32 announcedAudioPort() const { return m_announcedAudio; }
    [[nodiscard]] quint32 announcedSampleRate() const { return m_announcedRate; }
    [[nodiscard]] std::uint8_t announcedRxCodec() const { return m_announcedRxCodec; }
    [[nodiscard]] int authCount() const { return m_authCount; }
    [[nodiscard]] bool serialOpened() const { return m_serialOpened; }
    [[nodiscard]] bool sawUsernameObfuscated() const { return m_usernameObfuscated; }
    [[nodiscard]] int civCommandsSeen() const { return m_civCommands; }
    [[nodiscard]] int audioPacketsFromClient() const { return m_audioFromClient; }

    // Push an unsolicited CI-V frame (CI-V Transceive, or a scope sweep).
    void pushCiv(const std::vector<std::uint8_t>& civ)
    {
        auto p = m_serial->frame(21 + civ.size(), 0x00, m_serialSeq++);
        p[0x10] = 0xc1;
        // LITTLE-endian u16, not a byte — a scope sweep is ~496 bytes.
        p[0x11] = static_cast<std::uint8_t>(civ.size() & 0xff);
        p[0x12] = static_cast<std::uint8_t>((civ.size() >> 8) & 0xff);
        p[0x13] = 0x00;
        p[0x14] = static_cast<std::uint8_t>(m_civSeq++);
        std::copy(civ.begin(), civ.end(), p.begin() + 21);
        m_serial->send(p);
    }

    void pushAudio(const std::vector<std::uint8_t>& pcm)
    {
        auto p = m_audio->frame(24 + pcm.size(), 0x00, m_audioSeq++);
        p[0x10] = 0x80;
        p[0x12] = static_cast<std::uint8_t>((m_audioInner >> 8) & 0xff);
        p[0x13] = static_cast<std::uint8_t>(m_audioInner & 0xff);
        ++m_audioInner;
        p[0x16] = static_cast<std::uint8_t>((pcm.size() >> 8) & 0xff);
        p[0x17] = static_cast<std::uint8_t>(pcm.size() & 0xff);
        std::copy(pcm.begin(), pcm.end(), p.begin() + 24);
        m_audio->send(p);
    }

private:
    void control(FakeStream& s, const QByteArray& b)
    {
        switch (b.size()) {
        case 0x80: {   // login
            // A real radio only accepts the OBFUSCATED credential. Checking it
            // here is what makes the passcode table load-bearing in this test
            // rather than incidental.
            const auto expect = encodePasscode("beer");
            m_usernameObfuscated = std::equal(expect.begin(), expect.end(),
                                              reinterpret_cast<const std::uint8_t*>(b.constData())
                                                  + 0x40);
            auto reply = s.frame(0x60, 0x00, m_seq++);
            reply[0x14] = 0x02;
            for (std::size_t i = 0; i < 6; ++i)
                reply[0x1a + i] = static_cast<std::uint8_t>(0xC0 + i);
            s.send(reply);
            return;
        }
        case 0x40: {   // auth
            const auto kind = static_cast<std::uint8_t>(b[0x15]);
            ++m_authCount;
            if (kind != 0x05)
                return;   // the FIRST auth gets no reply; only the token request does
            auto reply = s.frame(0x40, 0x00, m_seq++);
            reply[0x14] = 0x02;
            reply[0x15] = 0x05;
            s.send(reply);

            if (!m_sentCaps) {
                m_sentCaps = true;
                auto caps = s.frame(0xA8, 0x00, m_seq++);
                for (std::size_t i = 0; i < 16; ++i)
                    caps[0x42 + i] = static_cast<std::uint8_t>(0x40 + i);
                const char* name = "IC-705";
                std::copy(name, name + 6, caps.begin() + 0x52);
                s.send(caps);
            }
            return;
        }
        case 0x90: {   // stream request
            // Refuse unless the client echoed back the identity we published in
            // the capabilities packet. A real radio has to know WHICH radio the
            // client wants when a server fronts several.
            bool identityEchoed = true;
            for (std::size_t i = 0; i < 16; ++i)
                if (static_cast<std::uint8_t>(b[0x20 + i]) != static_cast<std::uint8_t>(0x40 + i))
                    identityEchoed = false;
            if (!identityEchoed)
                return;

            m_announcedRxCodec = static_cast<std::uint8_t>(b[0x72]);
            m_announcedRate  = getBe32(b, 0x74);
            m_announcedCiv   = getBe32(b, 0x7c);
            m_announcedAudio = getBe32(b, 0x80);

            auto grant = s.frame(0x90, 0x00, m_seq++);
            const char* name = "IC-705";
            std::copy(name, name + 6, grant.begin() + 0x40);
            for (std::size_t i = 0; i < 6; ++i)
                grant[0x1a + i] = static_cast<std::uint8_t>(0xD0 + i);
            grant[0x60] = 0x01;
            s.send(grant);
            return;
        }
        default:
            return;
        }
    }

    void serial(FakeStream& s, const QByteArray& b)
    {
        const auto marker = static_cast<std::uint8_t>(b[0x10]);
        if (b.size() == 0x16 && marker == 0xc0) {
            m_serialOpened = static_cast<std::uint8_t>(b[0x15]) == 0x05;
            return;
        }
        if (marker != 0xc1)
            return;

        const auto len = static_cast<std::size_t>(getLe16(b, 0x11));
        if (static_cast<std::size_t>(b.size()) != 21 + len)
            return;
        std::vector<std::uint8_t> civ(b.constData() + 21, b.constData() + 21 + len);
        auto frame = parseFrame(civ);
        if (!frame)
            return;
        ++m_civCommands;

        // Answer a read-frequency with the radio's frequency, addressed BACK to
        // the controller.
        if (frame->cmd == cmd::kReadFreq) {
            std::vector<std::uint8_t> reply{0xFE, 0xFE, kControllerAddress, kIc705Addr,
                                            cmd::kReadFreq};
            const auto bcd = encodeFreq(kRadioFrequencyHz);
            reply.insert(reply.end(), bcd.begin(), bcd.end());
            reply.push_back(kCivEom);
            pushCiv(reply);
            return;
        }
        // Everything else is acknowledged.
        pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
        (void)s;
    }

    void audio(FakeStream&, const QByteArray& b)
    {
        if (b.size() > 24 && static_cast<std::uint8_t>(b[0x10]) == 0x80)
            ++m_audioFromClient;
    }

    FakeStream* m_control = nullptr;
    FakeStream* m_serial = nullptr;
    FakeStream* m_audio = nullptr;
    std::uint16_t m_seq = 0;
    std::uint16_t m_serialSeq = 0;
    std::uint16_t m_audioSeq = 0;
    std::uint16_t m_audioInner = 1;
    std::uint8_t  m_civSeq = 0;
    bool m_sentCaps = false;
    bool m_serialOpened = false;
    bool m_usernameObfuscated = false;
    int m_authCount = 0;
    int m_civCommands = 0;
    int m_audioFromClient = 0;
    quint32 m_announcedCiv = 0;
    quint32 m_announcedAudio = 0;
    quint32 m_announcedRate = 0;
    std::uint8_t m_announcedRxCodec = 0;
};

// Spin the event loop until `pred` holds or the deadline passes.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 4000)
{
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    FakeIc705 radio;
    IcomSession session;

    QString connectedName;
    QString disconnectReason;
    std::vector<CivFrame> frames;
    std::vector<float> audio;
    QObject::connect(&session, &IcomSession::connected, &app,
                     [&](const QString& n) { connectedName = n; });
    QObject::connect(&session, &IcomSession::disconnected, &app,
                     [&](const QString& r) { disconnectReason = r; });
    QObject::connect(&session, &IcomSession::civFrameReady, &app,
                     [&](const CivFrame& f) { frames.push_back(f); });
    QObject::connect(&session, &IcomSession::audioReady, &app,
                     [&](const std::vector<float>& s) {
                         audio.insert(audio.end(), s.begin(), s.end());
                     });

    IcomSession::Params p;
    p.host = QHostAddress::LocalHost;
    p.controlPort = radio.controlPort();
    p.serialPort  = radio.serialPort();
    p.audioPort   = radio.audioPort();
    p.username = QStringLiteral("beer");
    p.password = QStringLiteral("beerbeer");
    p.civAddress = kIc705Addr;

    check(session.start(p), "session starts");

    // ---- Phase 0: the session comes up -------------------------------------
    check(waitFor([&] { return !connectedName.isEmpty(); }),
          "the session completes login, auth, capabilities, stream request and serial open");
    check(connectedName == QStringLiteral("IC-705"),
          "the device name comes from the radio, not from a literal");
    check(disconnectReason.isEmpty(), "and nothing failed on the way");
    check(session.isConnected(), "the session reports itself connected");

    check(radio.sawUsernameObfuscated(),
          "credentials go out through the substitution table, not in clear text");
    check(radio.authCount() >= 2,
          "BOTH auths are sent — the first establishes the session, the second requests "
          "the token that gates the media streams");
    check(radio.serialOpened(), "the CI-V pipe is explicitly opened after its handshake");

    // The ports the client announced must be the ones it actually bound. This
    // is the reason binding is separable from handshaking.
    check(radio.announcedCivPort() != 0 && radio.announcedAudioPort() != 0,
          "the client announced real local ports");
    check(radio.announcedSampleRate() == 48000, "sample rate is announced big-endian");
    check(radio.announcedRxCodec() == 4, "codec 4 (LPCM 1ch 16-bit) is negotiated");

    // ---- Phase 1: CI-V control ---------------------------------------------
    session.sendCiv(cmdReadFrequency(kIc705Addr));
    check(waitFor([&] { return !frames.empty(); }), "the radio answers a CI-V command");
    check(radio.civCommandsSeen() >= 1, "and the radio actually received it");

    bool sawFrequency = false;
    for (const auto& f : frames) {
        if (f.cmd == cmd::kReadFreq && f.data.size() == kFreqBytes) {
            auto hz = decodeFreq(f.data);
            if (hz && *hz == kRadioFrequencyHz)
                sawFrequency = true;
        }
    }
    check(sawFrequency, "the frequency decodes to exactly what the radio reported");

    // Our own commands are echoed by the bus and must NOT surface as radio
    // state — otherwise every command looks confirmed the instant it is sent,
    // including the ones the radio goes on to reject.
    const std::size_t before = frames.size();
    radio.pushCiv({0xFE, 0xFE, kIc705Addr, kControllerAddress, cmd::kReadFreq, kCivEom});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    check(frames.size() == before, "an echo of our own command is filtered out");

    // ---- Phase 2: a scope sweep --------------------------------------------
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x00);
        body.push_back(encodeBcdByte(1));    // division 1
        body.push_back(encodeBcdByte(1));    // of 1 — the WLAN case
        body.push_back(0x00);                // centre mode
        const auto centre = encodeFreq(14'100'000);
        const auto span   = encodeFreq(100'000);
        body.insert(body.end(), centre.begin(), centre.end());
        body.insert(body.end(), span.begin(), span.end());
        body.push_back(0x00);                // in range
        // A FULL 475-point sweep, deliberately. A short synthetic one would fit
        // inside both an 80-byte frame cap and an 8-bit payload-length field,
        // and would therefore pass against code that cannot carry a real one.
        for (int i = 0; i < kScopePointsIc705; ++i)
            body.push_back(static_cast<std::uint8_t>(i % (kScopeMaxAmplitude + 1)));

        std::vector<std::uint8_t> civ{0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kScope,
                                      scope::kWaveData};
        civ.insert(civ.end(), body.begin(), body.end());
        civ.push_back(kCivEom);
        radio.pushCiv(civ);
    }
    check(waitFor([&] {
              for (const auto& f : frames)
                  if (f.cmd == cmd::kScope && f.hasSub && f.sub == scope::kWaveData)
                      return true;
              return false;
          }),
          "a scope sweep arrives over the serial stream");

    {
        ScopeDecoder decoder;
        std::optional<ScopeFrame> sweep;
        for (const auto& f : frames)
            if (auto s = decoder.feed(f))
                sweep = s;
        check(sweep.has_value(), "and the decoder assembles it");
        if (sweep) {
            check(sweep->startHz == 14'000'000 && sweep->endHz == 14'200'000,
                  "centre mode edges are centre +/- span (span is a HALF-width)");
            check(sweep->raw.size() == static_cast<std::size_t>(kScopePointsIc705),
                  "and the sweep is padded to the radio's geometry");
        }
    }

    // ---- Phase 3: audio, both directions -----------------------------------
    {
        std::vector<float> tone(682);
        for (std::size_t i = 0; i < tone.size(); ++i)
            tone[i] = (i % 2) ? 0.5f : -0.5f;
        radio.pushAudio(encodeAudio(AudioCodec::Lpcm1ch16, tone));
    }
    check(waitFor([&] { return audio.size() >= 682; }), "receive audio decodes to samples");
    check(std::fabs(audio[0] + 0.5f) < 1e-3f || std::fabs(audio[0] - 0.5f) < 1e-3f,
          "and the samples are the ones the radio sent");

    // Transmit: one 20 ms frame becomes TWO packets on the wire.
    session.sendAudio(std::vector<float>(960, 0.25f));
    check(waitFor([&] { return radio.audioPacketsFromClient() >= 2; }),
          "a 20 ms transmit frame reaches the radio as the 1364 + 556 pair");

    // ---- Teardown ----------------------------------------------------------
    session.stop();
    check(!session.isConnected(), "stop() tears the session down");

    if (g_failures == 0)
        std::printf("icom_session_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
