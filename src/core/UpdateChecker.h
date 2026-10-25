#pragma once

#include <QDateTime>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace AetherSDR {

//DH1KLM What the Updates window shows about one release, read from the GitHub
//DH1KLM release JSON. Every string is network text: plain text only, trimmed and
//DH1KLM capped by parseRelease(), never markup.
struct ReleaseAsset {
    QString name;
    QUrl    url;
    qint64  size = 0;
};

struct ReleaseInfo {
    QString version;                    //DH1KLM parsed digits, "26.10.3"; empty if unusable
    QString tagline;                    //DH1KLM the release name after "AetherSDR vX — "
    QDateTime published;                //DH1KLM UTC; invalid when absent
    QDateTime answered;                 //DH1KLM UTC, when this answer arrived; invalid from settings
    QStringList chapters;               //DH1KLM the body's ### headings, boilerplate dropped
    QList<ReleaseAsset> assets;         //DH1KLM only download URLs that pass isTrustedDownloadUrl()
    //DH1KLM Only the version an earlier answer saved, not an answer from this run:
    //DH1KLM no details, and no claim about what GitHub says now.
    bool fromSettings = false;
    bool isValid() const { return !version.isEmpty(); }
};

//DH1KLM Checks the GitHub releases API for a newer AetherSDR version, and is the
//DH1KLM GUI's one entry point to the Updates settings (UpdateSettings stays inside
//DH1KLM the engine). kReleasesPageUrl is the browser-facing page; the API endpoint
//DH1KLM lives in the .cpp.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    static constexpr char kReleasesPageUrl[] =
        "https://github.com/aethersdr/AetherSDR/releases/latest";

    //DH1KLM Manual is Help → Check for Updates and reports every outcome to the
    //DH1KLM operator; Automatic is the background startup check, which only lights
    //DH1KLM the title-bar badge.
    enum class Trigger { Manual, Automatic };
    Q_ENUM(Trigger)

    //DH1KLM `network` lets a test supply an in-memory transport; null makes one.
    //DH1KLM A supplied manager is not adopted: the caller keeps it alive.
    explicit UpdateChecker(QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);

    //DH1KLM Starts a check; false when one is already in flight. A manual request
    //DH1KLM made during an automatic check upgrades that check, so the operator
    //DH1KLM still gets an answer.
    bool checkNow(Trigger trigger = Trigger::Manual);

    //DH1KLM Starts an automatic check when UpdateSettings allow one now and this
    //DH1KLM install is not updated by a store. Returns whether a check started.
    bool checkAutomaticallyIfDue();

    //DH1KLM "the Microsoft Store" or "Flatpak" when that installs this copy's
    //DH1KLM updates, worded to follow "come from"; empty for an installer,
    //DH1KLM portable, DMG or AppImage build. Probed once per process.
    static QString storeName();

    //DH1KLM The Help menu's automatic-check toggle. Off also retires the badge.
    static bool checkOnStartup();
    static void setCheckOnStartup(bool enabled);

    //DH1KLM Skip This Version: stores the parsed release, never the raw text.
    static void skipVersion(const QString& version);
    static bool isSkipped(const QString& version);

    //DH1KLM Whether parsed version `a` is newer than `b`; false if `a` is unusable.
    static bool isNewer(const QString& a, const QString& b);

    //DH1KLM The release the title-bar badge should announce: `latestVersion` when
    //DH1KLM it is newer than `runningVersion`, the automatic check is on, it is not
    //DH1KLM skipped, and this copy is not updated by a store; otherwise empty. Live
    //DH1KLM results and the one restored from settings at startup both go through it.
    static QString badgeVersion(const QString& latestVersion, const QString& runningVersion);

    //DH1KLM badgeVersion() for the release the last successful check recorded.
    static QString knownBadgeVersion();

    //DH1KLM The release the last successful check described. Until this run's
    //DH1KLM check lands it is the version a previous answer saved (no details), so
    //DH1KLM a badge restored at startup can be acted on without a request.
    //DH1KLM Held in memory only: settings keep just the parsed version.
    const ReleaseInfo& latestRelease() const { return m_latest; }
    bool isChecking() const { return m_inFlight; }

    //DH1KLM Reads the release JSON GitHub returns. Pure: no network, no settings.
    static ReleaseInfo parseRelease(const QByteArray& json);

    //DH1KLM Only https://github.com/aethersdr/AetherSDR/releases/download/... may
    //DH1KLM reach the browser from a release's asset list.
    static bool isTrustedDownloadUrl(const QUrl& url);

    //DH1KLM The installer for an OS ("windows", "macos", "linux"), a CPU
    //DH1KLM architecture as QSysInfo names it, and
    //DH1KLM whether a Windows copy is the portable zip. Index into `assets`, or -1.
    static int pickAsset(const QList<ReleaseAsset>& assets, const QString& os,
                         const QString& cpuArch, bool portable);

    //DH1KLM pickAsset() for this computer, or an invalid asset when none fits
    //DH1KLM (the window then offers the release page).
    static ReleaseAsset assetForThisComputer(const ReleaseInfo& release);

    //DH1KLM The release page for a parsed version, built from the digits alone.
    static QUrl releasePageUrl(const QString& version);

signals:
    //DH1KLM A request went out; isChecking() is now true.
    void checkStarted();
    //DH1KLM A check finished with a usable answer; latestRelease() now holds it.
    void releaseInfoChanged();
    void updateAvailable(const QString& latestVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void upToDate(const QString& currentVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void checkFailed(AetherSDR::UpdateChecker::Trigger trigger);

private:
    void fail(const QString& reason);

    QNetworkAccessManager* m_nam{nullptr};
    ReleaseInfo m_latest;
    bool m_inFlight = false;
    bool m_tooLarge = false;
    //DH1KLM Whether the request in flight began as the automatic check. A manual
    //DH1KLM request can upgrade m_trigger mid-flight; the daily wait still counts it.
    bool m_startedAutomatically = false;
    Trigger m_trigger = Trigger::Manual;
};

} // namespace AetherSDR
