//DH1KLM: Inject the real P2Client frame handler without creating sockets.
// Thetis ANAN-7000DLE normal RX1 uses physical DDC2, delivered as logical RX0.
#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/AnanFilterOptions.h"

#include <QStringList>
#include <QLoggingCategory>
#include <QCoreApplication>

#include <array>
#include <algorithm>
#include <complex>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

using namespace AetherSDR::anan;

namespace {
QStringList sessionLog;
void captureSessionLog(QtMsgType, const QMessageLogContext& context, const QString& message)
{
    if (QString::fromLatin1(context.category ? context.category : "") == QLatin1String("aether.anan.p2")) {
        sessionLog.append(message);
    }
}
}

namespace AetherSDR::anan {
struct P2ClientTestAccess {
    static void prepare(P2Client& client, int selectedBoardId, AnanModel model = AnanModel::Automatic,
                        bool overrideDetection = false)
    {
        client.m_expectedBoardId = selectedBoardId;
        client.m_model = model;
        client.m_overrideDetection = overrideDetection;
        const auto profile = receiveProfile(selectedBoardId, model, overrideDetection);
        client.m_numAdcs = profile ? profile->numAdcs : 2;
        client.m_activeDdcs = {{48, 0}};
        client.m_activeDdcCount = 1;
        client.m_running = true;
    }
    static void feed(P2Client& client, std::span<const std::uint8_t> bytes, quint16 port)
    {
        client.handleDatagram(bytes, port);
    }
    static std::uint32_t frequencyWord(const P2Client& client) { return client.m_ddc0FreqWord; }
    static double correction(const P2Client& client) { return client.m_frequencyCorrection; }
    static bool replaceStreams(P2Client& client, const std::vector<DdcConfig>& streams,
                               const std::vector<double>& centers, qint64 sent = 1444)
    {
        client.m_ddcRateSendOverride = [sent](auto) { return sent; };
        const bool ok = client.setReceiveStreams(streams, centers);
        client.m_ddcRateSendOverride = {};
        return ok;
    }
    static auto words(const P2Client& client) { return client.sharedFreqWords(); }
    static bool validated(const P2Client& client) { return client.m_discoveryValidated; }
    static int physicalDdc(const P2Client& client) { return client.m_firstDdcIndex; }
    static int receiveInput(const P2Client& client) { return client.m_filters ? client.m_filters->rxOnlyInput : 0; }
    static int antenna(const P2Client& client) { return client.m_rxAntenna; }
    static bool selectLpf(const P2Client& client) { return client.m_selectReceiveLpf; }
    static void setAdcs(P2Client& client, int count) { client.m_numAdcs = count; }
    static void setDdcCount(P2Client& client, int count) { client.m_activeDdcCount = count; }
    static int adcs(const P2Client& client) { return client.m_numAdcs; }
    static ReceiveFilterKind filters(const P2Client& client) { return client.m_receiveFilterKind; }
    static bool swapsSpeaker(const P2Client& client) { return client.m_swapSpeakerChannels; }
    static void setDdc(P2Client& client, DdcConfig config) { client.m_activeDdcs = {config}; }
    static void setDdcs(P2Client& client, std::vector<DdcConfig> ddcs)
    {
        client.m_activeDdcs = std::move(ddcs);
        client.m_activeDdcCount = static_cast<int>(client.m_activeDdcs.size());
    }
    static void setExpected(P2Client& client, int ddc, std::uint32_t sequence)
    {
        client.m_expectedSeq[ddc] = sequence;
    }
    static auto expected(const P2Client& client, int ddc) { return client.m_expectedSeq[ddc]; }
    struct RateAttempt {
        bool accepted = false;
        std::optional<std::array<std::uint8_t, 1444>> packet;
    };
    static RateAttempt sendRate(P2Client& client, int ddc, int rate, qint64 result = 1444)
    {
        RateAttempt attempt;
        client.m_ddcRateSendOverride = [&attempt, result](std::span<const std::uint8_t> bytes) {
            attempt.packet.emplace();
            std::copy(bytes.begin(), bytes.end(), attempt.packet->begin());
            return result;
        };
        attempt.accepted = client.setDdcRateLive(ddc, rate);
        client.m_ddcRateSendOverride = {};
        return attempt;
    }
    static auto ratePacket(P2Client& client, int ddc, int rate)
    {
        const auto attempt = sendRate(client, ddc, rate);
        return attempt.accepted ? attempt.packet : std::nullopt;
    }
    static auto candidate(P2Client& client, int ddc, int rate)
    {
        return client.prepareDdcRateChange(ddc, rate);
    }
    static int rate(const P2Client& client, int ddc) { return client.m_activeDdcs[ddc].rateKsps; }
    static auto txBytes(const P2Client& client) { return client.m_txBytes; }
    static int alex(const P2Client& client) { return client.m_alexAttenuationDb; }
    static int attenuation(const P2Client& client, int adc)
    {
        return adc == 0 ? client.m_adc0AttenuationDb : client.m_adc1AttenuationDb;
    }
    static auto refresh(P2Client& client, qint64 nowMs)
    {
        return client.takeGeneralRefresh(nowMs);
    }
};
}

