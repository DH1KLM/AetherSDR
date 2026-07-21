#include "asr/AsrEngine.h"

#include "asr/SileroVad.h"
#include "core/Resampler.h"

#include <QLoggingCategory>
#include <QThread>

#include <algorithm>
#include <utility>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrEngine, "aether.asr.engine")

namespace {
constexpr int kAsrRate = 16000;      // whisper's required rate
constexpr int kResampleBlock = 4096; // max samples per r8brain process() call
// Wide transition band -> short FIR -> low latency. Speech lives well below the
// 8 kHz downsampled Nyquist, so a 10%-of-Nyquist guard is ample and keeps the
// resampler's group delay small (important for live copy and for prompt segment
// close-out).
constexpr double kResampleTransBand = 10.0;
} // namespace

// ---- AsrWorker -------------------------------------------------------------

AsrWorker::AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig)
    : m_factory(std::move(factory))
    , m_segmenter(segConfig)
    , m_vadModelPath(segConfig.vadModelPath)
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

    // Build the learned VAD on the worker thread (the ONNX session must live
    // here). On any failure, leave the segmenter on its energy VAD.
    if (!m_vadModelPath.empty()) {
        auto vad = std::make_unique<SileroVad>();
        if (vad->load(m_vadModelPath)) {
            m_vad = std::move(vad);
            m_segmenter.setVad(m_vad.get());
            qCInfo(lcAsrEngine, "ASR: Silero VAD loaded from %s",
                   m_vadModelPath.c_str());
        } else {
            qCWarning(lcAsrEngine, "ASR: Silero VAD load failed (%s) — using energy VAD",
                      m_vadModelPath.c_str());
        }
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

std::vector<float> AsrWorker::toSixteenK(const QVector<float>& monoSamples, int sampleRate)
{
    const int rate = sampleRate > 0 ? sampleRate : kAsrRate;
    if (rate == kAsrRate) {
        return std::vector<float>(monoSamples.constBegin(), monoSamples.constEnd());
    }

    // Rebuild the resampler if the source rate changed (or first use).
    if (!m_resampler || m_resamplerSrcRate != rate) {
        m_resampler = std::make_unique<Resampler>(static_cast<double>(rate),
                                                  static_cast<double>(kAsrRate), kResampleBlock,
                                                  kResampleTransBand);
        m_resamplerSrcRate = rate;
    }

    // r8brain processes at most kResampleBlock samples per call; chunk the input.
    const int total = static_cast<int>(monoSamples.size());
    std::vector<float> out;
    out.reserve(static_cast<size_t>(total) * kAsrRate / rate + 16);
    for (int off = 0; off < total; off += kResampleBlock) {
        const int n = std::min(kResampleBlock, total - off);
        const QByteArray block = m_resampler->process(monoSamples.constData() + off, n);
        const auto* f = reinterpret_cast<const float*>(block.constData());
        const int count = block.size() / static_cast<int>(sizeof(float));
        out.insert(out.end(), f, f + count);
    }
    return out;
}

void AsrWorker::processAudio(const QVector<float>& monoSamples, int sampleRate)
{
    if (monoSamples.isEmpty()) {
        return;
    }

    const std::vector<float> pcm16k = toSixteenK(monoSamples, sampleRate);
    if (pcm16k.empty()) {
        return;
    }

    std::vector<std::vector<float>> segments =
        m_segmenter.feed(pcm16k.data(), static_cast<int>(pcm16k.size()));
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
        const AsrTranscript result = m_backend->transcribe(seg, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            continue;
        }
        if (!result.text.isEmpty()) {
            emit segmentText(result.text, result.confidence);
        }
    }
}

void AsrWorker::setMaxSegmentMs(int ms)
{
    m_segmenter.setMaxSegmentMs(ms);
}

void AsrWorker::setSpeechRms(float rms)
{
    m_segmenter.setSpeechRms(rms);
}

void AsrWorker::setHangoverMs(int ms)
{
    m_segmenter.setHangoverMs(ms);
}

void AsrWorker::reset()
{
    m_segmenter.reset();
    m_resampler.reset();
    m_resamplerSrcRate = 0;
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
    connect(this, &AsrEngine::requestSetMaxSegmentMs, m_worker, &AsrWorker::setMaxSegmentMs);
    connect(this, &AsrEngine::requestSetSpeechRms, m_worker, &AsrWorker::setSpeechRms);
    connect(this, &AsrEngine::requestSetHangoverMs, m_worker, &AsrWorker::setHangoverMs);
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

void AsrEngine::pushAudio(const QVector<float>& monoSamples, int sampleRate)
{
    if (!m_enabled || monoSamples.isEmpty()) {
        return;
    }
    emit requestProcess(monoSamples, sampleRate);
}

void AsrEngine::setDecodeBufferMs(int ms)
{
    emit requestSetMaxSegmentMs(ms);
}

void AsrEngine::setSpeechRms(float rms)
{
    emit requestSetSpeechRms(rms);
}

void AsrEngine::setSilenceDurationMs(int ms)
{
    emit requestSetHangoverMs(ms);
}

void AsrEngine::reset()
{
    emit requestReset();
}

} // namespace AetherSDR
