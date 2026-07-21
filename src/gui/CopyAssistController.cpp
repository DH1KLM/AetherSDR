#include "CopyAssistController.h"

#include "CopyAssistPanel.h"

#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"
#include "asr/WhisperAsrBackend.h"
#include "core/AppSettings.h"
#include "gui/AsrAudioTap.h"

#include <QObject>

#include <algorithm>

namespace {
// Map a 1–100 "sensitivity" (higher = more sensitive) to the VAD's RMS energy
// threshold (lower = more sensitive), spanning a practical HF-voice range.
float sensitivityToRms(int percent)
{
    percent = std::clamp(percent, 1, 100);
    constexpr float leastSensitive = 0.050f;
    constexpr float mostSensitive = 0.001f;
    return leastSensitive - (percent - 1) / 99.0f * (leastSensitive - mostSensitive);
}

void saveInt(const char* key, int value)
{
    auto& s = AetherSDR::AppSettings::instance();
    s.setValue(QString::fromLatin1(key), QString::number(value));
    s.save();
}
} // namespace

namespace AetherSDR {

CopyAssistController::CopyAssistController(AudioEngine* audio, CopyAssistPanel* panel,
                                          QObject* parent)
    : QObject(parent)
    , m_panel(panel)
    , m_asr(new AsrEngine(whisperAsrBackendFactory(), this))
    , m_models(new AsrModelManager(this))
    , m_tap(new AsrAudioTap(audio, m_asr, this))
{
    // Populate the tier selector from the catalog and default per platform.
    for (const AsrModelTier& tier : AsrModelCatalog::tiers()) {
        m_panel->addTier(tier.id, tier.displayName);
    }
    m_tierId = AsrModelCatalog::defaultTierId();
    m_panel->setCurrentTier(m_tierId);

    // Panel intent.
    connect(m_panel, &CopyAssistPanel::enableToggled, this,
            &CopyAssistController::onEnableToggled);
    connect(m_panel, &CopyAssistPanel::tierChanged, this,
            &CopyAssistController::onTierChanged);

    // Model download → engine load.
    connect(m_models, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        if (total > 0) {
            m_panel->setStatus(tr("Downloading model… %1%")
                                   .arg(static_cast<int>(got * 100 / total)));
        } else {
            m_panel->setStatus(tr("Downloading model…"));
        }
    });
    connect(m_models, &AsrModelManager::alreadyPresent, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::finished, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::failed, this, [this](const QString& err) {
        m_panel->setStatus(tr("Model download failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });

    // Engine lifecycle.
    connect(m_asr, &AsrEngine::ready, this, [this] {
        if (m_enabled) {
            m_tap->setEnabled(true);        // start feeding RX audio
            m_panel->setStatus(tr("Listening…"));
        }
    });
    connect(m_asr, &AsrEngine::loadFailed, this, [this](const QString& err) {
        m_panel->setStatus(tr("Model load failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });
    connect(m_asr, &AsrEngine::finalText, m_panel, &CopyAssistPanel::appendText);
    connect(m_asr, &AsrEngine::error, this, [this](const QString& err) {
        m_panel->setStatus(err);
    });

    // --- Segmentation tuning: load persisted values, apply, then wire live ---
    auto& s = AppSettings::instance();
    const int bufferMs = s.value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000"))
                             .toString().toInt();
    const int sensitivity = s.value(QStringLiteral("AsrSensitivity"), QStringLiteral("80"))
                                .toString().toInt();
    const int silenceMs = s.value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300"))
                              .toString().toInt();

    m_asr->setDecodeBufferMs(bufferMs);
    m_asr->setSpeechRms(sensitivityToRms(sensitivity));
    m_asr->setSilenceDurationMs(silenceMs);

    // Reflect in the panel BEFORE wiring the change signals, so restoring saved
    // state doesn't immediately re-persist it.
    m_panel->setBufferMs(bufferMs);
    m_panel->setSensitivity(sensitivity);
    m_panel->setSilenceMs(silenceMs);

    connect(m_panel, &CopyAssistPanel::bufferMsChanged, this, [this](int ms) {
        m_asr->setDecodeBufferMs(ms);
        saveInt("AsrDecodeBufferMs", ms);
    });
    connect(m_panel, &CopyAssistPanel::sensitivityChanged, this, [this](int pct) {
        m_asr->setSpeechRms(sensitivityToRms(pct));
        saveInt("AsrSensitivity", pct);
    });
    connect(m_panel, &CopyAssistPanel::silenceMsChanged, this, [this](int ms) {
        m_asr->setSilenceDurationMs(ms);
        saveInt("AsrSilenceMs", ms);
    });
}

CopyAssistController::~CopyAssistController() = default;

void CopyAssistController::onEnableToggled(bool on)
{
    m_enabled = on;
    if (on) {
        beginEnable();
    } else {
        m_tap->setEnabled(false);
        m_panel->setStatus(tr("Disabled"));
    }
}

void CopyAssistController::onTierChanged(const QString& tierId)
{
    if (tierId == m_tierId) {
        return;
    }
    m_tierId = tierId;
    if (m_enabled) {
        // Switch models live: stop feeding, fetch/load the new tier.
        m_tap->setEnabled(false);
        beginEnable();
    }
}

void CopyAssistController::beginEnable()
{
    m_panel->setStatus(tr("Preparing model…"));
    requestModel(m_tierId);
}

void CopyAssistController::requestModel(const QString& tierId)
{
    const AsrModelTier* tier = AsrModelCatalog::tierById(tierId);
    if (tier == nullptr) {
        m_panel->setStatus(tr("Unknown model tier: %1").arg(tierId));
        m_panel->setAsrEnabled(false);
        return;
    }
    m_models->ensure(*tier); // emits alreadyPresent / finished / failed
}

} // namespace AetherSDR
