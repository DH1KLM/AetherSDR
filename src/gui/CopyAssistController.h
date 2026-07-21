#pragma once

#include <QObject>
#include <QString>

namespace AetherSDR {

class AudioEngine;
class CopyAssistPanel;
class CopyAssistSettingsDialog;
class AsrEngine;
class AsrModelManager;
class AsrAudioTap;

// Which IAsrBackend the engine is currently built around. Selects the factory in
// buildEngine(); a local model's source (downloaded tier vs. user-supplied
// "custom" file) is an orthogonal axis keyed off the tier id. Extending to a new
// local engine family is a drop-in: add an enumerator, a buildEngine() case, and
// a mapping in backendForTier(). See AsrModelFamily.
enum class AsrBackendKind {
    Whisper, // local whisper.cpp (a catalog tier or a user "custom" file)
    Remote,  // RemoteAsrBackend over HTTP
};

// Wires the Copy Assist panel to the ASR subsystem (RFC #4333, Phase 5). Owns
// the AsrEngine, the model download manager, and the audio tap, and translates
// panel intent into the enable → (download model) → load → tap-on flow, routing
// transcripts and status back to the panel. Lives in the app layer so the panel
// stays a pure view and aethercore/aetherasr stay decoupled.
class CopyAssistController : public QObject {
    Q_OBJECT
public:
    CopyAssistController(AudioEngine* audio, CopyAssistPanel* panel, QObject* parent = nullptr);
    ~CopyAssistController() override;

    // Clear the transcript and drop any in-progress utterance — used on retune so
    // the decode window starts fresh for the new frequency.
    void clearDecode();

private slots:
    void onEnableToggled(bool on);
    void onTierChanged(const QString& tierId);

private:
    void buildEngine();  // (re)create the engine+tap for the current backend
    void applyTuning();  // push saved VAD tuning into the engine
    void beginEnable();
    void requestModel(const QString& tierId);
    bool promptRemoteConfig();  // edit + persist the remote endpoint; true if accepted
    QString promptCustomModel(); // pick a local ggml/gguf model file (empty if cancelled)
    void promptLogFile();        // pick + persist the transcript log path
    void appendToLogFile(const QString& text); // write one utterance if logging is on
    // Which backend a selected tier id maps to (catalog family → backend kind;
    // the "custom" file and any unknown id default to local Whisper).
    static AsrBackendKind backendForTier(const QString& tierId);
    // Switch the active backend + tier, clearing the remote flag when leaving
    // remote and rebuilding the engine only when the backend kind actually changes.
    void setBackend(AsrBackendKind kind, const QString& tierId);

    AudioEngine* m_audio = nullptr;
    CopyAssistPanel* m_panel = nullptr;
    CopyAssistSettingsDialog* m_settings = nullptr; // modeless model/GPU/options dialog
    AsrEngine* m_asr = nullptr;
    AsrModelManager* m_models = nullptr;
    AsrAudioTap* m_tap = nullptr;
    QString m_tierId;
    QString m_customModelPath; // user-picked local model (for the "Custom model…" tier)
    bool m_enabled = false;
    AsrBackendKind m_backend = AsrBackendKind::Whisper; // active inference backend
};

} // namespace AetherSDR
