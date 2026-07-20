// Offline unit test for AsrEngine (RFC #4333, Phase 3). Uses a deterministic
// fake backend injected via the factory, so the engine's worker-thread
// orchestration — async load, audio -> segment -> transcribe -> finalText,
// enabled-gating, and load-failure — is verified without any model or whisper.

#include "asr/AsrEngine.h"
#include "asr/IAsrBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

// Deterministic fake: load() succeeds unless constructed to fail; transcribe()
// returns a fixed phrase for any non-empty utterance.
class FakeBackend : public IAsrBackend {
public:
    explicit FakeBackend(bool loadOk) : m_loadOk(loadOk) {}
    bool load(const QString&, QString* error) override
    {
        if (!m_loadOk) {
            if (error != nullptr) {
                *error = QStringLiteral("fake load failure");
            }
            return false;
        }
        m_loaded = true;
        return true;
    }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>& pcm, QString*) override
    {
        if (pcm.empty()) {
            return {};
        }
        return AsrTranscript{QStringLiteral("OVER"), 0.9f};
    }
    void unload() override { m_loaded = false; }

private:
    bool m_loadOk;
    bool m_loaded = false;
};

AsrBackendFactory factory(bool loadOk)
{
    return [loadOk] { return std::unique_ptr<IAsrBackend>(new FakeBackend(loadOk)); };
}

// Generate at the real RX pipeline rate (24 kHz) so the engine must resample
// 24k -> 16k on its worker before segmenting.
constexpr int kSrcRate = 24000;

QVector<float> tone(int ms, float amp = 0.3f)
{
    QVector<float> v;
    const int n = ms * kSrcRate / 1000;
    for (int i = 0; i < n; ++i) {
        v.push_back(amp * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / kSrcRate)));
    }
    return v;
}

QVector<float> silence(int ms)
{
    return QVector<float>(ms * kSrcRate / 1000, 0.0f);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Async load emits ready() -----------------------------------------
    {
        AsrEngine engine(factory(true));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        QSignalSpy failSpy(&engine, &AsrEngine::loadFailed);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "setModelPath -> ready() emitted");
        expect(failSpy.isEmpty(), "no loadFailed on a good backend");
        expect(engine.isReady(), "engine reports ready");

        // ---- Audio -> segment -> transcribe -> finalText -------------------
        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.setEnabled(true);
        engine.pushAudio(silence(150), kSrcRate);
        engine.pushAudio(tone(500), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate); // trailing silence closes the utterance
        expect(textSpy.wait(5000), "utterance transcribed -> finalText() emitted");
        if (!textSpy.isEmpty()) {
            const auto args = textSpy.first();
            expect(args.at(0).toString() == QStringLiteral("OVER"),
                   "finalText carries the backend's transcription");
            expect(std::abs(args.at(1).toFloat() - 0.9f) < 1e-4f,
                   "finalText carries the backend's confidence");
        }

        // ---- Disabled engine ignores audio --------------------------------
        engine.reset();
        engine.setEnabled(false);
        const int before = textSpy.count();
        engine.pushAudio(tone(500), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        QSignalSpy idle(&engine, &AsrEngine::finalText);
        idle.wait(400); // give the worker a chance; expect nothing
        expect(textSpy.count() == before, "disabled engine emits no finalText");
    }

    // ---- Load failure surfaces loadFailed(), not ready() ------------------
    {
        AsrEngine engine(factory(false));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        QSignalSpy failSpy(&engine, &AsrEngine::loadFailed);
        engine.setModelPath(QStringLiteral("/bad"));
        expect(failSpy.wait(5000), "failing backend -> loadFailed() emitted");
        expect(readySpy.isEmpty(), "no ready() on load failure");
        expect(!engine.isReady(), "engine not ready after load failure");
    }

    std::printf(g_failures == 0 ? "\nASR engine: ALL PASS\n"
                                : "\nASR engine: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
