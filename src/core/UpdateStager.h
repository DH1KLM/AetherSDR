#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

namespace AetherSDR {

class CappedDownload;

//DH1KLM The signed in-app update manifest (RFC #5304), schema 1, as
//DH1KLM docs/architecture/update-manifest.md defines it. parse() is strict: a
//DH1KLM manifest that breaks any rule there is refused whole.
struct UpdateManifest {
    struct Asset {
        QString name;
        QUrl url;
        QString sha256;   //DH1KLM 64 lowercase hex digits
        qint64 size{0};
    };
    struct File {
        QString path;
        bool symlink{false};
        QString target;   //DH1KLM symlink only
        qint64 size{0};
        QString sha256;
        bool executable{false};
    };
    struct Op {
        enum class Kind { Remove, Symlink, Patch, Add };
        Kind kind{Kind::Remove};
        QString path;
        QString target;       //DH1KLM Symlink
        QString member;       //DH1KLM Patch, Add: the bundle member
        int windowLog{0};     //DH1KLM Patch, Add
        QString sha256;       //DH1KLM Patch, Add: the result
        qint64 size{0};       //DH1KLM Patch, Add: the result
        bool executable{false};
        QString fromSha256;   //DH1KLM Patch: the file it applies to
    };
    struct Patch {
        QString from;   //DH1KLM the version it applies to, "26.10.2"
        Asset bundle;
        QList<Op> ops;
    };
    struct Payload {
        QString platform;   //DH1KLM "windows", "linux"
        QString arch;       //DH1KLM QSysInfo::buildCpuArchitecture()'s name
        Asset full;
        QList<File> files;
        QList<Patch> patches;
    };

    int schema{0};
    QString tag;       //DH1KLM "v26.10.3"
    QString version;   //DH1KLM "26.10.3"
    QDateTime generatedAt;
    QString keyId;
    QList<Payload> payloads;

    static bool parse(const QByteArray& json, UpdateManifest* out, QString* problem);
};

//DH1KLM Takes AetherSDR from the running release to a newer one by applying a
//DH1KLM signed manifest's per-file patches into a staging copy of the install.
//DH1KLM Engine only (RFC #5304): it never starts on its own, never touches the
//DH1KLM running install, and swapping the staged tree in is the platform phases'
//DH1KLM job. With Request::fullFallback it stages the full release when no patch
//DH1KLM applies, and every staged release keeps a record beside it
//DH1KLM (stagedRelease(), revalidate()).
//DH1KLM
//DH1KLM stage() fetches the manifest and its signature, verifies the signature
//DH1KLM against the embedded update key before reading a byte of the JSON, refuses
//DH1KLM a manifest that is not newer than this build, is older than one already
//DH1KLM accepted (a replay), or names another release than GitHub's latest (a
//DH1KLM frozen latest/), then downloads this build's bundle, checks its size and
//DH1KLM SHA-256, copies the install into stagingRoot and applies the ops there.
//DH1KLM Every file of the new release is checked before staged() is emitted.
class UpdateStager : public QObject {
    Q_OBJECT

public:
    struct Request {
        QString runningVersion;    //DH1KLM "26.10.2"
        QString platform;          //DH1KLM "windows", "linux"
        QString arch;              //DH1KLM QSysInfo::buildCpuArchitecture()
        QString githubLatestTag;   //DH1KLM releases/latest's tag_name, from UpdateChecker
        QString installRoot;       //DH1KLM the tree the running build was installed as
        QString stagingRoot;       //DH1KLM per-user app data, outside the install
        QUrl manifestUrl;          //DH1KLM kManifestUrl unless a test says otherwise
        //DH1KLM When no patch applies (or one fails), stage the full release
        //DH1KLM instead: the signed portable zip's files plus the installer's own
        //DH1KLM (unins*). Windows: its full download is that tree. Otherwise
        //DH1KLM fullDownloadNeeded() is emitted, as before.
        bool fullFallback = false;
    };

    //DH1KLM An update staged by an earlier run (or this one), waiting for the swap.
    struct StagedRelease {
        QString version;   //DH1KLM empty: nothing usable is staged
        QString dir;       //DH1KLM <stagingRoot>/<version>
        QString from;      //DH1KLM the version it was staged for
    };

    //DH1KLM What choose() decided for a verified manifest.
    enum class Decision { Patch, FullDownload, NotNewer, Replay, NotLatest, NoPayload };

    static const char* const kManifestUrl;
    //DH1KLM Hard ceilings, whatever a manifest claims.
    static constexpr qint64 kMaxManifestBytes = 4 * 1024 * 1024;
    static constexpr qint64 kMaxBundleBytes = 256LL * 1024 * 1024;
    static constexpr qint64 kMaxFullBytes = 512LL * 1024 * 1024;
    static constexpr int kMaxWindowLog = 31;
    static constexpr int kMaxBundleMembers = 4096;

    explicit UpdateStager(QObject* parent = nullptr);
    ~UpdateStager() override;

    void stage(const Request& request);
    void cancel();
    bool isBusy() const;

