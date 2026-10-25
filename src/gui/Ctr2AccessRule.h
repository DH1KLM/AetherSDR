#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace AetherSDR {

//DH1KLM The directories udev reads rules from, highest precedence first. A file
//DH1KLM in an earlier directory masks one of the same name in every later one.
inline QStringList udevRuleDirs()
{
    return {QStringLiteral("/etc/udev/rules.d"), QStringLiteral("/run/udev/rules.d"),
            QStringLiteral("/usr/local/lib/udev/rules.d"),
            QStringLiteral("/usr/lib/udev/rules.d"), QStringLiteral("/lib/udev/rules.d")};
}

//DH1KLM Whether the copy of `name` that udev actually uses (the first one found in
//DH1KLM `dirs`) is `rule`. A copy further down does not count: an older rule in
//DH1KLM /etc masks the packaged one in /usr/lib. A link to /dev/null is how an
//DH1KLM administrator turns a rule off; it counts as settled, so it is never
//DH1KLM written through.
inline bool udevRuleInEffect(const QStringList& dirs, const QString& name, const QString& rule)
{
    for (const QString& dir : dirs) {
        const QString path = dir + QLatin1Char('/') + name;
        const QFileInfo info(path);
        if (info.isSymLink() && info.symLinkTarget() == QLatin1String("/dev/null"))
            return true;
        QFile file(path);
        if (file.exists())
            return file.open(QIODevice::ReadOnly) && QString::fromUtf8(file.readAll()) == rule;
    }
    return false;
}

} //DH1KLM namespace AetherSDR
