#include "core/MercuryProcess.h"

#include <QFileInfo>

namespace AetherSDR {

MercuryProcess::MercuryProcess(QObject* parent) : QObject(parent)
{
    // Mercury writes progress to both streams; merging them keeps the order the
    // operator would see in a terminal.
    m_process.setProcessChannelMode(QProcess::MergedChannels);

    connect(&m_process, &QProcess::readyReadStandardOutput,
            this, &MercuryProcess::readReady);
    connect(&m_process, &QProcess::started, this, &MercuryProcess::started);
    connect(&m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart)
                    emit failed(tr("Could not run %1").arg(m_binaryPath));
            });
    connect(&m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) {
                const bool expected = m_stopping;
                m_stopping = false;
                emit finished(code, expected);
            });
}

MercuryProcess::~MercuryProcess()
{
    // A modem left running would hold the sound devices and the TCP ports, so
    // the next start fails for reasons that look nothing like the cause.
    stop();
}

QStringList MercuryProcess::buildArguments(const Options& options)
{
    QStringList args;

    // -p is the ARQ base port; Mercury derives the data port as base+1 itself,
    // exactly as VARA does, so only the one number is passed.
    args << QStringLiteral("-p") << QString::number(options.basePort);
    args << QStringLiteral("-m") << QString::number(options.modeIndex);

    // Each of these is omitted when unset rather than passed empty. Mercury has
    // sensible per-platform defaults, and an empty -x or -i is not "default" to
    // it — it is a device named "", which fails.
    if (!options.soundSystem.trimmed().isEmpty())
        args << QStringLiteral("-x") << options.soundSystem.trimmed();
    if (!options.captureDevice.trimmed().isEmpty())
        args << QStringLiteral("-i") << options.captureDevice.trimmed();
    if (!options.playbackDevice.trimmed().isEmpty())
        args << QStringLiteral("-o") << options.playbackDevice.trimmed();
    if (!options.configPath.trimmed().isEmpty())
        args << QStringLiteral("-C") << options.configPath.trimmed();

    return args;
}

bool MercuryProcess::start(const QString& binaryPath, const Options& options)
{
    if (isRunning())
        return true;

    const QString path = binaryPath.trimmed();
    if (path.isEmpty()) {
        emit failed(tr("No Mercury binary is configured."));
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        emit failed(tr("Mercury binary not found: %1").arg(path));
        return false;
    }
    if (!info.isExecutable()) {
        emit failed(tr("Mercury binary is not executable: %1").arg(path));
        return false;
    }

    m_binaryPath = path;
    m_pending.clear();
    m_stopping = false;
    m_process.start(path, buildArguments(options));
    return true;
}

void MercuryProcess::stop()
{
    if (!isRunning())
        return;
    m_stopping = true;
    m_process.terminate();
    // Mercury closes its sound devices on SIGTERM. Give it a moment to do that
    // before insisting, since killing it outright can leave a device claimed.
    if (!m_process.waitForFinished(4000))
        m_process.kill();
}

bool MercuryProcess::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void MercuryProcess::readReady()
{
    m_pending += m_process.readAllStandardOutput();
    // Split on either terminator: Mercury's own logging is newline-terminated,
    // but a Windows build's runtime is not guaranteed to be.
    for (;;) {
        int cut = m_pending.indexOf('\n');
        if (cut < 0)
            break;
        const QString line =
            QString::fromLocal8Bit(m_pending.left(cut)).trimmed();
        m_pending.remove(0, cut + 1);
        if (!line.isEmpty())
            emit output(line);
    }
    // A modem that logs without newlines must not grow this without bound.
    if (m_pending.size() > 64 * 1024)
        m_pending.clear();
}

} // namespace AetherSDR