static int failures = 0;
static void check(bool condition, const char* claim)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", claim);
        ++failures;
    }
}

static std::array<std::uint8_t, 60> discovery(std::uint8_t boardId)
{
    std::array<std::uint8_t, 60> packet{};
    packet[4] = 2;
    packet[5] = 0x88;
    packet[10] = 0x6A;
    packet[11] = boardId;
    packet[12] = 43;
    packet[13] = 27;
    packet[20] = 7; // Deliberately not the profile's physical DDC count.
    packet[21] = 1;
    return packet;
}

static std::array<std::uint8_t, kDdcHeaderLen + kDdcSampleBytes> iq()
{
    std::array<std::uint8_t, kDdcHeaderLen + kDdcSampleBytes> packet{};
    packet[13] = 24;
    packet[15] = 1;
    packet[kDdcHeaderLen] = 0x40;
    return packet;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
    const auto previousHandler = qInstallMessageHandler(captureSessionLog);
    //DH1KLM: Resolve the actual profile before driving its independent ADC attenuators.
    for (const int board : {1,2,3,4,5,10,20}) {
        for (const auto& entry : kAnanModels) {
            if (entry.model == AnanModel::Automatic || !receiveProfile(entry.nativeBoardId)) { continue; }
            P2Client client;
            P2ClientTestAccess::prepare(client, board, entry.model, true);
            //DH1KLM Oscillator correction follows every valid model/board override, not its label.
            client.setFrequencyCorrection(1.00001);
            client.setFrequencyCorrection(std::numeric_limits<double>::quiet_NaN());
            check(P2ClientTestAccess::correction(client) == 1.00001,
                  "all P2 profile overrides preserve valid correction and refuse nonfinite factors");
            P2ClientTestAccess::feed(client, discovery(board), 1024);
            check(P2ClientTestAccess::validated(client), "attenuator test requires a validated model override");
            const int adcs = P2ClientTestAccess::adcs(client);
            client.setStepAttenuationDb(0, 17);
            client.setStepAttenuationDb(1, 23);
            check(P2ClientTestAccess::attenuation(client, 0) == 17
                  && P2ClientTestAccess::attenuation(client, 1) == (adcs == 2 ? 23 : 0),
                  "only fitted ADC attenuators accept independent live settings");
            for (const int value : {std::numeric_limits<int>::min(), -1, 0, 1, 17, 31, 32,
                                     std::numeric_limits<int>::max()}) {
                for (int adc = 0; adc < adcs; ++adc) {
                    const int other = P2ClientTestAccess::attenuation(client, 1-adc);
                    client.setStepAttenuationDb(adc, value);
                    check(P2ClientTestAccess::attenuation(client, adc) == std::clamp(value, 0, 31)
                          && P2ClientTestAccess::attenuation(client, 1-adc) == other,
                          "P2 attenuator clamps at its boundary without changing the other ADC");
                }
            }
            client.setFrontEndAttenuation(0, 20, 30);
            const int expectedAlex = P2ClientTestAccess::filters(client) == ReceiveFilterKind::LegacyHpf ? 30 : 0;
            check(P2ClientTestAccess::attenuation(client, 0) == 20
                  && P2ClientTestAccess::alex(client) == expectedAlex,
                  "combined P2 setter retains ADC and classic relay stages, excludes MkII");
            client.setFrontEndAttenuation(1, 9, 0);
            check(P2ClientTestAccess::alex(client) == expectedAlex,
                  "ADC1 edit cannot clear ADC0 ALEX attenuation");
            const int before0 = P2ClientTestAccess::attenuation(client, 0);
            const int before1 = P2ClientTestAccess::attenuation(client, 1);
            client.setStepAttenuationDb(-1, 0);
            client.setStepAttenuationDb(2, 0);
            check(P2ClientTestAccess::attenuation(client, 0) == before0
                  && P2ClientTestAccess::attenuation(client, 1) == before1,
                  "invalid ADC indices cannot alter either attenuator");
        }
    }
    //DH1KLM: Direct setter injection, no socket or running session required.
    {
        P2Client client;
        client.setDdc0FrequencyHz(14'200'000);
        const auto previous = P2ClientTestAccess::frequencyWord(client);
        check(previous == phaseWord(14'200'000), "P2 accepts a valid receive tune");
        for (const double hz : {-1.0, 61'440'001.0, 1e300,
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity()}) {
            client.setDdc0FrequencyHz(hz);
            check(P2ClientTestAccess::frequencyWord(client) == previous,
                  "invalid P2 tune preserves the last valid phase word");
        }
        for (const double hz : {0.0, 61'440'000.0, 7'074'000.0}) {
            client.setDdc0FrequencyHz(hz);
            check(P2ClientTestAccess::frequencyWord(client) == phaseWord(hz),
                  "P2 keeps the established inclusive P1 tuning boundaries");
        }
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client,5,AnanModel::Anan200D,true);
        P2ClientTestAccess::feed(client,discovery(5),1024);
        auto config = defaultFilterConfig(false);
        config.rxOnlyInput = 3;
        client.setHardwareFilters(filterOptions(config));
        check(P2ClientTestAccess::receiveInput(client) == 3, "P2 live setter retains the selected RX input");
        auto invalid = filterOptions(config);
        invalid.insert(QStringLiteral("rxOnlyInput"),4);
        client.setHardwareFilters(invalid);
        check(P2ClientTestAccess::receiveInput(client) == 3, "P2 malformed input preserves the RX route");
        config.rxOnlyInput = 1;
        client.setReceiveHardware(filterOptions(config), 2);
        check(P2ClientTestAccess::receiveInput(client) == 1 && P2ClientTestAccess::antenna(client) == 2,
              "P2 applies an antenna bank and RX-only input as one complete receive configuration");
        client.setReceiveHardware(filterOptions(config), 0);
        check(P2ClientTestAccess::receiveInput(client) == 1 && P2ClientTestAccess::antenna(client) == 2,
              "invalid combined P2 bank cannot partially replace the input");
        client.setReceiveHardware(invalid, 3);
        check(P2ClientTestAccess::receiveInput(client) == 1 && P2ClientTestAccess::antenna(client) == 2,
              "invalid combined P2 input cannot partially replace the bank");
        client.setHardwareFilters(filterOptions(defaultFilterConfig(false)));
        check(P2ClientTestAccess::receiveInput(client) == 0 && P2ClientTestAccess::antenna(client) == 2,
              "P2 model defaults restore ANT input without changing its independent bank");
        client.stop();
    }
    //DH1KLM: Exercise production live-rate dispatch with an injected send, never a socket.
    for (const int board : {1, 2, 3, 4, 5, 10, 20}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, board);
        check(!P2ClientTestAccess::ratePacket(client, 0, 96), "no rate change before discovery validation");
        P2ClientTestAccess::feed(client, discovery(board), 1024);
        const auto profile = *receiveProfile(board);
        P2ClientTestAccess::setDdcs(client, {{48, 0}, {192, profile.numAdcs - 1}});
        P2ClientTestAccess::setExpected(client, 0, 100);
        check(P2ClientTestAccess::candidate(client, 0, 96).has_value()
              && P2ClientTestAccess::rate(client, 0) == 48
              && P2ClientTestAccess::expected(client, 0) == 100,
              "building a P2 candidate alone cannot commit its rate or reset gap detection");
        P2ClientTestAccess::setDdcs(client, {{48, 0}, {192, profile.numAdcs - 1}});
        const auto original = P2ClientTestAccess::ratePacket(client, 0, 48);
        check(original && (*original)[7] == (3u << profile.firstDdcIndex),
              "rate packet enables exactly the two selected physical DDC rows");
        P2ClientTestAccess::setExpected(client, 0, 100);
        P2ClientTestAccess::setExpected(client, 1, 200);
        for (const int ddc : {0, 1}) {
            for (const qint64 result : {-1, 0, 1443}) {
                const auto before = P2ClientTestAccess::txBytes(client);
                const auto attempt = P2ClientTestAccess::sendRate(client, ddc, 96, result);
                check(!attempt.accepted && attempt.packet.has_value(),
                      "P2 failed or incomplete local write reports failure after attempting the packet");
                check(P2ClientTestAccess::rate(client, 0) == 48
                      && P2ClientTestAccess::rate(client, 1) == 192
                      && P2ClientTestAccess::expected(client, 0) == 100
                      && P2ClientTestAccess::expected(client, 1) == 200,
                      "P2 failed rate write preserves both DDC rates and sequence expectations");
                check(P2ClientTestAccess::txBytes(client) == before + (result > 0 ? result : 0),
                      "P2 counters count only locally accepted bytes");
            }
        }
        for (const int rate : {-1, 0, 47, 49, 65536, std::numeric_limits<int>::max()}) {
            check(!P2ClientTestAccess::ratePacket(client, 0, rate), "invalid live rate produces no packet");
            check(P2ClientTestAccess::ratePacket(client, 0, 48) == original
                  && P2ClientTestAccess::expected(client, 0) == 100
                  && P2ClientTestAccess::expected(client, 1) == 200,
                  "refused rate preserves cached configuration and sequence tracking");
        }
        for (const int ddc : {-1, 2, 4}) {
            check(!P2ClientTestAccess::ratePacket(client, ddc, 96), "inactive logical DDC cannot change rate");
        }
        for (const int rate : kDdc0RatesKsps) {
            P2ClientTestAccess::setExpected(client, 0, 100);
            const auto packet = P2ClientTestAccess::ratePacket(client, 0, rate);
            check(packet.has_value(), "each supported P2 rate prepares a live packet");
            if (!packet) { continue; }
            const std::size_t row = 17 + 6 * profile.firstDdcIndex;
            check((((*packet)[row + 1] << 8) | (*packet)[row + 2]) == rate,
                  "selected rate uses the correct physical DDC row");
            auto normalized = *packet;
            normalized[row + 1] = (*original)[row + 1];
            normalized[row + 2] = (*original)[row + 2];
            check(normalized == *original, "rate change preserves sibling DDC, ADC routes and flags");
            check(P2ClientTestAccess::expected(client, 0).has_value() == (rate == 48)
                  && P2ClientTestAccess::expected(client, 1) == 200,
                  "only a changed DDC loses its previous sequence expectation");
            P2ClientTestAccess::setExpected(client, 0, 101);
            check(P2ClientTestAccess::ratePacket(client, 0, rate) == packet
                  && P2ClientTestAccess::expected(client, 0) == 101,
                  "unchanged-rate retry emits the same packet and keeps gap detection");
            P2ClientTestAccess::ratePacket(client, 0, 48);
        }
        P2ClientTestAccess::setExpected(client, 0, 300);
        const auto second = P2ClientTestAccess::ratePacket(client, 1, 384);
        check(second.has_value(), "second logical DDC can change rate independently");
        if (second) {
            const std::size_t row = 17 + 6 * (profile.firstDdcIndex + 1);
            check((((*second)[row + 1] << 8) | (*second)[row + 2]) == 384,
                  "second logical DDC uses its own physical row");
            auto normalized = *second;
            normalized[row + 1] = (*original)[row + 1];
            normalized[row + 2] = (*original)[row + 2];
            check(normalized == *original && P2ClientTestAccess::expected(client, 0) == 300
                  && !P2ClientTestAccess::expected(client, 1),
                  "second-DDC change preserves the first receiver and its sequence tracking");
        }
        client.stop();
        check(!P2ClientTestAccess::ratePacket(client, 0, 96), "stopped session cannot prepare rate changes");
    }
    //DH1KLM: Discard the initial General as a lost UDP datagram. Continued IQ
    // must not prevent later General packets from restoring ALEX enable. The
    // scheduler used by the real keepalive is driven without sleeps or sockets.
    for (const int board : {1, 5, 10}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, board);
        check(!P2ClientTestAccess::refresh(client, 5000),
              "no General retry before discovery validation");
        P2ClientTestAccess::feed(client, discovery(board), 1024);
        const auto profile = *receiveProfile(board);
        P2ClientTestAccess::feed(client, iq(),
            static_cast<quint16>(1035 + profile.firstDdcIndex));
        check(!P2ClientTestAccess::refresh(client, 499), "General refresh is not sent every HP tick");
        const auto retry = P2ClientTestAccess::refresh(client, 500);
        check(retry && (*retry)[4] == 0 && (*retry)[37] == 8 && (*retry)[38] == 1
              && (*retry)[58] == profile.disablePa && (*retry)[59] == profile.alexEnableMask,
              "lost General is retried with the validated model's ALEX/PA configuration despite live IQ");
        check(!P2ClientTestAccess::refresh(client, 999), "General retry respects its own cadence");
        check(P2ClientTestAccess::refresh(client, 1000) == retry,
              "General refresh continues after the first retry");
        check(P2ClientTestAccess::refresh(client, 9000) == retry
              && !P2ClientTestAccess::refresh(client, 9000),
              "late timer delivery sends one refresh instead of a catch-up burst");
        client.stop();
        check(!P2ClientTestAccess::refresh(client, 10000), "disconnect cancels pending General refresh");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        const auto retry = P2ClientTestAccess::refresh(client, 500);
        check(retry && (*retry)[59] == 3 && (*retry)[58] == 1,
              "Anvelina/Orion MkII mapped to ANAN-200D refreshes the captured ALEX enable 03 with PA disabled");
        client.stop();
        P2ClientTestAccess::prepare(client, 10);
        check(!P2ClientTestAccess::refresh(client, 500),
              "a new unvalidated session cannot replay the previous device's General");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        int links = 0;
        int samples = 0;
        int logicalDdc = -1;
        int gapDdc = -1;
        QObject::connect(&client, &P2Client::linkUp, &client, [&]() { ++links; });
        QObject::connect(&client, &P2Client::ddc0IqReady, &client,
            [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
        QObject::connect(&client, &P2Client::ddcIqReady, &client,
            [&](int index, const std::vector<std::complex<float>>&) { logicalDdc = index; });
        QObject::connect(&client, &P2Client::ddcSequenceGap, &client,
            [&](int index) { gapDdc = index; });
        const auto frame = iq();
        P2ClientTestAccess::feed(client, frame, 1035);
        check(links == 0 && samples == 0, "stale DDC0 cannot link a session before profile validation");
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 2
              && P2ClientTestAccess::selectLpf(client),
              "Orion MkII selects its DDC2/LPF profile independent of reported receivers");
        P2ClientTestAccess::feed(client, frame, 1035);
        P2ClientTestAccess::feed(client, frame, 1038);
        check(links == 0 && samples == 0, "disabled physical DDC0 and DDC3 cannot establish single RX1");
        P2ClientTestAccess::feed(client, frame, 1037);
        check(links == 1 && samples == 1 && logicalDdc == 0,
              "physical DDC2 establishes link and reaches the existing logical RX0 DSP signal");
        P2ClientTestAccess::feed(client, frame, 1037);
        check(links == 1 && gapDdc == 0 && client.droppedPackets() == 1,
              "physical DDC2 sequence discontinuity is delivered as logical RX0");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 10);
        P2ClientTestAccess::feed(client, discovery(10), 1024);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 0
              && !P2ClientTestAccess::selectLpf(client), "Saturn retains its DDC0 packet profile");
    }
    for (const int selected : {10, 4}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, selected);
        int errors = 0;
        QObject::connect(&client, &P2Client::connectionError, &client,
            [&](const QString&) { ++errors; });
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning() && errors == 1,
              "Orion MkII cannot satisfy another selected physical board");
    }
    for (const std::uint8_t unsupported : {0, 6, 11, 99}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, -1);
        P2ClientTestAccess::feed(client, discovery(unsupported), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "recognized but unimplemented RX boards and unknown boards fail closed");
    }
    for (const bool invalidAdcs : {true, false}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        if (invalidAdcs) {
            P2ClientTestAccess::setAdcs(client, 1);
        } else {
            P2ClientTestAccess::setDdcCount(client, 6);
        }
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "Orion MkII rejects wrong ADC count and a DDC range exceeding the profile");
    }
    for (const DdcConfig invalid : {DdcConfig{48, 2}, DdcConfig{48, -1}, DdcConfig{123, 0}}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5);
        P2ClientTestAccess::setDdc(client, invalid);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "invalid physical ADC indices and unsupported rates cannot start an Orion MkII session");
    }
    for (const int id : {1, 2, 3, 4, 20}) {
        for (const bool manual : {false, true}) {
            P2Client client;
            P2ClientTestAccess::prepare(client, manual ? -1 : id);
            int samples = 0;
            QObject::connect(&client, &P2Client::ddc0IqReady, &client,
                [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
            P2ClientTestAccess::feed(client, discovery(static_cast<std::uint8_t>(id)), 1024);
            const ReceiveProfile profile = *receiveProfile(id);
            check(P2ClientTestAccess::validated(client)
                  && P2ClientTestAccess::adcs(client) == profile.numAdcs
                  && P2ClientTestAccess::filters(client) == profile.filterKind
                  && P2ClientTestAccess::swapsSpeaker(client) == profile.swapSpeakerChannels,
                  "selected and address-only new-family sessions resolve physical RX properties");
            P2ClientTestAccess::feed(client, iq(), static_cast<quint16>(1035 + profile.firstDdcIndex));
            check(samples == 1, "new receive family delivers its physical RX1 stream to the existing DSP signal");
        }
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 2);
        P2ClientTestAccess::setDdcCount(client, 3);
        P2ClientTestAccess::feed(client, discovery(2), 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "Hermes II rejects a third physical DDC even though the generic packet has room");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 1);
        P2ClientTestAccess::setDdc(client, {48, 1});
        P2ClientTestAccess::feed(client, discovery(1), 1024);
        check(!P2ClientTestAccess::validated(client), "Hermes cannot use nonexistent ADC1");
    }
    //DH1KLM: Thetis NetworkIO compatibility includes alternate Hermes IDs and
    //DH1KLM: Red Pitaya's Hermes identity; every accepted pair must deliver IQ.
    int compatiblePairs = 0;
    for (const int board : {1, 2, 3, 4, 5, 10, 20}) {
        for (const auto& entry : kAnanModels) {
            const int id = static_cast<int>(entry.model);
            const bool expected = id == -1 || entry.nativeBoardId == board
                || (id >= 2 && id <= 5 && (board == 1 || board == 2))
                || (id == 15 && board == 1);
            P2Client client;
            P2ClientTestAccess::prepare(client, board, entry.model);
            int blocks = 0;
            QObject::connect(&client, &P2Client::ddc0IqReady, [&](const auto&) { ++blocks; });
            P2ClientTestAccess::feed(client, discovery(board), 1024);
            check(P2ClientTestAccess::validated(client) == expected,
                  "P2 model/board acceptance matches Thetis compatibility without override");
            if (!expected) { continue; }
            ++compatiblePairs;
            const auto profile = *receiveProfile(board, entry.model);
            for (const int rate : kDdc0RatesKsps) {
                const auto packet = P2ClientTestAccess::ratePacket(client, 0, rate);
                const std::size_t row = 17 + 6 * profile.firstDdcIndex;
                check(packet && (((*packet)[row + 1] << 8) | (*packet)[row + 2]) == rate,
                      "every compatible P2 pair places all six rates in its effective DDC row");
            }
            P2ClientTestAccess::feed(client, iq(), static_cast<quint16>(1035 + profile.firstDdcIndex));
            check(blocks == 1, "logical model uses its correct P2 RX port");
        }
    }
    check(compatiblePairs == 27, "P2 compatibility matrix covers 27 accepted model/board pairs");
    for (const int board : {1, 5}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, board, AnanModel::RedPitaya);
        P2ClientTestAccess::feed(client, discovery(board), 1024);
        check(P2ClientTestAccess::adcs(client) == 2 && P2ClientTestAccess::physicalDdc(client) == 2
            && P2ClientTestAccess::filters(client) == ReceiveFilterKind::LegacyHpf,
            "Red Pitaya P2 shared board IDs use dual ADC and legacy filters");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D);
        P2ClientTestAccess::feed(client, discovery(5), 1024);
        check(!P2ClientTestAccess::validated(client), "Orion MkII cannot silently become ANAN-200D");
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan8000);
        auto busy = discovery(5); busy[4] = 3;
        P2ClientTestAccess::feed(client, busy, 1024);
        check(!P2ClientTestAccess::validated(client), "selected logical model cannot take over a busy P2 radio");
    }
    {
        //DH1KLM: A DIY Orion MkII digital board can deliberately use the
        // ANAN-200D effective profile. Board identity remains the actual reply.
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        P2ClientTestAccess::feed(client, discovery(5), 0);
        check(P2ClientTestAccess::validated(client)
              && P2ClientTestAccess::physicalDdc(client) == 2
              && P2ClientTestAccess::adcs(client) == 2
              && P2ClientTestAccess::filters(client) == ReceiveFilterKind::LegacyHpf,
              "explicit override applies ANAN-200D legacy filters on Orion MkII");
    }
    for (const bool busy : {false, true}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::Anan200D, true);
        auto packet = discovery(busy ? 5 : 4);
        if (busy) { packet[4] = 3; }
        P2ClientTestAccess::feed(client, packet, 1024);
        check(!P2ClientTestAccess::validated(client) && !client.isRunning(),
              "override cannot bypass busy status or a different selected device identity");
    }
    for (const quint16 port : {0, 1024, 1025, 1037}) {
        P2Client client;
        P2ClientTestAccess::prepare(client, 5, AnanModel::RedPitaya);
        auto packet = discovery(5);
        std::fill(packet.begin() + 5, packet.begin() + 11, 0);
        packet[10] = 1; packet[12] = 2; packet[13] = 30;
        P2ClientTestAccess::feed(client, packet, port);
        check(P2ClientTestAccess::validated(client) == (port == 0 || port == 1024),
              "Patroclus Red Pitaya P2 validates only on a discovery endpoint, never status/IQ ports");
        if (port == 0 || port == 1024) {
            int samples = 0;
            QObject::connect(&client, &P2Client::ddc0IqReady,
                [&](const std::vector<std::complex<float>>& block) { samples += int(block.size()); });
            P2ClientTestAccess::feed(client, iq(), 1037);
            check(samples == 1 && P2ClientTestAccess::physicalDdc(client) == 2,
                  "accepted Patroclus P2 discovery routes physical DDC2 into logical RX0");
        }
    }
    {
        P2Client client;
        P2ClientTestAccess::prepare(client, 4);
        P2ClientTestAccess::feed(client, discovery(4), 1024);
        const std::vector<DdcConfig> streams{{48, 0}, {96, 0}, {192, 0}, {48, 0}, {48, 0}};
        const std::vector<double> centers{14'175'000, 7'100'000, 21'250'000, 28'500'000,
                                          50'150'000};
        check(P2ClientTestAccess::replaceStreams(client, streams, centers),
              "Orion accepts five independent logical DDCs within DDC2..6");
        const auto encoded = P2ClientTestAccess::ratePacket(client, 4, 48);
        check(encoded && (*encoded)[7] == 0x7c && (*encoded)[17 + 6 * 6 + 5] == 24,
              "Orion five-stream packet enables physical DDC2..6 with a complete "
              "final row");
        int lastStream = 0;
        QObject::connect(&client, &P2Client::ddcIqReady, [&](int index, const auto&) {
            if (index == 4)
                ++lastStream;
        });
        P2ClientTestAccess::feed(client, iq(), 1041);
        check(lastStream == 1, "physical DDC6 IQ arrives at logical user stream four");
        const auto words = P2ClientTestAccess::words(client);
        check(words.size() == 5 && words[0] == phaseWord(centers[0]) &&
                  words[1] == phaseWord(centers[1]) && words[4] == phaseWord(centers[4]),
              "each enabled P2 DDC retains its independent tuning word for "
              "keepalives");
        auto bad = centers;
        bad[2] = std::numeric_limits<double>::quiet_NaN();
        check(!P2ClientTestAccess::replaceStreams(client, streams, bad) &&
                  P2ClientTestAccess::words(client) == words,
              "malformed additional tuning leaves every existing DDC word "
              "unchanged");
        P2ClientTestAccess::setExpected(client,0,17);P2ClientTestAccess::setExpected(client,1,29);
        auto changed = streams;
        changed[1].rateKsps = 384;
        check(!P2ClientTestAccess::replaceStreams(client, changed, centers, -1) &&
                  P2ClientTestAccess::words(client) == words,
              "refused DDC configuration write preserves the current receiver "
              "assignment");
        check(P2ClientTestAccess::expected(client,0)==17&&P2ClientTestAccess::expected(client,1)==29,"failed multi-DDC write preserves all sequence trackers");
        check(P2ClientTestAccess::replaceStreams(client,changed,centers)&&P2ClientTestAccess::expected(client,0)==17&&!P2ClientTestAccess::expected(client,1),"adopted rate resets only the changed DDC tracker");
        auto tooMany = streams;
        tooMany.push_back({48, 0});
        auto more = centers;
        more.push_back(3'700'000);
        check(!P2ClientTestAccess::replaceStreams(client, tooMany, more),
              "a sixth user DDC exceeds the bounded stream pool");
    }
    qInstallMessageHandler(previousHandler);
    check(sessionLog.filter(QStringLiteral("validated board")).size() > 0
          && sessionLog.filter(QStringLiteral("RX IQ started")).size() > 0,
          "P2 application logs report successful discovery and first IQ without speaker audio");
    if (failures == 0) {
        std::puts("anan_receive_profile_test: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
