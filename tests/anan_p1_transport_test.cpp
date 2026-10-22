#include "core/backends/anan/AnanClient.h"
#include "core/backends/anan/AnanFilterOptions.h"

#include <QStringList>
#include <QLoggingCategory>
#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace AetherSDR::anan;
namespace {
QStringList sessionLog;
void captureSessionLog(QtMsgType, const QMessageLogContext& context, const QString& message)
{
    if (QString::fromLatin1(context.category ? context.category : "") == QLatin1String("aether.anan.p1")) {
        sessionLog.append(message);
    }
}
}

namespace AetherSDR::anan {
struct P1ClientTestAccess {
    static bool timeout(P1Client& client) {
        return QMetaObject::invokeMethod(client.m_timeoutTimer, "timeout", Qt::DirectConnection);
    }
    static bool prepare(P1Client& client, const P1Client::Params& params,
                        std::vector<QByteArray>& packets) {
        acceptWrites(client, packets);
        return client.prepare(params);
    }
    static void acceptWrites(P1Client& client, std::vector<QByteArray>& packets) {
        client.m_sendOverride = [&packets](std::span<const std::uint8_t> bytes) {
            packets.emplace_back(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
            return true;
        };
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
        check(cfg.receiveFilterBits == c.filter && cfg.adc1ReceiveFilterBits == (c.adcs == 2 ? 0x20 : 0), "HPF/BPF distinction; ADC1 bypass only for a fitted ADC1");
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
        const int used = std::min(4, c.count);
        std::vector<DdcConfig> streams(used, {48, 0});
        std::vector<double> centers{14'175'000, 7'100'000, 21'250'000, 28'500'000};
        centers.resize(used);
        check(client.setReceiveStreams(streams, centers),
              "P1 independent receive slots fit the fixed frame geometry");
        const auto multi = P1ClientTestAccess::config(client);
        check(multi.frequencyHz == 14'175'000 && multi.receiveFrequencies[1] == 7'100'000,
              "P1 transport preserves distinct per-slot frequencies");
        auto invalid = centers;
        invalid[1] = std::numeric_limits<double>::quiet_NaN();
        check(!client.setReceiveStreams(streams, invalid) &&
                  P1ClientTestAccess::config(client).receiveFrequencies == multi.receiveFrequencies,
              "invalid P1 frequency leaves every current slot unchanged");
        int slotDeliveries = 0;
        QObject::connect(&client, &P1Client::ddcIqReady, [&](int index, const auto& block) {
            check(index >= 0 && index < c.count && !block.empty(),
                  "P1 IQ deliveries have distinct slot indices");
            ++slotDeliveries;
            if (index == 1)
                check(block.front().real() < -0.99f, "second slot has its own IQ samples");
        });
        P1ClientTestAccess::feed(client, iq(c.count, 0xffffffff));
        P1ClientTestAccess::feed(client, iq(c.count, 0));
        check(linked == 1 && blocks == 2, "sequence wrap and single link up");
        P1ClientTestAccess::feed(client, iq(c.count, 0));
        check(blocks == 2 && P1ClientTestAccess::counters(client).drops == 0, "duplicate discarded without huge loss");
        P1ClientTestAccess::feed(client, iq(c.count, 3));
        check(blocks == 3 && P1ClientTestAccess::counters(client).drops == 2, "forward gaps count missing packets");
        check(slotDeliveries == 3 * c.count,
              "each accepted P1 frame delivers all slots exactly once");
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
void liveRateWriteFailure() {
    for (const auto& entry : kAnanModels) {
        if (entry.model == AnanModel::Automatic
            || !P1Client::profile(entry.nativeBoardId, entry.model == AnanModel::RedPitaya)) { continue; }
        std::vector<QByteArray> packets;
        P1Client client;
        auto p = params(entry.nativeBoardId);
        p.model = entry.model;
        p.rxAntenna = 3;
        p.adc0AttenuationDb = 17;
        p.adc1AttenuationDb = 9;
        auto filters = defaultFilterConfig(false);
        filters.rxOnlyInput = 2;
        filters.ocEnabled = true;
        filters.ocReceive[0] = {14000000, 14350000, 5};
        p.filters = filters;
        check(P1ClientTestAccess::prepare(client, p, packets), "P1 rate failure model prepared");
        client.setDdc0FrequencyHz(14200000);
        const int wire = entry.nativeBoardId == 3 ? 4 : entry.nativeBoardId == 4 ? 5
            : entry.nativeBoardId == 5 ? 10 : entry.nativeBoardId;
        P1ClientTestAccess::feed(client, discovery(wire));
        check(client.isRunning(), "P1 rate failure model validated");
        const int maximum = entry.model == AnanModel::RedPitaya ? 384 : 192;
        qint64 now = 0;
        for (const int rate : {96, 192, 48, 384}) {
            if (rate > maximum) { continue; }
            const auto previous = P1ClientTestAccess::config(client);
            const auto before = P1ClientTestAccess::counters(client);
            const auto count = packets.size();
            P1ClientTestAccess::refuseWrites(client);
            check(!client.setDdcRateLive(0, rate), "P1 refused rate packet reports failure");
            check(client.isRunning() && packets.size() == count
                && P1ClientTestAccess::counters(client).txBytes == before.txBytes,
                "P1 refused rate packet neither ends session nor counts sent bytes");
            check(P1ClientTestAccess::config(client).rateKsps == previous.rateKsps,
                "P1 refused rate packet preserves the previous live rate");
            P1ClientTestAccess::acceptWrites(client, packets);
            //DH1KLM: The ordinary control scheduler must keep the old rate after a refused write.
            P1ClientTestAccess::drain(client, now += 1000000000);
            P1ClientTestAccess::drain(client, now += 1000000000);
            bool foundGeneral = false;
            for (std::size_t n = count; n < packets.size(); ++n) {
                for (const int base : {8, 520}) {
                    if (packets[n][base + 3] != 0) { continue; }
                    foundGeneral = true;
                    check((static_cast<unsigned char>(packets[n][base + 4]) & 3)
                            == (previous.rateKsps == 48 ? 0 : previous.rateKsps == 96 ? 1
                                : previous.rateKsps == 192 ? 2 : 3),
                        "P1 periodic general bank retains old rate after failure");
                }
            }
            check(foundGeneral, "P1 periodic scheduler revisits the general bank");
            const auto sentBefore = P1ClientTestAccess::counters(client).txBytes;
            check(client.setDdcRateLive(0, rate), "P1 rate retry succeeds");
            check(P1ClientTestAccess::config(client).rateKsps == rate
                && P1ClientTestAccess::counters(client).txBytes == sentBefore + 1032,
                "P1 accepted rate packet commits rate and counts exactly one packet");
            const auto& packet = packets.back();
            const int code = rate == 48 ? 0 : rate == 96 ? 1 : rate == 192 ? 2 : 3;
            check((static_cast<unsigned char>(packet[12]) & 3) == code
                && (static_cast<unsigned char>(packet[524]) & 3) == code,
                "P1 rate retry writes the requested code in both general frames");
            const auto retryCount = packets.size();
            check(client.setDdcRateLive(0, rate) && packets.size() == retryCount + 1,
                "P1 repeated accepted rate still sends the UDP recovery packet");
            auto expected = previous;
            expected.rateKsps = rate;
            for (const int address : {0, 4, 0x12, 0x14, 0x16, 0x1c, 0x1e, 0x24, 0x26}) {
                check(p1::receiveControl(address, P1ClientTestAccess::config(client))
                        == p1::receiveControl(address, expected),
                    "P1 rate retry preserves antenna, RX input, filters, OC and tuning");
            }
            const auto acceptedCount = packets.size();
            for (const int invalid : {-1, 0, 47, 49, 768}) {
                check(!client.setDdcRateLive(0, invalid), "P1 invalid rate refused");
            }
            check(!client.setDdcRateLive(-1, rate) && !client.setDdcRateLive(1, rate)
                && packets.size() == acceptedCount
                && P1ClientTestAccess::config(client).rateKsps == rate,
                "P1 invalid live requests cannot send or alter the accepted rate");
            if (maximum == 192) {
                check(!client.setDdcRateLive(0, 384) && packets.size() == acceptedCount,
                    "P1 shared board identity never enables Red Pitaya's extra rate");
            }
        }
    }
}
void adc1RoutingAndFilters() {
    constexpr std::array<unsigned, 6> p1Bits{0x10,0x08,0x04,0x01,0x02,0x40};
    for (const auto& entry : kAnanModels) {
        const auto profile = P1Client::profile(entry.nativeBoardId, entry.model == AnanModel::RedPitaya);
        if (!profile || entry.model == AnanModel::Automatic) { continue; }
        for (int adc = 0; adc < profile->numAdcs; ++adc) {
            std::vector<QByteArray> packets;
            P1Client client;
            auto p = params(entry.nativeBoardId);
            p.model = entry.model;
            p.ddc0AdcIndex = adc;
            check(P1ClientTestAccess::prepare(client, p, packets), "P1 ADC route fixture prepared");
            const int wire = entry.nativeBoardId == 3 ? 4 : entry.nativeBoardId == 4 ? 5
                : entry.nativeBoardId == 5 ? 10 : entry.nativeBoardId;
            P1ClientTestAccess::feed(client, discovery(wire));
            const auto initial = P1ClientTestAccess::config(client);
            check(p1::receiveControl(0x1c, initial)
                      == p1::Control{0x1c, static_cast<std::uint8_t>(adc ? 0x55 : 0),
                                     static_cast<std::uint8_t>(adc ? 0x01 : 0), 0, 0},
                  "P1 ADC1 routes all five wire slots without changing RX slot zero");
            bool routeSent = false;
            for (const auto& packet : packets) {
                if (packet.size() != 1032) { continue; }
                for (const int base : {8,520}) {
                    if (static_cast<unsigned char>(packet[base + 3]) != 0x1c) { continue; }
                    routeSent = true;
                    check(static_cast<unsigned char>(packet[base + 4]) == (adc ? 0x55 : 0)
                          && static_cast<unsigned char>(packet[base + 5]) == (adc ? 1 : 0),
                          "P1 startup emits the selected ADC route in the actual control packet");
                }
            }
            check(routeSent, "P1 startup primes the ADC routing bank");
            int delivered = 0;
            QObject::connect(&client, &P1Client::ddc0IqReady, [&](const auto& block) {
                ++delivered;
                for (const auto sample : block) { check(sample.real() > 0.49f, "ADC1 still decodes P1 RX slot zero"); }
            });
            P1ClientTestAccess::feed(client, iq(profile->frameSlotCount));
            check(delivered == 1, "selected ADC delivers one IQ block");
            auto filters = defaultFilterConfig(profile->filterKind == ReceiveFilterKind::MkIIBpf);
            filters.receiveAdc1 = std::array<FilterBand, 6>{{
                {0,999999}, {1000000,2999999}, {3000000,7999999},
                {8000000,15999999}, {16000000,31999999}, {32000000,61440000}}};
            filters.bypassAdc0 = true;
            filters.receiveBypass.fill(true);
            filters.disable6mLnaOnRx = true;
            for (const bool bypass : {false, true}) {
                filters.bypassAdc1 = bypass;
                client.setHardwareFilters(filterOptions(filters));
                for (std::size_t row = 0; row < p1Bits.size(); ++row) {
                    const auto band = (*filters.receiveAdc1)[row];
                    for (const auto hz : {band.lowHz, (band.lowHz + band.highHz) / 2, band.highHz}) {
                        client.setDdc0FrequencyHz(hz);
                        const auto config = P1ClientTestAccess::config(client);
                        check(config.adc1ReceiveFilterBits == (profile->numAdcs == 1 ? 0
                                  : bypass ? 0x20 : p1Bits[row]),
                              "P1 ADC1 exact independent range-edge and bypass bits");
                        check(config.receiveFilterBits == 0x20 && config.adcIndex == adc,
                              "P1 independent ADC1 filters preserve ADC0 bypass and source routing");
                    }
                }
            }
            client.setFrontEndAttenuation(0, 20, 30);
            const int expectedAlex = profile->filterKind == ReceiveFilterKind::LegacyHpf ? 30 : 0;
            check(P1ClientTestAccess::config(client).alexAttenuationDb == expectedAlex,
                  "combined P1 setter preserves only classic ALEX attenuation");
            client.setFrontEndAttenuation(1, 9, 0);
            check(P1ClientTestAccess::config(client).alexAttenuationDb == expectedAlex,
                  "P1 ADC1 edit leaves ADC0 ALEX relays unchanged");
            packets.clear();
            P1ClientTestAccess::drain(client, 0);
            P1ClientTestAccess::drain(client, 1000000000);
            P1ClientTestAccess::drain(client, 2000000000);
            bool bankZero = false;
            for (const auto& packet : packets) {
                for (const int base : {8, 520}) {
                    if (static_cast<unsigned char>(packet[base + 3]) != 0) { continue; }
                    bankZero = true;
                    const int expected = expectedAlex / 10;
                    check((static_cast<unsigned char>(packet[base + 6]) & 3) == expected,
                          "live P1 control packets carry the exact classic ATT relays");
                }
            }
            check(bankZero, "P1 scheduler transmits the ATT relay bank");
            client.setFrontEndAttenuation(0, 0, 0);
            qint64 now = 2000000000;
            for (const int value : {std::numeric_limits<int>::min(), -1, 0, 17, 31, 32,
                                     std::numeric_limits<int>::max()}) {
                for (int target = 0; target < profile->numAdcs; ++target) {
                    const auto previous = P1ClientTestAccess::config(client);
                    client.setStepAttenuationDb(target, value);
                    const auto updated = P1ClientTestAccess::config(client);
                    const int expected0 = target == 0 ? std::clamp(value, 0, 31) : previous.adc0AttenuationDb;
                    const int expected1 = target == 1 ? std::clamp(value, 0, 31) : previous.adc1AttenuationDb;
                    check(updated.adc0AttenuationDb == expected0 && updated.adc1AttenuationDb == expected1
                          && updated.adcIndex == adc && updated.frequencyHz == previous.frequencyHz,
                          "P1 attenuation targets only the selected ADC and preserves source/tuning");
                    packets.clear();
                    P1ClientTestAccess::drain(client, now += 1000000000);
                    P1ClientTestAccess::drain(client, now += 1000000000);
                    bool found0 = false, found1 = false;
                    for (const auto& packet : packets) {
                        for (const int base : {8,520}) {
                            const auto address = static_cast<unsigned char>(packet[base + 3]);
                            if (address == 0x14) {
                                found0 = true;
                                check(static_cast<unsigned char>(packet[base + 7]) == (0x20 | expected0),
                                      "P1 live ADC0 attenuation reaches its actual control-bank byte");
                            } else if (address == 0x16) {
                                found1 = true;
                                check(static_cast<unsigned char>(packet[base + 4])
                                          == (profile->numAdcs == 2 ? 0x20 | expected1 : 0),
                                      "P1 live ADC1 attenuation reaches only fitted hardware");
                            }
                        }
                    }
                    check(found0 && found1, "P1 scheduler refreshes both attenuation banks");
                }
            }
        }
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
    const auto previousHandler = qInstallMessageHandler(captureSessionLog);
    profilesAndWire(); refusals(); pacingAndErrors(); dispatch(); redPitaya384(); logicalModels();
    liveRateWriteFailure();
    adc1RoutingAndFilters();
    for (const bool discoveryAnswered : {false, true}) {
        std::vector<QByteArray> packets; P1Client client; QString reason;
        QObject::connect(&client, &P1Client::connectionError, [&](const QString& error) { reason = error; });
        check(P1ClientTestAccess::prepare(client, params(1), packets), "P1 timeout fixture");
        if (discoveryAnswered) { P1ClientTestAccess::feed(client, discovery(1)); }
        check(P1ClientTestAccess::timeout(client), "production timeout callback invoked without waiting");
        check(reason.contains(discoveryAnswered ? QStringLiteral("P1 IQ") : QStringLiteral("P1 Discovery")),
              "P1 timeout distinguishes missing discovery from absent IQ after discovery");
    }
    {
        std::vector<QByteArray> packets;
        P1Client client;
        auto p = params(5);
        p.model = AnanModel::Anan200D;
        p.overrideDetection = true;
        p.ddc0RateKsps = 48;
        p.filters = defaultFilterConfig(false);
        check(P1ClientTestAccess::prepare(client, p, packets), "P1 multi-band fixture");
        P1ClientTestAccess::feed(client, discovery(10));
        const std::array<ReceiveFilterDemand, 2> demands{{{0,7100000},{0,14200000}}};
        client.setReceiveFilterPlan(planReceiveFilters(*p.filters, demands));
        check(client.setReceiveStreams({{48,0},{48,0}}, {7100000,14200000}), "P1 two live filter demands");
        auto cfg = P1ClientTestAccess::config(client);
        check(cfg.receiveFilterBits == 0x20 && cfg.lowPassBits == 1,
              "P1 override emits receive bypass and highest-band 20m LPF");
        client.setReceiveFilterPlan(planReceiveFilters(*p.filters, std::span(demands).first(1)));
        check(client.setReceiveStreams({{48,0}}, {7100000}), "P1 retire high-band stream");
        cfg = P1ClientTestAccess::config(client);
        check(cfg.receiveFilterBits == 0x08 && cfg.lowPassBits == 2,
              "P1 restores 40m HPF/LPF after the high-band receiver closes");
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
    {
        //DH1KLM: A live hardware-page edit reaches the P1 bank-local encoder through dispatch.
        std::vector<QByteArray> packets;
        AnanClient dispatcher;
        AnanClientTestAccess::select(dispatcher, 1);
        P1Client& client = *AnanClientTestAccess::p1(dispatcher);
        check(P1ClientTestAccess::prepare(client, params(5), packets), "live P1 filter fixture");
        client.setDdc0FrequencyHz(2300000);
        P1ClientTestAccess::feed(client, discovery(10));
        check(P1ClientTestAccess::config(client).receiveFilterBits == 0x08, "P1 initial 80 m BPF");
        auto config = defaultFilterConfig(true);
        config.receive[0].highHz = 2499999;
        config.receive[1].lowHz = 2500000;
        config.bypassAdc1 = false;
        dispatcher.setHardwareFilters(filterOptions(config));
        const auto applied = P1ClientTestAccess::config(client);
        check(applied.receiveFilterBits == 0x10 && applied.adc1ReceiveFilterBits == 0x10,
            "P1 live edited range and ADC1 auto selection update both banks");
        config.receiveAdc1 = defaultFilterConfig(true).receive;
        dispatcher.setHardwareFilters(filterOptions(config));
        const auto independent = P1ClientTestAccess::config(client);
        check(independent.receiveFilterBits == 0x10 && independent.adc1ReceiveFilterBits == 0x08,
            "P1 dispatcher applies separate ADC1 limits without retuning ADC0");
        config.rxOnlyInput = 2;
        dispatcher.setReceiveHardware(filterOptions(config), 3);
        const auto routed = P1ClientTestAccess::config(client);
        check(routed.antenna == 3, "P1 dispatcher changes the bank and RX-only input together");
        dispatcher.setReceiveHardware(filterOptions(config), 0);
        auto malformed = filterOptions(config);
        malformed.insert(QStringLiteral("rxOnlyInput"), 7);
        dispatcher.setReceiveHardware(malformed, 1);
        check(P1ClientTestAccess::config(client).antenna == 3
              && P1ClientTestAccess::config(client).rxOnlyInput == 2,
              "invalid combined P1 settings cannot partially replace the bank or input");
        check(routed.rxOnlyInput == 2 && routed.mkIIBpf
              && p1::receiveControl(0,routed)->at(3) == 0x59,
              "P1 dispatcher maps EXT1 and MkII RX master into general bank");
        config.bypassAdc0 = true;
        dispatcher.setHardwareFilters(filterOptions(config));
        check(P1ClientTestAccess::config(client).receiveFilterBits == 0x20,
            "P1 live bypass replaces selected BPF");
        auto invalid = filterOptions(config);
        invalid.insert(QStringLiteral("receive"), QVariantList{});
        dispatcher.setHardwareFilters(invalid);
        check(P1ClientTestAccess::config(client).receiveFilterBits == 0x20,
            "malformed live configuration leaves P1 control bank unchanged");
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
    {
        std::vector<QByteArray> packets; P1Client client;
        check(P1ClientTestAccess::prepare(client, params(5), packets), "RX OC P1 fixture");
        P1ClientTestAccess::feed(client, discovery(10));
        auto config = defaultFilterConfig(true);
        config.ocEnabled = true; config.ocReceive[1] = {3500000, 3800000, 11};
        client.setHardwareFilters(filterOptions(config));
        client.setDdc0FrequencyHz(3700000);
        const auto live = P1ClientTestAccess::config(client);
        check(live.rxOcMask == 11 && (*p1::receiveControl(0, live))[2] == 0x16,
              "P1 live retune selects OC mask in C2 bits 1..7");
        client.stop();
        check(packets.size() >= 2 && packets[packets.size()-2].size() == 1032
              && packets[packets.size()-2][13] == 0,
              "P1 clears active OC pins in a control packet before stream stop");
    }
    {
        std::vector<QByteArray> packets; P1Client client;
        check(P1ClientTestAccess::prepare(client,params(4),packets), "P1 RX bypass fixture");
        P1ClientTestAccess::feed(client,discovery(5));
        auto config = defaultFilterConfig(false);
        config.bypassAdc1 = false; config.receiveBypass[0] = true;
        client.setHardwareFilters(filterOptions(config)); client.setDdc0FrequencyHz(3800000);
        auto applied = P1ClientTestAccess::config(client);
        check(applied.receiveFilterBits == 0x20 && applied.adc1ReceiveFilterBits == 0x10,
              "P1 ADC0 per-band bypass does not leak into ADC1");
        config.disable6mLnaOnRx = true; config.lpfBypassOnRx = true;
        client.setHardwareFilters(filterOptions(config)); client.setDdc0FrequencyHz(51000000);
        applied = P1ClientTestAccess::config(client);
        check(applied.receiveFilterBits == 0x20 && applied.adc1ReceiveFilterBits == 0x40
              && applied.lowPassBits == 0x10,
              "P1 applies RX 6m LNA bypass and forced 6m LPF in bank-local bits");
    }
    qInstallMessageHandler(previousHandler);
    check(sessionLog.filter(QStringLiteral("validated board")).size() > 0
          && sessionLog.filter(QStringLiteral("RX IQ started")).size() > 0
          && sessionLog.filter(QStringLiteral("connection failed")).size() > 0,
          "P1 application logs distinguish validated discovery, first IQ and failures");
    std::cout << "ANAN P1 transport checks passed\n";
}
