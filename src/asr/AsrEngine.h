#pragma once

#include "asr/AsrSegmenter.h"
#include "asr/IAsrBackend.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QThread;

namespace AetherSDR {

class Resampler;

// Factory that constructs an ASR backend. Invoked on the worker thread so the
// backend (and any model context) lives entirely there.
using AsrBackendFactory = std::function<std::unique_ptr<IAsrBackend>()>;

// Worker half of the ASR engine — runs on a dedicated thread, owns the backend
// and the segmenter, and does all CPU-heavy work (segmentation + inference).
// Never touched directly by callers; AsrEngine marshals to it via queued
// signals. (Declared here so AUTOMOC sees its Q_OBJECT.)
class AsrWorker : public QObject {
    Q_OBJECT
public:
    AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig);
    ~AsrWorker() override;

public slots:
    void init();                                   // create backend on this thread
    void loadModel(const QString& modelPath);
    // Mono float samples at sampleRate; resampled to whisper's 16 kHz on this
    // (worker) thread before segmentation — never on the audio/caller thread.
    void processAudio(const QVector<float>& monoSamples, int sampleRate);
    void reset();

signals:
    void loaded();
    void loadFailed(const QString& error);
    void segmentText(const QString& text);
    void errorOccurred(const QString& error);

private:
    // Resample arbitrary-rate mono to 16 kHz mono (returns the input unchanged
    // when already 16 kHz). Builds/rebuilds the r8brain resampler on rate change.
    std::vector<float> toSixteenK(const QVector<float>& monoSamples, int sampleRate);

    AsrBackendFactory m_factory;
    std::unique_ptr<IAsrBackend> m_backend;
    AsrSegmenter m_segmenter;
    std::unique_ptr<Resampler> m_resampler;
    int m_resamplerSrcRate = 0;
    bool m_warnedNoModel = false;
};

// Engine half — main-thread facing. Accepts 16 kHz mono audio, ships it to the
// worker, and re-emits transcription results. Threading obeys the project rule:
// worker communicates only via auto-queued signals; no shared mutable state,
// no work on the audio callback (pushAudio only copies + posts).
class AsrEngine : public QObject {
    Q_OBJECT
public:
    // The only constructor: inject the backend factory. Production code passes
    // whisperAsrBackendFactory(); tests pass a deterministic fake.
    explicit AsrEngine(AsrBackendFactory factory, QObject* parent = nullptr);
    AsrEngine(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
              QObject* parent = nullptr);
    ~AsrEngine() override;

    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }

    // Load/switch the model file (async; emits ready() or loadFailed()).
    void setModelPath(const QString& modelPath);
    QString modelPath() const { return m_modelPath; }
    bool isReady() const { return m_ready; }

    // Feed mono audio at its native sampleRate (e.g. the 24 kHz RX pipeline).
    // Ignored unless enabled. Cheap — copies and posts to the worker, which
    // resamples to 16 kHz. No work happens on the caller/audio thread.
    void pushAudio(const QVector<float>& monoSamples, int sampleRate);
    void reset();

signals:
    void ready();
    void loadFailed(const QString& error);
    void finalText(const QString& text);
    void error(const QString& error);

    // Internal: engine -> worker (queued). Not part of the public contract.
    void requestLoad(const QString& modelPath);
    void requestProcess(const QVector<float>& monoSamples, int sampleRate);
    void requestReset();

private:
    void startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig);

    QThread* m_thread = nullptr;
    AsrWorker* m_worker = nullptr;
    bool m_enabled = false;
    bool m_ready = false;
    QString m_modelPath;
};

} // namespace AetherSDR
