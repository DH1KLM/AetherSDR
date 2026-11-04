#include "core/backends/anan/AnanDiversity.h"
#include "core/backends/anan/P1Client.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/RadioSettingsScope.h"
#include "core/AppSettings.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QJsonObject>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QThread>
#include <QSemaphore>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace AetherSDR::anan;

namespace AetherSDR::anan {
struct P1ClientTestAccess {
    static void seed(P1Client& client, std::vector<QByteArray>& writes) {
        client.m_running = client.m_validated = client.m_linkUp = true;
        client.m_config.numAdcs = 2;
        client.m_config.receivers = 5;
        client.m_profile = P1Client::Profile{2, 5, false, ReceiveFilterKind::LegacyHpf};
        client.m_host = QHostAddress(QStringLiteral("192.0.2.10"));
        client.m_clock.start();
        client.m_sendOverride = [&writes](std::span<const std::uint8_t> bytes) {
            writes.emplace_back(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
            return true;
        };
    }
    static void feed(P1Client& client, const p1::Packet& packet) {
        client.handleDatagram(client.m_host, 1024, packet);
    }
    static const p1::ReceiveConfig& config(const P1Client& client) { return client.m_config; }
    static void refuse(P1Client& client) {
        client.m_sendOverride = [](std::span<const std::uint8_t>) { return false; };
    }
};

struct P2ClientTestAccess {
    static void seed(P2Client& client, std::vector<QByteArray>& ddc, std::vector<QByteArray>& hp) {
        client.m_running = client.m_discoveryValidated = client.m_linkUp = true;
        client.m_numAdcs = 2; client.m_firstDdcIndex = 2; client.m_availableDdcs = 7;
        client.m_activeDdcs = {{48, 0}, {48, 1}};
        client.m_activeDdcCount = 2; client.m_streamLimit = 5;
        client.m_independentTuning = true;
        client.m_receiveWords[0] = phaseWord(10'000'000);
        client.m_receiveWords[1] = phaseWord(11'000'000);
        client.m_ddcRateSendOverride = [&ddc](std::span<const std::uint8_t> bytes) {
            ddc.emplace_back(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
            return qint64(bytes.size());
        };
        client.m_controlSendOverride = [&hp](std::span<const std::uint8_t> bytes, quint16) {
            hp.emplace_back(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
            return qint64(bytes.size());
        };
    }
    static void feed(P2Client& client, const std::vector<std::uint8_t>& packet, int port) {
        client.handleDatagram(packet, quint16(port));
    }
    static void refuse(P2Client& client) {
        client.m_ddcRateSendOverride = [](std::span<const std::uint8_t>) { return qint64(-1); };
    }
};

struct AnanClientTestAccess {
    static void seed(AnanClient& client, std::vector<QByteArray>& ddc, std::vector<QByteArray>& hp) {
        P2ClientTestAccess::seed(*client.m_p2, ddc, hp);
    }
};

struct AnanMultiRxTestAccess {
    static void seed(AnanBackend& backend, std::vector<QByteArray>& ddc, std::vector<QByteArray>& hp) {
        backend.m_connected = backend.m_discoveryInfoReceived = true;
        backend.m_discoveredBoardId = backend.m_pendingParams.expectedBoardId = 4;
        backend.m_discoveredReceiverCount = 7;
        backend.m_pendingParams.model = AnanModel::Anan200D;
        backend.m_pendingParams.protocol = 2;
        backend.m_radioSerial = QStringLiteral("diversity-test-radio");
        backend.m_receiveAdcs.fill(0);
        backend.m_receiveLayout.reset(5, 10e6, 48);
        backend.m_streamApplyOverride = [](const auto&, const auto&) { return true; };
        QMetaObject::invokeMethod(backend.m_client, [&] {
            AnanClientTestAccess::seed(*backend.m_client, ddc, hp);
        }, Qt::BlockingQueuedConnection);
    }
    static void txBusy(AnanBackend& backend, bool busy) { backend.m_txSessionArming = busy; }
    static void redPitaya(AnanBackend& backend, bool selected) {
        backend.m_discoveredBoardId = backend.m_pendingParams.expectedBoardId = selected ? 1 : 4;
        backend.m_pendingParams.model = selected ? AnanModel::RedPitaya : AnanModel::Anan200D;
        backend.m_pendingParams.protocol = selected ? 1 : 2;
        backend.m_pendingParams.redPitayaP1 = selected;
    }
    static void retire(AnanBackend& backend) {
        ++backend.m_receiveEpoch;
        backend.m_receiveIoEpoch->store(backend.m_receiveEpoch);
        backend.m_diversityPending = false;
        backend.m_connected = false;
    }
    static void block(AnanBackend& backend, QSemaphore& entered, QSemaphore& release) {
        QMetaObject::invokeMethod(backend.m_client, [&] { entered.release(); release.acquire(); }, Qt::QueuedConnection);
    }
    static void barrier(AnanBackend& backend) {
        QMetaObject::invokeMethod(backend.m_client, [] {}, Qt::BlockingQueuedConnection);
    }
};
} // namespace AetherSDR::anan

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) { ++failures; }
}

std::vector<std::uint8_t> iqPacket(int samples, std::uint32_t sequence = 0)
{
    std::vector<std::uint8_t> packet(16 + std::size_t(samples) * 6);
    for (int n = 0; n < 4; ++n) { packet[n] = std::uint8_t(sequence >> (24 - 8 * n)); }
    packet[13] = 24; packet[14] = std::uint8_t(samples >> 8); packet[15] = std::uint8_t(samples);
    for (int n = 0; n < samples; ++n) { packet[16 + n * 6] = 0x20; }
    return packet;
}

p1::Packet legacyPacket(std::uint32_t sequence)
{
    p1::Packet packet{};
    packet[0] = 0xef; packet[1] = 0xfe; packet[2] = 1; packet[3] = 6;
    for (int n = 0; n < 4; ++n) { packet[4 + n] = std::uint8_t(sequence >> (24 - 8 * n)); }
    for (int frame = 0; frame < 2; ++frame) {
        const int base = 8 + frame * 512;
        packet[base] = packet[base + 1] = packet[base + 2] = 0x7f;
        for (int n = 0; n < 504 / 32; ++n) {
            const int start = base + 8 + n * 32;
            packet[start] = packet[start + 6] = 0x20;
            packet[start + 12] = 0x10; packet[start + 18] = 0x08; packet[start + 24] = 0x04;
        }
    }
    return packet;
}

void mixerChecks()
{
    const std::array<std::complex<float>, 2> a{{{0.25f, 0.0f}, {0.0f, 0.25f}}};
    std::vector<std::complex<float>> result;
    DiversitySettings settings; settings.enabled = true;
    check(combineDiversity(a, a, settings, result) && std::abs(result[0].real() - 0.5f) < 1e-6f,
          "coherent in-phase inputs add before demodulation without amplitude normalization");
    settings.phaseDegrees = 180;
    check(combineDiversity(a, a, settings, result) && std::abs(result[0]) < 1e-6f
        && std::abs(result[1]) < 1e-6f, "180-degree rotation cancels both I and Q");
    settings.phaseDegrees = 90; settings.gain = 2;
    check(combineDiversity(a, a, settings, result) && std::abs(result[0] - std::complex<float>(0.25f, 0.5f)) < 1e-6f,
          "phase rotation uses the Thetis complex multiplier sign and gain");
    const std::array<std::complex<float>, 2> b{{{0.125f, 0.0f}, {0.125f, 0.0f}}};
    settings.reference = 1;
    check(combineDiversity(a, b, settings, result) && std::abs(result[0] - std::complex<float>(0.125f, 0.5f)) < 1e-6f,
          "reference selection rotates the other ADC");
    settings.output = 0;
    check(combineDiversity(a, b, settings, result) && result[0] == a[0], "ADC0 audition bypasses gain and phase");
    settings.output = 1;
    check(combineDiversity(a, b, settings, result) && result[0] == b[0], "ADC1 audition bypasses gain and phase");
    settings.enabled = false; settings.output = 2;
    check(combineDiversity(a, b, settings, result) && result[0] == a[0], "disabled combiner retains primary IQ");
    settings.gain = std::numeric_limits<double>::quiet_NaN();
    check(!combineDiversity(a, b, settings, result) && result.empty(), "nonfinite settings cannot publish stale samples");
    settings = {};
    check(!combineDiversity(a, std::span(b).first(1), settings, result), "unpaired input lengths are rejected");
}

void transportChecks(QCoreApplication& app)
{
    std::vector<QByteArray> ddcWrites, hpWrites, p1Writes;
    P2Client modern;
    P2ClientTestAccess::seed(modern, ddcWrites, hpWrites);
    std::vector<std::complex<float>> primary;
    QObject::connect(&modern, &P2Client::ddcIqReady, &app,
        [&primary](int index, const std::vector<std::complex<float>>& samples) {
            if (index == 0) { primary = samples; }
        });
    DiversitySettings settings; settings.enabled = true;
    check(modern.setDiversity(settings), "P2 enables paired receive configuration through injected writers");
    check(!ddcWrites.empty() && std::uint8_t(ddcWrites.back()[7]) == 0x09
        && std::uint8_t(ddcWrites.back()[1363]) == 2
        && ddcWrites.back()[17] == 0 && ddcWrites.back()[23] == 1,
          "P2 enables DDC0 plus independent DDC3 and requests interleaved DDC1 from ADC1");
    check(!hpWrites.empty() && hpWrites.back().mid(9, 4) == hpWrites.back().mid(13, 4)
        && hpWrites.back().mid(9, 4) != hpWrites.back().mid(21, 4),
          "P2 pair is frequency-locked while RX2 keeps its independent tuning");
    P2ClientTestAccess::feed(modern, iqPacket(4), 1035);
    check(primary.size() == 2 && std::abs(primary[0].real() - 0.5f) < 1e-6f,
          "P2 splits alternating same-frame samples into one coherent primary stream");
    settings.phaseDegrees = 180;
    check(modern.setDiversity(settings), "P2 phase edit is accepted without restarting the receiver");
    P2ClientTestAccess::feed(modern, iqPacket(4, 1), 1035);
    check(primary.size() == 2 && std::abs(primary[0]) < 1e-6f, "P2 production decoder cancels paired identical inputs");
    primary.clear();
    P2ClientTestAccess::feed(modern, iqPacket(3, 2), 1035);
    P2ClientTestAccess::feed(modern, iqPacket(4, 2), 1037);
    check(primary.empty(), "P2 rejects odd pairs and retired ordinary primary DDC traffic");
    settings.enabled = false;
    check(modern.setDiversity(settings) && std::uint8_t(ddcWrites.back()[7]) == 0x0c
        && ddcWrites.back()[1363] == 0, "P2 disabling restores the original DDC map and clears sync");
    P2ClientTestAccess::refuse(modern);
    settings.enabled = true;
    check(!modern.setDiversity(settings), "P2 refused configuration write is not accepted");

    P1Client legacy;
    P1ClientTestAccess::seed(legacy, p1Writes);
    check(legacy.setReceiveStreams({{48, 0}, {48, 1}, {48, 0}}, {10e6, 11e6, 12e6}), "seed three independent P1 streams");
    std::array<std::vector<std::complex<float>>, 4> streams;
    QObject::connect(&legacy, &P1Client::ddcIqReady, &app,
        [&streams](int index, const std::vector<std::complex<float>>& samples) {
            if (index >= 0 && index < 4) { streams[index] = samples; }
        });
    settings.phaseDegrees = 0;
    check(legacy.setDiversity(settings), "P1 enables a same-frame ADC pair");
    const auto& config = P1ClientTestAccess::config(legacy);
    const auto control = p1::receiveControl(0, config);
    check(control && ((*control)[4] & 0x80) && config.receiveAdcs[1] == 1
        && config.receiveFrequencies[0] == config.receiveFrequencies[1]
        && config.receiveFrequencies[2] == 11'000'000 && config.receiveFrequencies[3] == 12'000'000,
          "P1 sets Thetis frequency-lock bit and reserves slot1 without losing RX2/RX3 tuning");
    P1ClientTestAccess::feed(legacy, legacyPacket(0));
    check(!streams[0].empty() && !streams[1].empty() && !streams[2].empty()
        && std::abs(streams[0][0].real() - 0.5f) < 1e-6f
        && std::abs(streams[1][0].real() - 0.125f) < 1e-6f
        && std::abs(streams[2][0].real() - 0.0625f) < 1e-6f,
          "P1 production decoder combines ADC slots0/1 and preserves the independent stream map");
    settings.enabled = false;
    check(legacy.setDiversity(settings) && !P1ClientTestAccess::config(legacy).diversity
        && P1ClientTestAccess::config(legacy).receiveFrequencies[1] == 12'000'000,
          "P1 disabling restores the saved logical stream assignments");
    P1ClientTestAccess::refuse(legacy);
    settings.enabled = true;
    check(!legacy.setDiversity(settings) && !P1ClientTestAccess::config(legacy).diversity,
          "P1 refused wire write leaves local diversity disabled");
}

void backendChecks(QCoreApplication& app)
{
    using namespace AetherSDR;
    std::vector<QByteArray> ddc, hp;
    AnanBackend backend;
    AnanMultiRxTestAccess::seed(backend, ddc, hp);
    QVariantMap state;
    int replies = 0, errors = 0;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &app, [&](quint64, const QVariant& value) {
        state = value.toMap(); ++replies;
    });
    QObject::connect(&backend, &IRadioBackend::extensionError, &app, [&](quint64, const QString&) { ++errors; });
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.get"), 1);
    check(state.value(QStringLiteral("available")).toBool(), "validated two-ADC backend admits diversity editing");
    AnanMultiRxTestAccess::redPitaya(backend, true);
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.get"), 10);
    check(state.value(QStringLiteral("available")).toBool(), "P1 Red Pitaya reporting Hermes retains its two physical ADCs");
    AnanMultiRxTestAccess::redPitaya(backend, false);
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.get"), 11);
    QVariantMap fields{{QStringLiteral("identity"), state.value(QStringLiteral("identity"))},
        {QStringLiteral("enabled"), true}, {QStringLiteral("gain"), 1.25},
        {QStringLiteral("phaseDegrees"), 12.5}, {QStringLiteral("reference"), 0}, {QStringLiteral("output"), 2}};
    QVariantMap invalid = fields; invalid[QStringLiteral("identity")] = QStringLiteral("retired");
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 2, invalid);
    invalid = fields; invalid[QStringLiteral("phaseDegrees")] = QStringLiteral("12.5");
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 3, invalid);
    AnanMultiRxTestAccess::txBusy(backend, true);
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 4, fields);
    AnanMultiRxTestAccess::txBusy(backend, false);
    check(errors == 3 && ddc.empty(), "retired identity, string-valued phase and TX preparation refuse before transport writes");
    const RadioSettingsScope scope(QStringLiteral("anan"), QStringLiteral("diversity-test-radio"));
    const QJsonObject future{{QStringLiteral("future"), true}};
    check(scope.setFeature(QStringLiteral("Diversity"), 99, future), "seed a newer diversity schema");
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 5, fields);
    check(errors == 4 && ddc.empty() && scope.featureExact(QStringLiteral("Diversity")) == future,
          "newer settings schema is preserved and cannot authorize a wire change");
    check(scope.removeFeature(QStringLiteral("Diversity")), "clear isolated future-schema fixture");
    const int before = replies;
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 6, fields);
    QElapsedTimer timer; timer.start();
    while (replies == before && timer.elapsed() < 2000) { app.processEvents(); QThread::msleep(1); }
    AnanMultiRxTestAccess::barrier(backend);
    check(replies == before + 1 && state.value(QStringLiteral("enabled")).toBool() && !ddc.empty(),
          "queued production route applies and acknowledges the same-session configuration");
    const QJsonObject document = scope.featureExact(QStringLiteral("Diversity"));
    const QJsonObject saved = document.isEmpty() ? QJsonObject{} : document.begin().value().toObject();
    check(document.size() == 1 && saved.value(QStringLiteral("phaseDegrees")).toDouble() == 12.5
        && !saved.contains(QStringLiteral("enabled")), "only adjustment values persist, scoped by model/board/protocol");
    QSemaphore entered, release;
    AnanMultiRxTestAccess::block(backend, entered, release);
    const bool blocked = entered.tryAcquire(1, 2000);
    check(blocked, "establish deterministic queued-request retirement ordering");
    const std::size_t writes = ddc.size();
    fields[QStringLiteral("phaseDegrees")] = 25.0;
    backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("diversity.set"), 7, fields);
    AnanMultiRxTestAccess::retire(backend);
    release.release();
    AnanMultiRxTestAccess::barrier(backend); app.processEvents();
    check(ddc.size() == writes && scope.featureExact(QStringLiteral("Diversity")) == document,
          "retiring the connection fences queued diversity work and settings writes");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-diversity"));
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    mixerChecks();
    transportChecks(app);
    backendChecks(app);
    return failures == 0 ? 0 : 1;
}
