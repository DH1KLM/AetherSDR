#pragma once

#include "core/asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>

struct whisper_context;

namespace AetherSDR {

// whisper.cpp implementation of IAsrBackend. CPU inference (the vendored ggml
// is CPU-only for now); language defaults to English but is configurable.
// Lives entirely on AsrEngine's worker thread.
class WhisperAsrBackend : public IAsrBackend {
public:
    WhisperAsrBackend();
    explicit WhisperAsrBackend(QString language);
    ~WhisperAsrBackend() override;

    bool load(const QString& modelPath, QString* error) override;
    bool isLoaded() const override { return m_ctx != nullptr; }
    QString transcribe(const std::vector<float>& pcm16k, QString* error) override;
    void unload() override;

    void setLanguage(const QString& language) { m_language = language; }

private:
    whisper_context* m_ctx = nullptr;
    QString m_language;
    int m_threads = 0;
};

// Factory for wiring AsrEngine to the production whisper backend. Kept here so
// AsrEngine.cpp never references whisper (keeping the engine — and its unit
// test — independent of the vendored library). Matches AsrBackendFactory.
std::function<std::unique_ptr<IAsrBackend>()>
whisperAsrBackendFactory(const QString& language = QStringLiteral("en"));

} // namespace AetherSDR