    //DH1KLM The update signing key built in (Ed25519, 32 raw bytes), from
    //DH1KLM packaging/update-signing.pub.
    static QByteArray builtInKey();
    //DH1KLM builtInKey(), unless a test replaced it.
    static QList<QByteArray> trustedKeys();
    static void setTrustedKeysForTesting(const QList<QByteArray>& keys);
    //DH1KLM Tests: fetch "<base>/<tag>/<asset>" instead of the bundle's GitHub
    //DH1KLM URL (a file:// base keeps them off the network). The manifest's own
    //DH1KLM URLs are still checked as signed.
    static void setDownloadBaseForTesting(const QUrl& base);

    //DH1KLM A raw 64-byte Ed25519 signature of manifest by a trusted key.
    static bool verify(const QByteArray& manifest, const QByteArray& signature);

    //DH1KLM acceptedUtc: the newest generated_at already accepted (invalid: none).
    static Decision choose(const UpdateManifest& manifest, const Request& request,
                           const QDateTime& acceptedUtc, const UpdateManifest::Payload** payload,
                           const UpdateManifest::Patch** patch);

    //DH1KLM The members of a bundle: a ustar of flat NNNNNN.zst files. The members
    //DH1KLM are views into bundle, which must outlive them.
    static bool readBundle(const QByteArray& bundle, QHash<QString, QByteArray>* members,
                           QString* problem);

    //DH1KLM The full release from its portable zip into stagingDir: exactly the
    //DH1KLM files manifest lists, plus the installer's own files (unins*) copied
    //DH1KLM from installRoot, then every file checked. On any failure, or once
    //DH1KLM cancelled is set, stagingDir is removed.
    static bool applyFull(const QString& installRoot, const QByteArray& zip,
                          const QList<UpdateManifest::File>& files, const QString& stagingDir,
                          QString* problem, const std::atomic_bool* cancelled = nullptr);

    //DH1KLM The release staged in stagingRoot, if it is still the one to install:
    //DH1KLM staged for runningVersion, from installRoot, it is GitHub's latest
    //DH1KLM (githubLatestTag) and not the release that was rolled back. Otherwise
    //DH1KLM an empty StagedRelease. Whatever else staging wrote there is removed
    //DH1KLM (other versions' trees and records, half-written records, the work
    //DH1KLM folders a crashed run left); nothing else in the folder is touched,
    //DH1KLM since it is the swap helper's state folder too. Never call it while a
    //DH1KLM stager isBusy(), whose work folder it would remove.
    //DH1KLM An empty githubLatestTag (not known yet) discards nothing on that
    //DH1KLM ground. Cheap: reads the record only. revalidate() checks every file.
    static StagedRelease stagedRelease(const QString& stagingRoot, const QString& runningVersion,
                                       const QString& installRoot, const QString& githubLatestTag,
                                       const QString& rolledBackVersion);
    //DH1KLM Before the swap: the staged tree's signed manifest still verifies and
    //DH1KLM every file still matches it.
    static bool revalidate(const QString& stagingRoot, const StagedRelease& staged,
                           const QString& platform, const QString& arch, QString* problem);

    //DH1KLM Copies installRoot into stagingDir, applies patch's ops there in order
    //DH1KLM and checks every entry of files. On any failure, or once cancelled is
    //DH1KLM set (checked between steps), stagingDir is removed.
    static bool applyPatch(const QString& installRoot, const QByteArray& bundle,
                           const UpdateManifest::Patch& patch,
                           const QList<UpdateManifest::File>& files, const QString& stagingDir,
                           QString* problem, const std::atomic_bool* cancelled = nullptr);

signals:
    void progress(const QString& step, qint64 done, qint64 total);
    //DH1KLM The new release, complete and checked, in stagedDir.
    void staged(const QString& stagedDir, const QString& version);
    //DH1KLM No patch could be applied; the caller may offer the full download.
    void fullDownloadNeeded(const QString& reason, const AetherSDR::UpdateManifest::Asset& full,
                            const QString& version);
    //DH1KLM The running build is already the newest release.
    void upToDate();
    //DH1KLM The manifest could not be trusted or fetched.
    void failed(const QString& reason);

private:
    enum class Stage { Idle, Manifest, Signature, Bundle, Full };

    //DH1KLM Applies (on a worker thread) and, if this request is still current,
    //DH1KLM puts the result in <stagingRoot>/<version> beside its record.
    void runWorker(std::function<QString(const QString& work)> apply);
    //DH1KLM No patch applies or it failed: the full release when the request
    //DH1KLM allows it, otherwise fullDownloadNeeded().
    void fallBack(const QString& reason);

    void onDownloaded(const QUrl& url, const QByteArray& body);
    void fail(const QString& reason);

    CappedDownload* m_download;
    Stage m_stage{Stage::Idle};
    //DH1KLM Every stage() and cancel() starts a new generation: a worker's result
    //DH1KLM is used only if its generation is still the current one.
    quint64 m_generation{0};
    std::shared_ptr<std::atomic_bool> m_cancelled;
    Request m_request;
    QByteArray m_manifestBytes;
    QByteArray m_signature;
    UpdateManifest m_manifest;
    const UpdateManifest::Payload* m_payload{nullptr};
    const UpdateManifest::Patch* m_patch{nullptr};
};

} //DH1KLM namespace AetherSDR
