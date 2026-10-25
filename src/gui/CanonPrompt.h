#pragma once

#include "gui/CanonWindow.h"

class QLabel;
class QPushButton;

namespace AetherSDR {

//DH1KLM A canon confirmation: the CanonWindow ground with a warning eyebrow, the
//DH1KLM question, and Cancel beside a confirm button named for the action. Cancel
//DH1KLM is the default, so Return, Escape and the corner button all decline; exec()
//DH1KLM returns Accepted only for the confirm button. The text can change while it
//DH1KLM is open (an upload advancing under a close prompt).
class CanonPrompt : public CanonWindow {
    Q_OBJECT

public:
    CanonPrompt(const QString& title, const QString& text, const QString& confirmLabel,
                QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const;

    QPushButton* confirmButton() const { return m_confirm; }
    QPushButton* cancelButton() const { return m_cancel; }

private:
    QLabel*      m_text{nullptr};
    QPushButton* m_confirm{nullptr};
    QPushButton* m_cancel{nullptr};
};

} //DH1KLM namespace AetherSDR
