// Offline unit test for AsrEngine (RFC #4333, Phase 3). Uses a deterministic
// fake backend injected via the factory, so the engine's worker-thread
// orchestration — async load, audio -> segment -> transcribe -> finalText,
// enabled-gating, and load-failure — is verified without any model or whisper.

#include "core/asr/AsrEngine.h"
#include "core/asr/IAsrBackend.h"

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
    QString transcribe(const std::vector<float>& pcm, QString*) override
    {
        return pcm.empty() ? QString() : QStringLiteral("OVER");
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

QVector<float> tone(int ms, float amp = 0.3f)
{
    QVector<float> v;
    const int n = ms * 16000 / 1000;
    for (int i = 0; i < n; ++i) {
        v.push_back(amp * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / 16000.0)));
    }
    return v;
}

QVector<float> silence(int ms)
{
    return QVector<float>(ms * 16000 / 1000, 0.0f);
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
        engine.pushAudio(silence(150));
        engine.pushAudio(tone(500));
        engine.pushAudio(silence(400)); // trailing silence closes the utterance
        expect(textSpy.wait(5000), "utterance transcribed -> finalText() emitted");
        if (!textSpy.isEmpty()) {
            expect(textSpy.first().first().toString() == QStringLiteral("OVER"),
                   "finalText carries the backend's transcription");
        }

        // ---- Disabled engine ignores audio --------------------------------
        engine.reset();
        engine.setEnabled(false);
        const int before = textSpy.count();
        engine.pushAudio(tone(500));
        engine.pushAudio(silence(400));
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
