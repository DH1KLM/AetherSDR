#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Runs a MercuryV2 modem as a child process.
//
// WHY A PROCESS AND NOT A LIBRARY. Mercury (Rhizomatica, GPL-3.0) is built as a
// standalone binary with its own Makefile, its own audio backends for every
// platform, and its own radio control. It exposes a VARA-compatible TCP host
// interface precisely so applications drive it from outside — its own GUI,
// mercury-qt, does exactly this. Vendoring 1300 files and a second audio stack
// into this build would buy nothing and cost the Windows and macOS builds.
//
// So this only supervises: start it, watch it, stop it, and surface what it
// says. The session itself is spoken over TCP by the same client that drives
// VARA, because Mercury implements the same protocol on the same port layout.
//
// LAUNCHING IS OPTIONAL. An operator may already run Mercury themselves, or run
// it on another machine. Nothing here is required to use the engine — it is a
// convenience for the common case of one local binary.
class MercuryProcess : public QObject {
    Q_OBJECT

public:
    explicit MercuryProcess(QObject* parent = nullptr);
    ~MercuryProcess() override;

    // The command line for a given configuration.
    //
    // Static and pure so it can be tested without launching anything: getting
    // these arguments wrong is silent — Mercury starts, opens the wrong sound
    // device, and simply never hears the radio.
    struct Options {
        int basePort = 8400;
        QString soundSystem;        // -x; omitted when empty (Mercury's default)
        QString captureDevice;      // -i
        QString playbackDevice;     // -o
        int modeIndex = 1;          // -m
        QString configPath;         // -C; Mercury's own config file
    };
    static QStringList buildArguments(const Options& options);

    // Start the binary. Returns false and emits failed() if it cannot be run.
    bool start(const QString& binaryPath, const Options& options);
    void stop();

    bool isRunning() const;
    QString binaryPath() const { return m_binaryPath; }

signals:
    // A line Mercury wrote to stdout or stderr, already trimmed. Forwarded so
    // the page's event log shows why a modem refused to start instead of
    // leaving the operator with a process that vanished.
    void output(const QString& line);
    void started();
    // The process ended. `expected` is true when stop() asked it to.
    void finished(int exitCode, bool expected);
    void failed(const QString& reason);

private:
    void readReady();

    QProcess m_process;
    QString m_binaryPath;
    QByteArray m_pending;
    bool m_stopping{false};
};

} // namespace AetherSDR
