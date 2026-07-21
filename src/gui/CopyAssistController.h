#pragma once

#include <QObject>
#include <QString>

namespace AetherSDR {

class AudioEngine;
class CopyAssistPanel;
class AsrEngine;
class AsrModelManager;
class AsrAudioTap;

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
    bool promptRemoteConfig(); // edit + persist the remote endpoint; true if accepted

    AudioEngine* m_audio = nullptr;
    CopyAssistPanel* m_panel = nullptr;
    AsrEngine* m_asr = nullptr;
    AsrModelManager* m_models = nullptr;
    AsrAudioTap* m_tap = nullptr;
    QString m_tierId;
    bool m_enabled = false;
    bool m_remote = false; // using the RemoteAsrBackend rather than local whisper
};

} // namespace AetherSDR
