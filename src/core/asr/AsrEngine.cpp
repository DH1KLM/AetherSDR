#include "core/asr/AsrEngine.h"

#include <QLoggingCategory>
#include <QThread>

#include <utility>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrEngine, "aether.asr.engine")

// ---- AsrWorker -------------------------------------------------------------

AsrWorker::AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig)
    : m_factory(std::move(factory))
    , m_segmenter(segConfig)
{
}

AsrWorker::~AsrWorker()
{
    if (m_backend != nullptr) {
        m_backend->unload();
    }
}

void AsrWorker::init()
{
    m_backend = m_factory ? m_factory() : nullptr;
    if (m_backend == nullptr) {
        emit errorOccurred(QStringLiteral("ASR backend could not be created."));
    }
}

void AsrWorker::loadModel(const QString& modelPath)
{
    if (m_backend == nullptr) {
        emit loadFailed(QStringLiteral("No ASR backend."));
        return;
    }
    QString error;
    if (m_backend->load(modelPath, &error)) {
        m_warnedNoModel = false;
        m_segmenter.reset();
        emit loaded();
    } else {
        emit loadFailed(error);
    }
}

void AsrWorker::processAudio(const QVector<float>& samples16k)
{
    if (samples16k.isEmpty()) {
        return;
    }

    std::vector<std::vector<float>> segments =
        m_segmenter.feed(samples16k.constData(), samples16k.size());
    if (segments.empty()) {
        return;
    }

    if (m_backend == nullptr || !m_backend->isLoaded()) {
        if (!m_warnedNoModel) {
            m_warnedNoModel = true;
            emit errorOccurred(QStringLiteral("Speech detected but no ASR model is loaded."));
        }
        return;
    }

    for (std::vector<float>& seg : segments) {
        QString error;
        const QString text = m_backend->transcribe(seg, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            continue;
        }
        if (!text.isEmpty()) {
            emit segmentText(text);
        }
    }
}

void AsrWorker::reset()
{
    m_segmenter.reset();
}

// ---- AsrEngine -------------------------------------------------------------

AsrEngine::AsrEngine(AsrBackendFactory factory, QObject* parent)
    : AsrEngine(std::move(factory), AsrSegmenter::Config{}, parent)
{
}

AsrEngine::AsrEngine(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
                     QObject* parent)
    : QObject(parent)
{
    startThread(std::move(factory), segConfig);
}

void AsrEngine::startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig)
{
    m_thread = new QThread(this);
    m_worker = new AsrWorker(std::move(factory), segConfig);
    m_worker->moveToThread(m_thread);

    // Worker lifecycle: create the backend once the thread is running. The
    // worker is deleted manually after quit()+wait() in the destructor (a
    // finished->deleteLater would need a live event loop that no longer exists
    // once the thread has stopped), so it is intentionally parentless.
    connect(m_thread, &QThread::started, m_worker, &AsrWorker::init);

    // Engine -> worker (queued across threads).
    connect(this, &AsrEngine::requestLoad, m_worker, &AsrWorker::loadModel);
    connect(this, &AsrEngine::requestProcess, m_worker, &AsrWorker::processAudio);
    connect(this, &AsrEngine::requestReset, m_worker, &AsrWorker::reset);

    // Worker -> engine (queued back to the main thread).
    connect(m_worker, &AsrWorker::loaded, this, [this] {
        m_ready = true;
        emit ready();
    });
    connect(m_worker, &AsrWorker::loadFailed, this, [this](const QString& err) {
        m_ready = false;
        emit loadFailed(err);
    });
    connect(m_worker, &AsrWorker::segmentText, this, &AsrEngine::finalText);
    connect(m_worker, &AsrWorker::errorOccurred, this, &AsrEngine::error);

    m_thread->start();
}

AsrEngine::~AsrEngine()
{
    if (m_thread != nullptr) {
        m_thread->quit();
        m_thread->wait();
        // m_worker is deleted via the thread's finished -> deleteLater; but that
        // slot won't run without an event loop here, so delete it directly now
        // that the thread has stopped.
        delete m_worker;
        m_worker = nullptr;
    }
}

void AsrEngine::setModelPath(const QString& modelPath)
{
    m_modelPath = modelPath;
    m_ready = false;
    emit requestLoad(modelPath);
}

void AsrEngine::pushAudio(const QVector<float>& samples16k)
{
    if (!m_enabled || samples16k.isEmpty()) {
        return;
    }
    emit requestProcess(samples16k);
}

void AsrEngine::reset()
{
    emit requestReset();
}

} // namespace AetherSDR
