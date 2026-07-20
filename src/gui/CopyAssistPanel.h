#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
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
    QString currentTier() const;

    void setStatus(const QString& text);
    bool isAsrEnabled() const;
    void setAsrEnabled(bool on);

public slots:
    // Append one transcribed utterance, colored by confidence in [0, 1].
    void appendText(const QString& text, float confidence);
    void clearText();

signals:
    void enableToggled(bool on);
    void tierChanged(const QString& tierId);
    void clearRequested();

private:
    static QString colorForConfidence(float confidence);

    QTextEdit* m_text = nullptr;
    QCheckBox* m_enable = nullptr;
    QComboBox* m_tier = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_clear = nullptr;
};

} // namespace AetherSDR
