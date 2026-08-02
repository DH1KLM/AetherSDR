#include "core/VaraTransmitter.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

using namespace AetherSDR::Vara;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString findModelPath()
{
    for (const char* candidate : {"data/vara/vara_model.json",
                                  "../data/vara/vara_model.json",
                                  "../../data/vara/vara_model.json"}) {
        if (QFile::exists(QString::fromLatin1(candidate)))
            return QString::fromLatin1(candidate);
    }
    return {};
}

void testTxInhibit()
{
    Transmitter tx;
    // The default is the safety property: a Transmitter nobody has deliberately
    // armed must not be able to key (Constitution VI). Asserted first, because
    // every other case below can pass with the default the wrong way round.
    expect(tx.isTransmitInhibited(),
           "a freshly constructed Transmitter is INHIBITED by default");
    expect(tx.synthesizeAckBurst(true, 1).isEmpty(),
           "an unarmed Transmitter refuses to synthesise");

    tx.setTransmitInhibited(true);
    expect(tx.isTransmitInhibited(), "Transmitter reports TX inhibited");

    QByteArray payload = "Hello World";
    QVector<float> audio = tx.synthesizeOfdmDataFrame(payload, 10);
    expect(audio.isEmpty(), "TX Inhibit refuses OFDM frame synthesis");

    QVector<float> ack = tx.synthesizeAckBurst(true, 1);
    expect(ack.isEmpty(), "TX Inhibit refuses ACK burst synthesis");

    tx.setTransmitInhibited(false);
    expect(!tx.isTransmitInhibited(), "Transmitter reports TX permitted after reset");
}

// The arm is a safety property, so it gets its own assertions rather than
// riding on the synthesis tests. Each maps to one of the four requirements
// documented in VaraTransmitter.h.
void testTransmitArm()
{
    Transmitter tx;

    // (1) fail closed
    expect(tx.isTransmitInhibited(), "a fresh Transmitter is inhibited");
    expect(!tx.isArmed(), "a fresh Transmitter is not armed");

    // (4) the gate is at the choke point, not only the synthesis entries: a
    // caller holding a pre-built buffer must not be able to route around it.
    QVector<float> prebuilt(256, 0.5f);
    expect(tx.packageBurstWithPtt(prebuilt).isEmpty(),
           "packageBurstWithPtt refuses a pre-built buffer while unarmed");

    // (2) arming is deliberate and explicit
    tx.armForSession();
    expect(tx.isArmed(), "armForSession() arms the transmitter");
    expect(!tx.packageBurstWithPtt(prebuilt).isEmpty(),
           "an armed transmitter packages a burst");

    tx.disarm();
    expect(!tx.isArmed(), "disarm() re-inhibits");
    expect(tx.packageBurstWithPtt(prebuilt).isEmpty(),
           "packageBurstWithPtt refuses again after disarm");
}

void testOfdmSynthesis()
{
    Transmitter tx;
    tx.setTransmitInhibited(false);   // fail-closed: arming is deliberate
    tx.setBandwidth(Bandwidth::Bw2300);

    QByteArray payload(64, 'A');
    QVector<float> audio = tx.synthesizeOfdmDataFrame(payload, 10); // 16-QAM Level 10

    expect(!audio.isEmpty(), "OFDM Level 10 frame synthesised successfully");

    int expectedSamples = (tx.pttLeadTimeMs() * 48000) / 1000 +
                          OfdmWaveform::kFrameSymbols * OfdmWaveform::kSymbolSamples +
                          (tx.pttTailTimeMs() * 48000) / 1000;
    expect(audio.size() == expectedSamples, "Audio length matches Lead + 153 Symbols + Tail");
}

void testMfskSynthesis()
{
    Transmitter tx;
    tx.setTransmitInhibited(false);   // fail-closed: arming is deliberate
    QString modelPath = findModelPath();
    if (!modelPath.isEmpty()) {
        bool loaded = tx.loadModel(modelPath);
        expect(loaded, "MFSK generator model loaded");

        QByteArray payload(32, 0);
        QVector<float> audio = tx.synthesizeMfskDataFrame(payload);
        expect(!audio.isEmpty(), "MFSK Level 1 frame synthesised successfully");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("=== Running vara_transmitter_test ===\n");
    testTxInhibit();
    testTransmitArm();
    testOfdmSynthesis();
    testMfskSynthesis();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
