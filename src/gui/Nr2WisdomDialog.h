#pragma once

#include "CanonWindow.h"

#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace AetherSDR {

//DH1KLM The FFTW Wisdom window NR2 shows while the first-use FFT plans are made: a
//DH1KLM canon dialog with the current step, a progress bar, a "Working..." line with
//DH1KLM the elapsed time, a reassurance line when the planner has gone quiet, and
//DH1KLM Cancel. MainWindow runs the planner on a worker thread and feeds it here.
//DH1KLM
//DH1KLM Modeless and a tool window, so it floats above the main window without a
//DH1KLM taskbar entry or stealing focus while the operator keeps working; on macOS
//DH1KLM it stays visible when the app deactivates. Cancel, the corner ✕, Escape and
//DH1KLM Ctrl+W (⌘W) all request cancellation, once. Cancel leaves the window up,
//DH1KLM showing "Canceling..." until the plan in flight finishes; the other three
//DH1KLM also hide it, as QDialog::reject() does. The owner deletes it after
//DH1KLM showResult().
class Nr2WisdomDialog : public CanonWindow {
    Q_OBJECT

public:
    enum class Outcome { Complete, Canceled, Failed };

    explicit Nr2WisdomDialog(QWidget* parent = nullptr);

    //DH1KLM From the planner, on the GUI thread. Each one counts as a sign of life
    //DH1KLM and hides the reassurance line. Ignored once cancellation is requested.
    void setStepDescription(const QString& description);
    void setProgress(int percent);

    //DH1KLM The planner has finished: stop the activity line and show the outcome.
    void showResult(Outcome outcome);

    bool isCancelRequested() const { return m_cancelRequested; }

signals:
    //DH1KLM Emitted once, the first time the operator cancels.
    void cancelRequested();

private:
    void requestCancel();
    void tickActivity();
    void noteActivity();
    static void announce(QLabel* label);

    QLabel*       m_status{nullptr};
    QProgressBar* m_progress{nullptr};
    QLabel*       m_activity{nullptr};
    QLabel*       m_elapsed{nullptr};
    QLabel*       m_reassurance{nullptr};
    QPushButton*  m_cancel{nullptr};
    QTimer*       m_activityTimer{nullptr};
    QElapsedTimer m_elapsedTimer;
    QElapsedTimer m_lastActivity;
    bool          m_cancelRequested{false};
};

} //DH1KLM namespace AetherSDR
