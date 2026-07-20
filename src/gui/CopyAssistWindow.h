#pragma once

#include <QWidget>

namespace AetherSDR {

class AudioEngine;
class CopyAssistPanel;
class CopyAssistController;

// Floating host window for the Copy Assist panel (RFC #4333, Phase 5). Owns the
// panel and its controller; constructed with the app's AudioEngine so the
// controller can tap RX audio. Non-modal tool window.
class CopyAssistWindow : public QWidget {
    Q_OBJECT
public:
    explicit CopyAssistWindow(AudioEngine* audio, QWidget* parent = nullptr);

private:
    CopyAssistPanel* m_panel = nullptr;
    CopyAssistController* m_controller = nullptr;
};

} // namespace AetherSDR
