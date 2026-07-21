#include "CopyAssistController.h"

#include "CopyAssistPanel.h"

#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"
#include "asr/RemoteAsrBackend.h"
#include "asr/WhisperAsrBackend.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/AsrAudioTap.h"

#include <QPushButton>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QObject>
#include <QStandardPaths>

#include <algorithm>

namespace {

constexpr const char* kRemoteTierId = "remote";
constexpr const char* kCustomTierId = "custom";

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
    // Tier selector: the downloadable model tiers, then a "Custom model…" entry
    // for a user-supplied local .bin/.gguf, then a "Remote server…" entry that
    // routes to the RemoteAsrBackend.
    for (const AsrModelTier& tier : AsrModelCatalog::tiers()) {
        m_panel->addTier(tier.id, tier.displayName);
    }
    m_panel->addTier(QString::fromLatin1(kCustomTierId), tr("Custom model…"));
    m_panel->addTier(QString::fromLatin1(kRemoteTierId), tr("Remote server…"));

    // Remember a previously-picked custom model so its filename shows in the list
    // (and the file-picker defaults to it) across restarts.
    m_customModelPath =
        AppSettings::instance().value(QStringLiteral("AsrCustomModelPath"), QString()).toString();
    if (!m_customModelPath.isEmpty()) {
        m_panel->setTierLabel(QString::fromLatin1(kCustomTierId),
                              tr("Custom: %1").arg(QFileInfo(m_customModelPath).fileName()));
    }

    // Initial backend: remote if previously configured+enabled, else the
    // GPU-class model when a GPU exists, else the platform default.
    const bool remoteConfigured =
        AppSettings::instance()
                .value(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"))
                .toString() == QStringLiteral("True")
        && !readRemoteConfig().url.isEmpty();
    if (remoteConfigured) {
        m_backend = AsrBackendKind::Remote;
        m_tierId = QString::fromLatin1(kRemoteTierId);
    } else {
        m_backend = AsrBackendKind::Whisper;
        m_tierId = asrGpuAvailable() ? QStringLiteral("large-v3-turbo")
                                     : AsrModelCatalog::defaultTierId();
    }
    m_panel->setCurrentTier(m_tierId);

    // Style the checkable toggles (Enable/Disable and the ↵ newline toggle) like
    // the applet toggle buttons: the checked state fills with the dim-cyan accent
    // so the on state is visibly distinct.
    const QString appletToggleStyle = QStringLiteral(
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.background.2}}; border-radius: 3px;"
        " padding: 3px 10px; font-weight: bold; color: {{color.text.primary}}; }"
        "QPushButton:hover { background: {{color.background.2}}; }"
        "QPushButton:checked { background: {{color.accent.dim}};"
        " color: {{color.text.primary}}; border: 1px solid {{color.accent.bright}}; }");
    ThemeManager::instance().applyStyleSheet(m_panel->enableButton(), appletToggleStyle);
    ThemeManager::instance().applyStyleSheet(m_panel->newlineButton(), appletToggleStyle);

    // Compute-device selector — shown whenever a GPU exists, so the user can pick
    // a GPU (or several) or force CPU. Hidden on GPU-less hosts (always CPU).
    const std::vector<AsrGpuDevice> gpus = asrGpuDevices();
    if (!gpus.empty()) {
        for (const AsrGpuDevice& g : gpus) {
            m_panel->addGpuDevice(g.index, g.name);
        }
        m_panel->addGpuDevice(-1, tr("CPU")); // force-CPU option
        int saved = AppSettings::instance()
                        .value(QStringLiteral("AsrGpuDevice"), QStringLiteral("0")).toString().toInt();
        if (saved != -1 && (saved < 0 || saved >= static_cast<int>(gpus.size()))) {
            saved = 0;
        }
        m_panel->setCurrentGpu(saved);
        m_panel->setGpuSelectorVisible(true);
    }

    // Panel intent.
    connect(m_panel, &CopyAssistPanel::enableToggled, this, &CopyAssistController::onEnableToggled);
    connect(m_panel, &CopyAssistPanel::tierChanged, this, &CopyAssistController::onTierChanged);
    connect(m_panel, &CopyAssistPanel::gpuChanged, this, [this](int index) {
        saveInt("AsrGpuDevice", index);
        if (m_backend != AsrBackendKind::Remote) {
            m_tap->setEnabled(false);
            buildEngine(); // rebuild the local engine on the chosen GPU
            if (m_enabled) {
                beginEnable();
            }
        }
    });

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
    m_panel->setFontPx(s.value(QStringLiteral("AsrFontPx"), QStringLiteral("13")).toString().toInt());
    m_panel->setNewlineOnSilence(
        s.value(QStringLiteral("AsrNewlineOnSilence"), QStringLiteral("False")).toString()
        == QStringLiteral("True"));
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
    connect(m_panel, &CopyAssistPanel::fontPxChanged, this,
            [](int px) { saveInt("AsrFontPx", px); });
    connect(m_panel, &CopyAssistPanel::newlineOnSilenceChanged, this, [](bool on) {
        auto& st = AppSettings::instance();
        st.setValue(QStringLiteral("AsrNewlineOnSilence"),
                    on ? QStringLiteral("True") : QStringLiteral("False"));
        st.save();
    });

    buildEngine();
}

