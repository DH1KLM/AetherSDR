#pragma once

//DH1KLM In-app update preferences (RFC #5304). One nested JSON object under the
//DH1KLM single AppSettings key "Updates" (the AetherClockSettings pattern), never
//DH1KLM flat keys.

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class UpdateSettings {
public:
    //DH1KLM An automatic check runs at most once per this many hours, which keeps
    //DH1KLM launches well under the unauthenticated GitHub API's 60 requests/hour.
    static constexpr qint64 kAutomaticCheckIntervalHours = 24;

    //DH1KLM Check for a newer release in the background at startup. Default on.
    static bool checkOnStartup();
    static void setCheckOnStartup(bool enabled);

    //DH1KLM The release the operator chose to skip; empty when none. A skipped
    //DH1KLM release never lights the title-bar badge, but an explicit
    //DH1KLM Help → Check for Updates still reports it.
    static QString skippedVersion();
    static void setSkippedVersion(const QString& version);
    static bool isSkipped(const QString& version);

    //DH1KLM When the last check (either trigger) got an answer from the server.
    //DH1KLM Invalid when there has never been one.
    static QDateTime lastCheckUtc();
    static void setLastCheckUtc(const QDateTime& when);

    //DH1KLM The newest release that answer named, as parsed digits; empty before
    //DH1KLM the first answer. Kept so a restart inside the throttle still knows
    //DH1KLM an update exists without asking the server again.
    static QString latestVersion();

    //DH1KLM One write for a successful answer: its time and the release it named.
    static void recordAnswer(const QDateTime& when, const QString& latestVersion);

    //DH1KLM Whether an automatic check may run at nowUtc: enabled, and the last
    //DH1KLM answer is missing, older than the interval, or in the future (the
    //DH1KLM clock moved backward).
    static bool automaticCheckDue(const QDateTime& nowUtc);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} //DH1KLM namespace AetherSDR
