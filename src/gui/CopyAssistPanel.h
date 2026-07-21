#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTextEdit;

namespace AetherSDR {

// Copy Assist — the speech-to-text decode panel (RFC #4333, Phase 5), modeled
// on the CW (ggmorse) decode panel in PanadapterApplet: a scrolling, read-only
// transcript whose text is color-coded by whisper's per-utterance confidence
// (green = high … red = low, the inverse of the CW decoder's cost coloring),
// plus enable / model-tier / clear controls and a status line.
//
// Pure view: it emits intent (enableToggled / tierChanged / clearRequested) and
// renders whatever appendText()/setStatus() it is given. The controller owns the
// ASR engine and wiring, so this widget links no ASR/whisper code and could be
// driven by streamed results in the thin-UI/aetherd future.
class CopyAssistPanel : public QWidget {
    Q_OBJECT
public:
    explicit CopyAssistPanel(QWidget* parent = nullptr);

    // Add a selectable model tier (stable id + human label).
    void addTier(const QString& id, const QString& label);
    void setCurrentTier(const QString& id);
    void setTierLabel(const QString& id, const QString& label);
    QString currentTier() const;

    // GPU selector (shown only when there's more than one GPU to choose from).
    void addGpuDevice(int index, const QString& name);
    void setCurrentGpu(int index);
    int currentGpu() const;
    void setGpuSelectorVisible(bool on);

    void setStatus(const QString& text);
    // Show/hide the indeterminate loading indicator (model download/verify/load).
    void setBusy(bool on);
    bool isAsrEnabled() const;
    void setAsrEnabled(bool on);
    // The Enable/Disable toggle button — exposed so the app layer can apply the
    // themed applet-toggle style (the panel itself stays ThemeManager-free).
    QPushButton* enableButton() const { return m_enable; }

    // Decode-buffer size in milliseconds (1000–20000). The slider works in
    // whole seconds; setBufferMs rounds/clamps into range.
    void setBufferMs(int ms);
    int bufferMs() const;

    // VAD sensitivity as a percentage 1–100 (higher = more sensitive).
    void setSensitivity(int percent);
    int sensitivity() const;

    // Silence duration (hangover) that ends an utterance, in ms (100–2000).
    void setSilenceMs(int ms);
    int silenceMs() const;

    // Transcript font size in px (8–32).
    void setFontPx(int px);
    int fontPx() const { return m_fontPx; }

public slots:
    // Append one transcribed utterance, colored by confidence in [0, 1].
    void appendText(const QString& text, float confidence);
    void clearText();

signals:
    void enableToggled(bool on);
    void tierChanged(const QString& tierId);
    void gpuChanged(int index);
    void clearRequested();
    void closeRequested();
    void bufferMsChanged(int ms);
    void sensitivityChanged(int percent);
    void silenceMsChanged(int ms);
    void fontPxChanged(int px);

private:
    static QString colorForConfidence(float confidence);
    void applyFont();
    void adjustFont(int deltaPx);
    // Append "<label> [compact slider] <value>" inline to the control bar,
    // mirroring the CW decode bar's fixed-width sliders.
    QSlider* addSliderInline(QHBoxLayout* bar, const QString& label,
                             const QString& accessibleName, int lo, int hi, int value,
                             QLabel** valueLabelOut);

    QTextEdit* m_text = nullptr;
    QPushButton* m_enable = nullptr; // checkable: "Enable" / "Disable"
    QComboBox* m_tier = nullptr;
    QComboBox* m_gpu = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_clear = nullptr;
    QSlider* m_buffer = nullptr;
    QLabel* m_bufferValue = nullptr;
    QSlider* m_sensitivity = nullptr;
    QLabel* m_sensitivityValue = nullptr;
    QSlider* m_silence = nullptr;
    QLabel* m_silenceValue = nullptr;
    QProgressBar* m_busy = nullptr;
    int m_fontPx = 13;
};

} // namespace AetherSDR
