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

private slots:
    void onEnableToggled(bool on);
    void onTierChanged(const QString& tierId);

private:
    void beginEnable();
    void requestModel(const QString& tierId);

    CopyAssistPanel* m_panel = nullptr;
    AsrEngine* m_asr = nullptr;
    AsrModelManager* m_models = nullptr;
    AsrAudioTap* m_tap = nullptr;
    QString m_tierId;
    bool m_enabled = false;
};

} // namespace AetherSDR
