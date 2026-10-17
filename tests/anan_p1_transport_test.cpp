#include "core/backends/anan/AnanClient.h"

#include <QCoreApplication>
#include <QTimer>
#include <cstdlib>
#include <iostream>

using namespace AetherSDR::anan;
namespace AetherSDR::anan {
struct P1ClientTestAccess {
    static bool timeout(P1Client& client) {
        return QMetaObject::invokeMethod(client.m_timeoutTimer, "timeout", Qt::DirectConnection);
    }
    static bool prepare(P1Client& client, const P1Client::Params& params,
                        std::vector<QByteArray>& packets) {
        client.m_sendOverride = [&packets](std::span<const std::uint8_t> bytes) {
            packets.emplace_back(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
            return true;
        };
        return client.prepare(params);
    }
    static void feed(P1Client& client, std::span<const std::uint8_t> data,
                     const char* sender = "192.0.2.10", quint16 port = 1024) {
        client.handleDatagram(QHostAddress(QString::fromLatin1(sender)), port, data);
    }
    static void drain(P1Client& client, qint64 now) { client.drainAt(now); }
    static auto counters(const P1Client& client) { return client.m_counters; }
    static auto config(const P1Client& client) { return client.m_config; }
    static auto queued(const P1Client& client) { return client.m_audio.size(); }
    static void refuseWrites(P1Client& client) {
        client.m_sendOverride = [](auto) { return false; };
    }
};
struct AnanClientTestAccess {
    static P1Client* p1(AnanClient& client) { return client.m_p1; }
    static P2Client* p2(AnanClient& client) { return client.m_p2; }
    static void select(AnanClient& client, int protocol) { client.m_protocol = protocol; }
};
}
namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
auto discovery(int wireBoard, bool busy = false) {
    std::array<std::uint8_t, 60> bytes{};
    bytes[0] = 0xef; bytes[1] = 0xfe; bytes[2] = busy ? 3 : 2;
    bytes[3] = 0x88; bytes[8] = 0x6a; bytes[9] = 27; bytes[10] = wireBoard;
    return bytes;
}
p1::Packet iq(int receivers, std::uint32_t seq = 0) {
    p1::Packet bytes{};
    bytes[0] = 0xef; bytes[1] = 0xfe; bytes[2] = 1; bytes[3] = 6;
    for (int n = 0; n < 4; ++n) bytes[4 + n] = static_cast<std::uint8_t>(seq >> (24 - 8 * n));
    for (int f = 0; f < 2; ++f) {
        const int base = 8 + 512 * f;
        bytes[base] = bytes[base + 1] = bytes[base + 2] = 0x7f;
        for (int r = 0; r < 504 / (6 * receivers + 2); ++r) {
            const int offset = base + 8 + r * (6 * receivers + 2);
            bytes[offset] = 0x40; // RX1 slot zero is positive.
            if (receivers > 1) bytes[offset + 6] = 0x80; // Other slots must not feed RX1.
        }
    }
    return bytes;
}
P1Client::Params params(int normalizedBoard = -1) {
    P1Client::Params p; p.host = QStringLiteral("192.0.2.10");
    p.expectedBoardId = normalizedBoard; return p;
}
void profilesAndWire() {
    struct Case { int wire; int normalized; int count; int adcs; int filter; };
    for (const Case c : {Case{1, 1, 4, 1, 0x10}, {2, 2, 2, 1, 0x10},
                         {4, 3, 5, 2, 0x10}, {5, 4, 5, 2, 0x10},
                         {10, 5, 5, 2, 0x04}, {20, 20, 4, 1, 0x04}}) {
        std::vector<QByteArray> packets; P1Client client;
        auto p = params(c.normalized);
        check(P1ClientTestAccess::prepare(client, p, packets), "profile prepare");
        client.setDdc0FrequencyHz(6'000'000);
        int linked = 0, discovered = -1, blocks = 0;
        QObject::connect(&client, &P1Client::linkUp, [&] { ++linked; });
        QObject::connect(&client, &P1Client::discoveryInfoReceived,
            [&](quint8 board, quint8, quint8 count) { discovered = board; check(count == c.count, "physical slot count"); });
        QObject::connect(&client, &P1Client::ddc0IqReady, [&](const auto& block) {
            ++blocks; check(block.size() == static_cast<std::size_t>(2 * (504 / (6 * c.count + 2))), "IQ geometry");
            for (auto sample : block) check(sample.real() > 0.49f, "RX1 uses P1 slot zero");
        });
        P1ClientTestAccess::feed(client, iq(c.count));
        check(packets.empty() && !linked, "no IQ or control before discovery");
        P1ClientTestAccess::feed(client, discovery(c.wire), "192.0.2.99");
        P1ClientTestAccess::feed(client, discovery(c.wire), "192.0.2.10", 1035);
        check(packets.empty() && discovered == -1, "foreign discovery ignored");
        P1ClientTestAccess::feed(client, discovery(c.wire));
        check(discovered == c.normalized && packets.size() >= 6, "normalized discovery and control priming");
        const auto cfg = P1ClientTestAccess::config(client);
        check(cfg.numAdcs == c.adcs && cfg.receivers == c.count, "board-specific ADC/layout");
        check(cfg.receiveFilterBits == c.filter && cfg.adc1ReceiveFilterBits == 0x20, "HPF/BPF distinction and ADC1 bypass");
        check(cfg.lowPassBits == 2, "6 MHz selects 60/40 LPF");
        for (const auto& packet : packets) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(packet.constData());
            if (packet.size() == 64) { check(b[2] == 4 && b[3] == 1, "only IQ stream starts"); continue; }
            check(packet.size() == 1032 && b[3] == 2, "EP2 endpoint");
            for (int f = 0; f < 2; ++f) {
                const int base = 8 + 512 * f;
                check((b[base + 3] & 1) == 0, "all emitted MOX bits clear");
                for (int r = 0; r < 63; ++r)
                    for (int n = 4; n < 8; ++n) check(b[base + 8 + r * 8 + n] == 0, "TX IQ never emitted");
            }
        }
        const auto count = packets.size();
        P1ClientTestAccess::feed(client, discovery(c.wire));
        check(packets.size() == count, "duplicate discovery does not restart");
        P1ClientTestAccess::feed(client, iq(c.count, 0xffffffff));
        P1ClientTestAccess::feed(client, iq(c.count, 0));
        check(linked == 1 && blocks == 2, "sequence wrap and single link up");
        P1ClientTestAccess::feed(client, iq(c.count, 0));
        check(blocks == 2 && P1ClientTestAccess::counters(client).drops == 0, "duplicate discarded without huge loss");
        P1ClientTestAccess::feed(client, iq(c.count, 3));
        check(blocks == 3 && P1ClientTestAccess::counters(client).drops == 2, "forward gaps count missing packets");
        check(!client.setDdcRateLive(1, 96) && !client.setDdcRateLive(0, 384), "unsupported live settings send nothing");
        check(client.setDdcRateLive(0, 192), "live P1 general rate bank");
        client.stop();
        check(packets.back().size() == 64 && packets.back()[3] == 0, "stop is an IQ-only stop command");
    }
}
void refusals() {
    for (int wire : {0, 3, 6, 11, 99}) {
        std::vector<QByteArray> packets; P1Client client;
        check(P1ClientTestAccess::prepare(client, params(), packets), "manual prepare");
        int errors = 0; QObject::connect(&client, &P1Client::connectionError, [&](const auto&) { ++errors; });
        P1ClientTestAccess::feed(client, discovery(wire));
        check(!client.isRunning() && errors == 1 && packets.empty(), "unsupported boards fail before controls");
    }
    for (bool busy : {false, true}) {
        std::vector<QByteArray> packets; P1Client client;
        check(P1ClientTestAccess::prepare(client, params(busy ? 5 : 3), packets), "selected prepare");
        P1ClientTestAccess::feed(client, discovery(10, busy));
        check(!client.isRunning() && packets.empty(), "wrong selection or busy radio cannot be taken over");
    }
    std::vector<QByteArray> packets; P1Client client;
    auto p = params(1); p.ddc0AdcIndex = 1;
    check(!P1ClientTestAccess::prepare(client, p, packets), "single ADC profile rejects ADC1");
    p = params(5); p.ddc0RateKsps = 768;
    check(!P1ClientTestAccess::prepare(client, p, packets), "P2-only rate rejected");
    p = params(); p.ddc0AdcIndex = 1;
    check(P1ClientTestAccess::prepare(client, p, packets), "unknown address waits for actual ADC count");
    P1ClientTestAccess::feed(client, discovery(1));
    check(!client.isRunning() && packets.empty(), "discovered one-ADC target rejects ADC1");
}
void pacingAndErrors() {
    std::vector<QByteArray> packets; P1Client client;
    auto p = params(5); p.speakerAudioEnabled = true;
    check(P1ClientTestAccess::prepare(client, p, packets), "speaker prepare");
    P1ClientTestAccess::feed(client, discovery(10)); packets.clear();
    std::array<std::int16_t, 4096> audio{};
    audio[0] = 0x1234; audio[1] = -2;
    client.enqueueSpeakerAudio(QByteArray(reinterpret_cast<const char*>(audio.data()), sizeof(audio)));
    P1ClientTestAccess::drain(client, 5'000'000);
    check(packets.size() == 1 && static_cast<unsigned char>(packets[0][16]) == 0x12,
          "P1 audio/control paced at 48 kHz");
    P1ClientTestAccess::drain(client, 10'000'000);
    check(packets.size() == 3, "fractional credit survives ticks");
    P1ClientTestAccess::drain(client, 1'000'000'000);
    check(packets.size() == 7, "long stall has bounded catch-up");
    client.enqueueSpeakerAudio(QByteArray(100000, '\0'));
    check(P1ClientTestAccess::queued(client) == 8192, "audio queue bounded");
    const auto before = P1ClientTestAccess::counters(client).txBytes;
    P1ClientTestAccess::refuseWrites(client);
    P1ClientTestAccess::drain(client, 1'010'000'000);
    check(!client.isRunning() && P1ClientTestAccess::counters(client).txBytes == before,
          "refused sends fail session without inflating bytes");
}
void dispatch() {
    AnanClient client;
    int up = 0; QObject::connect(&client, &AnanClient::linkUp, [&] { ++up; });
    int iqBlocks = 0;
    QObject::connect(&client, &AnanClient::ddc0IqReady, [&](const auto& block) {
        ++iqBlocks; check(block.size() == 1 && block.front().real() == 0.25f, "IQ forwarded unchanged");
    });
    auto* p1 = AnanClientTestAccess::p1(client); auto* p2 = AnanClientTestAccess::p2(client);
    check(p1->parent() == &client && p2->parent() == &client, "both transports follow facade affinity");
    emit p1->linkUp(); emit p2->linkUp(); check(up == 1, "P2 default ignores P1");
    const std::vector<std::complex<float>> block{{0.25f, -0.25f}};
    emit p1->ddc0IqReady(block); emit p2->ddc0IqReady(block);
    check(iqBlocks == 1, "P2 IQ path remains selected by default");
    AnanClientTestAccess::select(client, 1);
    emit p2->linkUp(); emit p1->linkUp(); check(up == 2, "P1 ignores inactive P2");
    emit p2->ddc0IqReady(block); emit p1->ddc0IqReady(block);
    check(iqBlocks == 2, "P1 IQ path suppresses inactive P2");
}
void redPitaya384() {
    for (const int wire : {1, 10}) {
        std::vector<QByteArray> packets; P1Client client;
        auto p = params(wire == 1 ? 1 : 5);
        p.redPitayaP1 = true; p.ddc0RateKsps = 384; p.ddc0AdcIndex = 1;
        check(P1ClientTestAccess::prepare(client, p, packets), "explicit Red Pitaya accepts 384 ksps and ADC1");
        client.setDdc0FrequencyHz(6'000'000);
        P1ClientTestAccess::feed(client, discovery(wire));
        const auto c = P1ClientTestAccess::config(client);
        check(client.isRunning() && c.rateKsps == 384 && c.receivers == 5 && c.numAdcs == 2
            && c.adcIndex == 1 && c.receiveFilterBits == 0x10,
            "Red Pitaya model retains five-slot dual-ADC legacy-filter layout");
        check(p1::receiveControl(0, c)->at(1) == 3, "384 ksps uses P1 rate code 3");
        check(client.setDdcRateLive(0, 384), "Red Pitaya live zoom accepts 384 ksps");
        check(!client.setDdcRateLive(0, 768), "Red Pitaya P1 cannot use P2-only rate");
    }
    std::vector<QByteArray> packets; P1Client client;
    auto p = params(5); p.ddc0RateKsps = 384;
    check(!P1ClientTestAccess::prepare(client, p, packets), "shared ID alone never enables Red Pitaya 384 ksps");
    p = params(3); p.redPitayaP1 = true;
    check(!P1ClientTestAccess::prepare(client, p, packets), "incompatible board cannot inherit Red Pitaya override");
}
void logicalModels() {
    for (const auto& entry : kAnanModels) {
        if (entry.model == AnanModel::Automatic) { continue; }
        std::vector<QByteArray> packets; P1Client client;
        auto p = params(entry.nativeBoardId); p.model = entry.model;
        const bool expected = entry.nativeBoardId == 1 || entry.nativeBoardId == 2
            || entry.nativeBoardId == 3 || entry.nativeBoardId == 4
            || entry.nativeBoardId == 5 || entry.nativeBoardId == 20;
        check(P1ClientTestAccess::prepare(client, p, packets) == expected,
              "only source-backed P1 identities can start");
        if (!expected) { continue; }
        const int wire = entry.nativeBoardId == 3 ? 4 : entry.nativeBoardId == 4 ? 5
            : entry.nativeBoardId == 5 ? 10 : entry.nativeBoardId;
        P1ClientTestAccess::feed(client, discovery(wire));
        check(client.isRunning(), "selected P1 model validates against native wire ID");
        if (entry.model == AnanModel::AnvelinaPro3) {
            bool extraBank = false;
            for (const auto& packet : packets) {
                if (packet.size() != 1032) { continue; }
                for (const int base : {8, 520}) {
                    if (static_cast<unsigned char>(packet[base + 3]) == 0x26) {
                        extraBank = true;
                        for (int n = 4; n <= 7; ++n) { check(packet[base + n] == 0, "Anvelina extra OC bits stay zero"); }
                    }
                }
            }
            check(extraBank, "Anvelina-specific RX control bank is included");
        }
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    profilesAndWire(); refusals(); pacingAndErrors(); dispatch(); redPitaya384(); logicalModels();
    for (const bool discoveryAnswered : {false, true}) {
        std::vector<QByteArray> packets; P1Client client; QString reason;
        QObject::connect(&client, &P1Client::connectionError, [&](const QString& error) { reason = error; });
        check(P1ClientTestAccess::prepare(client, params(1), packets), "P1 timeout fixture");
        if (discoveryAnswered) { P1ClientTestAccess::feed(client, discovery(1)); }
        check(P1ClientTestAccess::timeout(client), "production timeout callback invoked without waiting");
        check(reason.contains(discoveryAnswered ? QStringLiteral("P1 IQ") : QStringLiteral("P1 Discovery")),
              "P1 timeout distinguishes missing discovery from absent IQ after discovery");
    }
    //DH1KLM: Exercise the production P1 profile/priming path for the same
    // Orion MkII + ANAN-200D DIY override as P2, with no socket or fake radio.
    {
        std::vector<QByteArray> packets; P1Client client;
        auto p = params(5); p.model = AnanModel::Anan200D;
        check(!P1ClientTestAccess::prepare(client, p, packets), "mismatched P1 model refused without override");
        p.overrideDetection = true;
        check(P1ClientTestAccess::prepare(client, p, packets), "P1 explicit cross-board override prepared");
        client.setDdc0FrequencyHz(6'000'000);
        P1ClientTestAccess::feed(client, discovery(10));
        const auto cfg = P1ClientTestAccess::config(client);
        check(!packets.empty() && cfg.numAdcs == 2 && cfg.receivers == 5
              && cfg.receiveFilterBits == 0x10,
              "P1 Orion MkII reply primes ANAN-200D legacy HPF hardware");
    }
    for (const bool busy : {false, true}) {
        std::vector<QByteArray> packets; P1Client client;
        auto p = params(5); p.model = AnanModel::Anan200D; p.overrideDetection = true;
        check(P1ClientTestAccess::prepare(client, p, packets), "P1 refusal override fixture");
        auto packet = discovery(busy ? 10 : 5);
        if (busy) { packet[2] = 3; }
        P1ClientTestAccess::feed(client, packet);
        check(packets.empty() && !client.isRunning(), "P1 override cannot bypass busy or different discovery identity");
    }
    std::cout << "ANAN P1 transport checks passed\n";
}
