#pragma once

#include "core/asr/AsrSegmenter.h"
#include "core/asr/IAsrBackend.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QThread;

namespace AetherSDR {

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
    void processAudio(const QVector<float>& samples16k);
    void reset();

signals:
    void loaded();
    void loadFailed(const QString& error);
    void segmentText(const QString& text);
    void errorOccurred(const QString& error);

private:
    AsrBackendFactory m_factory;
    std::unique_ptr<IAsrBackend> m_backend;
    AsrSegmenter m_segmenter;
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

    // Feed audio. Ignored unless enabled. Cheap — copies and posts to worker.
    void pushAudio(const QVector<float>& samples16k);
    void reset();

signals:
    void ready();
    void loadFailed(const QString& error);
    void finalText(const QString& text);
    void error(const QString& error);

    // Internal: engine -> worker (queued). Not part of the public contract.
    void requestLoad(const QString& modelPath);
    void requestProcess(const QVector<float>& samples16k);
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
