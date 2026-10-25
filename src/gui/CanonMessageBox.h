#pragma once

#include "CanonWindow.h"

#include <QList>

class QLabel;
class QHBoxLayout;
class QPushButton;

namespace AetherSDR {

//DH1KLM A question or a notice in the style guide's window, for windows built on
//DH1KLM CanonWindow in place of QMessageBox: the title as a canon header, the text in
//DH1KLM canon ink, optional further text below it in the softer ink, and the buttons
//DH1KLM in a row at the bottom right. Run it with exec() and read clickedButton(),
//DH1KLM which stays null when the operator closes it instead (the corner button,
//DH1KLM Escape, ⌘W / Ctrl+W).
class CanonMessageBox : public CanonWindow {
    Q_OBJECT

public:
    //DH1KLM Accept and Reject buttons close it as accepted or rejected; Destructive
    //DH1KLM accepts too, and is drawn in the warning gold, for a step that replaces
    //DH1KLM what is on the device.
    enum class Role { Accept, Reject, Destructive };

    CanonMessageBox(const QString& title, const QString& text, QWidget* parent = nullptr);

    void setInformativeText(const QString& text);
    //DH1KLM Lets the operator select and copy the texts (commands to paste).
    void setTextSelectable(bool on);

    QPushButton* addButton(const QString& text, Role role);
    //DH1KLM The button Return clicks; also takes the focus when the box opens.
    //DH1KLM Every box sets one; without it nothing decides where the focus starts.
    void setDefaultButton(QPushButton* button);
    QPushButton* clickedButton() const { return m_clicked; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    QLabel*             m_text{nullptr};
    QLabel*             m_info{nullptr};
    QHBoxLayout*        m_buttons{nullptr};
    QList<QPushButton*> m_added;
    QPushButton*        m_default{nullptr};
    QPushButton*        m_clicked{nullptr};
};

} //DH1KLM namespace AetherSDR
