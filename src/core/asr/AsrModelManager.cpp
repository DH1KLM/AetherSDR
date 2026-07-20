#include "core/asr/AsrModelManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrModel, "aether.asr.model")

namespace {
constexpr qint64 kHashChunkBytes = 1 << 20; // 1 MiB
} // namespace

AsrModelManager::AsrModelManager(QObject* parent)
    : AsrModelManager(
          QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
              + QStringLiteral("/models"),
          nullptr, parent)
{
}

AsrModelManager::AsrModelManager(QString cacheDir, QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent)
    , m_cacheDir(std::move(cacheDir))
    , m_nam(nam)
{
    if (m_nam == nullptr) {
        m_nam = new QNetworkAccessManager(this);
        m_ownsNam = true;
    }
}

AsrModelManager::~AsrModelManager() = default;

QString AsrModelManager::modelPath(const AsrModelTier& tier) const
{
    return QDir(m_cacheDir).filePath(tier.fileName);
}

QString AsrModelManager::partPath(const AsrModelTier& tier) const
{
    return modelPath(tier) + QStringLiteral(".part");
}

bool AsrModelManager::isPresent(const AsrModelTier& tier) const
{
    const QFileInfo info(modelPath(tier));
    return info.exists() && info.size() == tier.sizeBytes;
}

bool AsrModelManager::verifyFile(const QString& path, const AsrModelTier& tier,
                                 QString* error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        }
        return false;
    }

    if (file.size() != tier.sizeBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("size mismatch: got %1, expected %2")
                         .arg(file.size())
                         .arg(tier.sizeBytes);
        }
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kHashChunkBytes);
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
    }

    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual.compare(tier.sha256, Qt::CaseInsensitive) != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("SHA-256 mismatch: got %1, expected %2")
                         .arg(actual, tier.sha256);
        }
        return false;
    }
    return true;
}

bool AsrModelManager::verify(const AsrModelTier& tier, QString* error) const
{
    return verifyFile(modelPath(tier), tier, error);
}

void AsrModelManager::ensure(const AsrModelTier& tier)
{
    if (isBusy()) {
        emit failed(QStringLiteral("A model download is already in progress."));
        return;
    }

    // Already cached and valid — nothing to do.
    if (isPresent(tier) && verify(tier, nullptr)) {
        emit alreadyPresent(modelPath(tier));
        return;
    }

    if (tier.sources.isEmpty()) {
        emit failed(QStringLiteral("No download sources for tier '%1'.").arg(tier.id));
        return;
    }

    if (!QDir().mkpath(m_cacheDir)) {
        emit failed(QStringLiteral("Cannot create model cache directory: %1").arg(m_cacheDir));
        return;
    }

    m_tier = tier;
    m_sourceErrors.clear();
    m_canceled = false;
    startSource(0);
}

void AsrModelManager::startSource(int index)
{
    if (index >= m_tier.sources.size()) {
        const QString detail = m_sourceErrors.isEmpty()
                                   ? QStringLiteral("all sources failed")
                                   : m_sourceErrors.join(QStringLiteral("; "));
        emit failed(QStringLiteral("Could not download %1: %2").arg(m_tier.fileName, detail));
        return;
    }

    m_sourceIndex = index;
    const QUrl url(m_tier.sources.at(index));

    // Each attempt starts a fresh .part (truncate). Byte-range resume across
    // interrupted downloads is a documented follow-up; today an interrupted or
    // failed source restarts from zero on the next source.
    m_partFile = std::make_unique<QFile>(partPath(m_tier));
    if (!m_partFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_sourceErrors << QStringLiteral("%1: cannot open temp file: %2")
                              .arg(url.host(), m_partFile->errorString());
        m_partFile.reset();
        startSource(index + 1);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR-ASR"));

    qCInfo(lcAsrModel) << "Fetching" << m_tier.fileName << "from" << url.toString();
    m_reply = m_nam->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &AsrModelManager::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &AsrModelManager::onReplyFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) { emit progress(received, total); });
}

void AsrModelManager::onReadyRead()
{
    if (m_reply == nullptr || m_partFile == nullptr) {
        return;
    }
    const QByteArray data = m_reply->readAll();
    if (!data.isEmpty()) {
        m_partFile->write(data);
    }
}

void AsrModelManager::onReplyFinished()
{
    if (m_reply == nullptr) {
        return;
    }

    const QNetworkReply::NetworkError err = m_reply->error();
    const QString host = m_reply->url().host();

    if (m_canceled) {
        cleanupReply();
        abandonPartFile();
        emit failed(QStringLiteral("Download canceled."));
        return;
    }

    if (err != QNetworkReply::NoError) {
        m_sourceErrors << QStringLiteral("%1: %2").arg(host, m_reply->errorString());
        cleanupReply();
        abandonPartFile();
        startSource(m_sourceIndex + 1);
        return;
    }

    // Flush any trailing bytes and close the file before hashing.
    onReadyRead();
    const QString partFilePath = m_partFile->fileName();
    m_partFile->close();
    m_partFile.reset();
    cleanupReply();

    QString verifyError;
    if (!verifyFile(partFilePath, m_tier, &verifyError)) {
        qCWarning(lcAsrModel) << "Rejected download from" << host << ":" << verifyError;
        m_sourceErrors << QStringLiteral("%1: %2").arg(host, verifyError);
        QFile::remove(partFilePath);
        startSource(m_sourceIndex + 1);
        return;
    }

    // Atomically move the verified file into place.
    const QString finalPath = modelPath(m_tier);
    QFile::remove(finalPath); // rename() won't overwrite on some platforms
    if (!QFile::rename(partFilePath, finalPath)) {
        QFile::remove(partFilePath);
        emit failed(QStringLiteral("Verified %1 but could not move it into the cache.")
                        .arg(m_tier.fileName));
        return;
    }

    qCInfo(lcAsrModel) << "Cached verified model" << finalPath;
    emit finished(finalPath);
}

void AsrModelManager::cancel()
{
    if (m_reply == nullptr) {
        return;
    }
    m_canceled = true;
    m_reply->abort(); // triggers finished() with OperationCanceledError
}

void AsrModelManager::cleanupReply()
{
    if (m_reply != nullptr) {
        m_reply->disconnect(this);
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void AsrModelManager::abandonPartFile()
{
    if (m_partFile != nullptr) {
        const QString path = m_partFile->fileName();
        m_partFile->close();
        m_partFile.reset();
        QFile::remove(path);
    }
}

} // namespace AetherSDR
