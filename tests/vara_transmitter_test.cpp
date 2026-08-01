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

void testOfdmSynthesis()
{
    Transmitter tx;
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
    testOfdmSynthesis();
    testMfskSynthesis();

    std::printf("Summary: %d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
