#pragma once

#include "gui/CanonWindow.h"

#include <QPointer>
#include <QString>
#include <QStringList>

class QCheckBox;
class QFrame;
class QScrollArea;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

class FirmwareUpdateCard;
class FlowLayout;
class RadioModel;
class UpdateChecker;

//DH1KLM File → Update...: what AetherSDR release is newest, what is in it, and the
//DH1KLM download built for this computer (RFC #5304); below it, the connected
//DH1KLM radio's firmware (FirmwareUpdateCard). Made to make an update worth
//DH1KLM installing, after aethersdr.com/experience. Everything it shows comes from
//DH1KLM UpdateChecker: the parsed version, and the release's tagline and chapters
//DH1KLM as plain text. Opening it without the release's details starts a check.
class UpdatesWindow : public CanonWindow {
    Q_OBJECT

public:
    //DH1KLM `radio` may be null (no radio section).
    UpdatesWindow(UpdateChecker* checker, RadioModel* radio, QWidget* parent = nullptr);

    //DH1KLM Scroll the radio firmware section into view (the status bar's
    //DH1KLM out-of-date firmware version opens the window here).
    void showFirmware();

    //DH1KLM Ask GitHub again now (Check Now, and Help → Check for Updates...).
    void checkNow();

    //DH1KLM Mirrors Help → Check for Updates Automatically.
    void setAutomaticCheck(bool on);

signals:
    //DH1KLM The badge the title bar should show now (empty clears it).
    void badgeChanged(const QString& version);
    //DH1KLM The window's Check daily box changed; MainWindow keeps the Help menu
    //DH1KLM toggle and the settings in step.
    void automaticCheckToggled(bool on);

protected:
    //DH1KLM Not while firmware is being written: every close path asks first.
    void done(int result) override;

private:
    friend class UpdatesWindowTestAccess;

    enum class View { Checking, Available, Current, Failed };

    void refresh();
    View currentView() const;
    void rebuildRelease();
    void openDownload();

    QPointer<UpdateChecker> m_checker;
    bool m_failed{false};
    bool m_firmwareBusy{false};
    FirmwareUpdateCard* m_firmware{nullptr};
    QScrollArea* m_scroll{nullptr};

    QWidget*     m_header{nullptr};
    QLabel*      m_eyebrow{nullptr};
    QWidget*     m_version{nullptr};
    QLabel*      m_tagline{nullptr};
    QLabel*      m_note{nullptr};
    QLabel*      m_chaptersTitle{nullptr};
    QWidget*     m_chips{nullptr};
    FlowLayout*  m_chipsLayout{nullptr};
    QPushButton* m_get{nullptr};
    QPushButton* m_notes{nullptr};
    QPushButton* m_skip{nullptr};
    QPushButton* m_checkNow{nullptr};
    QLabel*      m_footnote{nullptr};
    QCheckBox*   m_autoCheck{nullptr};
    QStringList  m_builtChapters;   //DH1KLM the chapters the chips were built from
};

} //DH1KLM namespace AetherSDR