CopyAssistController::~CopyAssistController() = default;

void CopyAssistController::clearDecode()
{
    m_panel->clearText();
    m_asr->reset(); // drop any half-built utterance so it doesn't cross frequencies
}

void CopyAssistController::buildEngine()
{
    // Tear down any previous engine+tap (order: tap first — it references the
    // engine) and rebuild for the current backend.
    delete m_tap;
    m_tap = nullptr;
    delete m_asr;

    const int gpuDevice = AppSettings::instance()
                              .value(QStringLiteral("AsrGpuDevice"), QStringLiteral("0"))
                              .toString().toInt();
    switch (m_backend) {
    case AsrBackendKind::Remote:
        m_asr = new AsrEngine(remoteAsrBackendFactory(readRemoteConfig()), this);
        break;
    case AsrBackendKind::Whisper:
        m_asr = new AsrEngine(whisperAsrBackendFactory(QStringLiteral("en"), gpuDevice), this);
        break;
    }
    m_tap = new AsrAudioTap(m_audio, m_asr, this);

    connect(m_asr, &AsrEngine::ready, this, [this] {
        m_panel->setBusy(false);
        if (m_enabled) {
            m_tap->setEnabled(true);
            m_panel->setStatus(m_backend == AsrBackendKind::Remote ? tr("Listening (remote)…")
                                                                   : tr("Listening…"));
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
        setBackend(AsrBackendKind::Remote, tierId);
    } else if (tierId == QString::fromLatin1(kCustomTierId)) {
        const QString path = promptCustomModel();
        if (path.isEmpty()) {
            m_panel->setCurrentTier(m_tierId); // user cancelled — revert
            return;
        }
        m_customModelPath = path;
        AppSettings::instance().setValue(QStringLiteral("AsrCustomModelPath"), path);
        AppSettings::instance().save();
        m_panel->setTierLabel(QString::fromLatin1(kCustomTierId),
                              tr("Custom: %1").arg(QFileInfo(path).fileName()));
        setBackend(AsrBackendKind::Whisper, tierId);
    } else {
        setBackend(backendForTier(tierId), tierId);
    }

    if (m_enabled) {
        m_tap->setEnabled(false);
        beginEnable();
    }
}

AsrBackendKind CopyAssistController::backendForTier(const QString& tierId)
{
    if (tierId == QString::fromLatin1(kRemoteTierId)) {
        return AsrBackendKind::Remote;
    }
    // A catalog tier routes by its declared engine family; the "custom" file and
    // any unknown id fall through to local whisper.
    if (const AsrModelTier* tier = AsrModelCatalog::tierById(tierId)) {
        switch (tier->family) {
        case AsrModelFamily::Whisper:
            return AsrBackendKind::Whisper;
        }
    }
    return AsrBackendKind::Whisper;
}

void CopyAssistController::setBackend(AsrBackendKind kind, const QString& tierId)
{
    const AsrBackendKind prev = m_backend;
    m_backend = kind;
    m_tierId = tierId;

    // Leaving the remote backend clears the persisted auto-connect flag so the
    // next launch starts on the local engine.
    if (prev == AsrBackendKind::Remote && kind != AsrBackendKind::Remote) {
        AppSettings::instance().setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"));
        AppSettings::instance().save();
    }

    // Only a change of backend kind needs a fresh engine; switching models within
    // the same backend (e.g. base → small, or a custom file) reloads via
    // beginEnable() without tearing the engine down.
    if (prev != kind) {
        m_tap->setEnabled(false);
        buildEngine();
    }
}

void CopyAssistController::beginEnable()
{
    m_panel->setBusy(true);
    if (m_backend == AsrBackendKind::Remote) {
        // No local model to fetch — the remote endpoint is contacted per
        // utterance. load() just marks the backend ready.
        m_panel->setStatus(tr("Connecting to remote server…"));
        m_asr->setModelPath(QString());
    } else if (m_tierId == QString::fromLatin1(kCustomTierId)) {
        // User-supplied model: load the picked file directly, bypassing the
        // catalog download + SHA verification (we don't know its checksum).
        if (m_customModelPath.isEmpty() || !QFileInfo::exists(m_customModelPath)) {
            m_panel->setBusy(false);
            m_panel->setStatus(tr("Custom model file not found — pick it again."));
            m_panel->setAsrEnabled(false);
            return;
        }
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(m_customModelPath);
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

QString CopyAssistController::promptCustomModel()
{
    // Default the picker to the last-picked file's folder, else the models cache
    // dir (where a manually-dropped ggml-*.bin would live).
    QString startDir = QFileInfo(m_customModelPath).absolutePath();
    if (startDir.isEmpty()) {
        startDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + QStringLiteral("/models");
    }
    return QFileDialog::getOpenFileName(
        m_panel, tr("Choose a Whisper model"), startDir,
        tr("Whisper models (*.bin *.gguf);;All files (*)"));
}

} // namespace AetherSDR
