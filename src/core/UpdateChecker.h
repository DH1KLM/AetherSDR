#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace AetherSDR {

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

    //DH1KLM The release the title-bar badge should announce: `latestVersion` when
    //DH1KLM it is newer than `runningVersion`, the automatic check is on, it is not
    //DH1KLM skipped, and this copy is not updated by a store; otherwise empty. Live
    //DH1KLM results and the one restored from settings at startup both go through it.
    static QString badgeVersion(const QString& latestVersion, const QString& runningVersion);

    //DH1KLM badgeVersion() for the release the last successful check recorded.
    static QString knownBadgeVersion();

signals:
    void updateAvailable(const QString& latestVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void upToDate(const QString& currentVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void checkFailed(AetherSDR::UpdateChecker::Trigger trigger);

private:
    void fail(const QString& reason);

    QNetworkAccessManager* m_nam{nullptr};
    bool m_inFlight = false;
    bool m_tooLarge = false;
    //DH1KLM Whether the request in flight began as the automatic check. A manual
    //DH1KLM request can upgrade m_trigger mid-flight; the daily wait still counts it.
    bool m_startedAutomatically = false;
    Trigger m_trigger = Trigger::Manual;
};

} // namespace AetherSDR
