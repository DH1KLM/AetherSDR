#include "CopyAssistController.h"

#include "CopyAssistPanel.h"

#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"
#include "asr/RemoteAsrBackend.h"
#include "asr/WhisperAsrBackend.h"
#include "core/AppSettings.h"
#include "gui/AsrAudioTap.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QObject>

#include <algorithm>

namespace {

constexpr const char* kRemoteTierId = "remote";

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

AetherSDR::RemoteAsrConfig readRemoteConfig()
{
    auto& s = AetherSDR::AppSettings::instance();
    AetherSDR::RemoteAsrConfig cfg;
    cfg.url = s.value(QStringLiteral("AsrRemoteUrl"), QString()).toString();
    cfg.apiKey = s.value(QStringLiteral("AsrRemoteApiKey"), QString()).toString();
    cfg.model = s.value(QStringLiteral("AsrRemoteModel"), QStringLiteral("whisper-1")).toString();
    cfg.language = QStringLiteral("en");
    return cfg;
}

} // namespace

namespace AetherSDR {

CopyAssistController::CopyAssistController(AudioEngine* audio, CopyAssistPanel* panel,
                                          QObject* parent)
    : QObject(parent)
    , m_audio(audio)
    , m_panel(panel)
    , m_models(new AsrModelManager(this))
{
    // Tier selector: the local model tiers plus a "Remote server…" entry that
    // routes to the RemoteAsrBackend.
    for (const AsrModelTier& tier : AsrModelCatalog::tiers()) {
        m_panel->addTier(tier.id, tier.displayName);
    }
    m_panel->addTier(QString::fromLatin1(kRemoteTierId), tr("Remote server…"));

    // Initial backend: remote if previously configured+enabled, else the
    // GPU-class model when a GPU exists, else the platform default.
    m_remote = AppSettings::instance()
                   .value(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"))
                   .toString() == QStringLiteral("True")
               && !readRemoteConfig().url.isEmpty();
    if (m_remote) {
        m_tierId = QString::fromLatin1(kRemoteTierId);
    } else {
        m_tierId = asrGpuAvailable() ? QStringLiteral("large-v3-turbo")
                                     : AsrModelCatalog::defaultTierId();
    }
    m_panel->setCurrentTier(m_tierId);

    // Panel intent.
    connect(m_panel, &CopyAssistPanel::enableToggled, this, &CopyAssistController::onEnableToggled);
    connect(m_panel, &CopyAssistPanel::tierChanged, this, &CopyAssistController::onTierChanged);

    // Model download → engine load (the handlers read m_asr at call time, so they
    // survive an engine rebuild on backend switch).
    connect(m_models, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        m_panel->setStatus(total > 0
                               ? tr("Downloading model… %1%").arg(static_cast<int>(got * 100 / total))
                               : tr("Downloading model…"));
    });
    connect(m_models, &AsrModelManager::verifying, this,
            [this] { m_panel->setStatus(tr("Verifying model…")); });
    connect(m_models, &AsrModelManager::alreadyPresent, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::finished, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::failed, this, [this](const QString& err) {
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Model download failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });

    // Live VAD tuning (reads m_asr at call time → survives engine rebuild).
    auto& s = AppSettings::instance();
    m_panel->setBufferMs(s.value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_panel->setSensitivity(s.value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt());
    m_panel->setSilenceMs(s.value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
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

    buildEngine();
}

CopyAssistController::~CopyAssistController() = default;

void CopyAssistController::buildEngine()
{
    // Tear down any previous engine+tap (order: tap first — it references the
    // engine) and rebuild for the current backend.
    delete m_tap;
    m_tap = nullptr;
    delete m_asr;

    m_asr = m_remote ? new AsrEngine(remoteAsrBackendFactory(readRemoteConfig()), this)
                     : new AsrEngine(whisperAsrBackendFactory(), this);
    m_tap = new AsrAudioTap(m_audio, m_asr, this);

    connect(m_asr, &AsrEngine::ready, this, [this] {
        m_panel->setBusy(false);
        if (m_enabled) {
            m_tap->setEnabled(true);
            m_panel->setStatus(m_remote ? tr("Listening (remote)…") : tr("Listening…"));
        }
    });
    connect(m_asr, &AsrEngine::loadFailed, this, [this](const QString& err) {
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Model load failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });
    connect(m_asr, &AsrEngine::finalText, m_panel, &CopyAssistPanel::appendText);
    connect(m_asr, &AsrEngine::error, this, [this](const QString& err) { m_panel->setStatus(err); });

    applyTuning();
}

void CopyAssistController::applyTuning()
{
    auto& s = AppSettings::instance();
    m_asr->setDecodeBufferMs(s.value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_asr->setSpeechRms(sensitivityToRms(
        s.value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt()));
    m_asr->setSilenceDurationMs(s.value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
}

void CopyAssistController::onEnableToggled(bool on)
{
    m_enabled = on;
    if (on) {
        beginEnable();
    } else {
        m_tap->setEnabled(false);
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Disabled"));
    }
}

void CopyAssistController::onTierChanged(const QString& tierId)
{
    if (tierId == m_tierId) {
        return;
    }

    if (tierId == QString::fromLatin1(kRemoteTierId)) {
        if (!promptRemoteConfig()) {
            m_panel->setCurrentTier(m_tierId); // user cancelled — revert
            return;
        }
        m_tierId = tierId;
        m_remote = true;
        m_tap->setEnabled(false);
        buildEngine();
    } else {
        m_tierId = tierId;
        if (m_remote) {
            m_remote = false;
            AppSettings::instance().setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"));
            AppSettings::instance().save();
            m_tap->setEnabled(false);
            buildEngine();
        }
    }

    if (m_enabled) {
        m_tap->setEnabled(false);
        beginEnable();
    }
}

void CopyAssistController::beginEnable()
{
    m_panel->setBusy(true);
    if (m_remote) {
        // No local model to fetch — the remote endpoint is contacted per
        // utterance. load() just marks the backend ready.
        m_panel->setStatus(tr("Connecting to remote server…"));
        m_asr->setModelPath(QString());
    } else {
        m_panel->setStatus(tr("Preparing model…"));
        requestModel(m_tierId);
    }
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

bool CopyAssistController::promptRemoteConfig()
{
    const RemoteAsrConfig current = readRemoteConfig();

    QDialog dialog(m_panel);
    dialog.setWindowTitle(tr("Remote ASR Server"));
    auto* form = new QFormLayout(&dialog);

    auto* urlEdit = new QLineEdit(current.url, &dialog);
    urlEdit->setPlaceholderText(tr("http://host:8080/v1/audio/transcriptions"));
    urlEdit->setMinimumWidth(360);
    form->addRow(tr("Endpoint URL:"), urlEdit);

    auto* keyEdit = new QLineEdit(current.apiKey, &dialog);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText(tr("optional (Bearer token)"));
    form->addRow(tr("API key:"), keyEdit);

    auto* modelEdit = new QLineEdit(current.model, &dialog);
    form->addRow(tr("Model:"), modelEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted || urlEdit->text().trimmed().isEmpty()) {
        return false;
    }

    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("AsrRemoteUrl"), urlEdit->text().trimmed());
    s.setValue(QStringLiteral("AsrRemoteApiKey"), keyEdit->text());
    s.setValue(QStringLiteral("AsrRemoteModel"), modelEdit->text().trimmed());
    s.setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("True"));
    s.save();
    return true;
}

} // namespace AetherSDR
