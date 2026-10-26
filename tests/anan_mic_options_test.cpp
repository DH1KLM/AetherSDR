#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/anan/AnanMicOptions.h"
#include "core/backends/anan/AnanSettings.h"
#include "core/backends/anan/P1Protocol.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>
#include <cstdlib>

using namespace AetherSDR;
using namespace AetherSDR::anan;
namespace AetherSDR::anan {
class AnanMicTestAccess {
public:
    static void prepare(AnanBackend& backend, int board, AnanModel model, int protocol)
    {
        backend.m_connected = true;
        backend.m_discoveryInfoReceived = true;
        backend.m_discoveredBoardId = board;
        backend.m_radioSerial = QStringLiteral("mic-test");
        backend.m_pendingParams.model = model;
        backend.m_pendingParams.protocol = protocol;
        backend.loadHardwareFilters(board);
    }
    static void reconnect(AnanBackend& backend) { ++backend.m_connectGeneration; }
    static void disconnect(AnanBackend& backend) { backend.m_connected = false; }
};
}
static void check(bool ok, const char* text)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) { std::exit(1); }
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("anan-microphone"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    // Thetis networkproto1 bank 10 C1 and network.c CmdTx byte 50 are the wire authority.
    for (int mask = 0; mask < 16; ++mask) {
        const MicOptions options{bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)};
        check(parseMicOptions(micOptions(options)).has_value(), "all microphone choices round-trip");
        const auto packet = buildMicControl(options, 0x12345678);
        check(packet[0] == 0x12 && packet[3] == 0x78 && packet[50] ==
            ((options.ptt ? 0 : 4) | (options.ring ? 8 : 0) | (options.bias ? 16 : 0) | (options.xlr ? 32 : 0)),
            "P2 microphone control matches independent Thetis bit expectations");
        for (std::size_t byte = 5; byte < packet.size(); ++byte) {
            if (byte != 15 && byte != 50) { check(packet[byte] == 0, "P2 microphone packet has no CW/keyer/other control activation"); }
        }
        p1::ReceiveConfig config;
        config.mic = options;
        const auto control = p1::receiveControl(0x14, config);
        if (options.xlr) { check(!control, "P1 rejects unsupported XLR"); }
        else {
            check(control && (*control)[0] == 0x14 && (*control)[1] ==
                ((options.ring ? 16 : 0) | (options.bias ? 32 : 0) | (options.ptt ? 0 : 64)),
                "P1 microphone permission and wiring never set MOX");
        }
    }
    auto invalid = micOptions({});
    invalid[QStringLiteral("bias")] = QStringLiteral("True");
    check(!parseMicOptions(invalid), "string booleans are rejected");
    check(!hasMicWiring(3) && !hasMicWiring(20) && hasMicWiring(4) && hasMicWiring(5)
        && hasXlrMic(10, 2) && !hasXlrMic(10, 1), "hardware availability stays model/protocol bounded");
    for (const int protocol : {1, 2}) {
        AnanBackend backend;
        AnanMicTestAccess::prepare(backend, 4, AnanModel::Anan200D, protocol);
        QSignalSpy results(&backend, &IRadioBackend::extensionResult);
        QSignalSpy errors(&backend, &IRadioBackend::extensionError);
        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("mic.get"), 1, {});
        const auto state = results.last().at(1).toMap();
        check(state.value(QStringLiteral("available")).toBool() && !state.value(QStringLiteral("xlrAvailable")).toBool(),
            "200D provides microphone wiring under both protocols");
        QVariantMap request{{QStringLiteral("identity"), state.value(QStringLiteral("identity"))},
                            {QStringLiteral("options"), micOptions({true, true, true, false})}};
        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("mic.set"), 2, request);
        check(errors.isEmpty() && results.last().at(1).toMap().value(QStringLiteral("options")).toMap()
            .value(QStringLiteral("bias")).toBool(), "valid live choices are accepted and published");
        check(AnanSettings::microphone(QStringLiteral("mic-test"), int(AnanModel::Anan200D), 4, protocol)
            .value(QStringLiteral("ring")).toBool(), "accepted choices persist per exact device/model/protocol");
        check(AnanSettings::microphone(QStringLiteral("other-radio"), int(AnanModel::Anan200D), 4, protocol).isEmpty(),
            "microphone choices do not leak to another radio");
        AnanMicTestAccess::reconnect(backend);
        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("mic.set"), 3, request);
        check(errors.size() == 1, "a prior session cannot write microphone wiring");
        AnanMicTestAccess::prepare(backend, 3, AnanModel::Anan100D, protocol);
        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("mic.set"), 4, request);
        check(errors.size() == 2, "unsupported hardware cannot write microphone choices");
        AnanMicTestAccess::disconnect(backend);
    }
    check(!AnanSettings::setMicrophone(QStringLiteral("no-xlr"), int(AnanModel::Anan200D), 4, 2,
        micOptions({false, false, false, true})), "non-Saturn XLR settings are refused");
    const RadioSettingsScope future(QStringLiteral("anan"), QStringLiteral("future"));
    check(future.setFeature(QStringLiteral("Microphone"), 99, {}), "future-version fixture persists");
    check(!AnanSettings::setMicrophone(QStringLiteral("future"), int(AnanModel::AnanG2), 10, 2, micOptions({})),
        "future schema is never overwritten");
    return 0;
}
