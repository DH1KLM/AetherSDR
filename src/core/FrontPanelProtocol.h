#pragma once
#include <QString>
#include <optional>

namespace AetherSDR::frontpanel {
inline QString serialPortName(const QString& value)
{
    const QString name = value.trimmed();
#ifdef Q_OS_WIN
    bool valid = false;
    const int number = name.toInt(&valid);
    if (valid && number > 0 && QString::number(number) == name) {
        return QStringLiteral("COM%1").arg(number);
    }
#endif
    return name;
}
inline const QString kG2V2Id = QStringLiteral("9f2b6c5a-4d7e-4c3b-9a21-3f8d0e6b12c4");
enum class Kind { Vfo, Encoder, Button, Identity, Register, Unregister };
struct Event { Kind kind; int number = 0; int value = 0; };
inline std::optional<Event> parse(const QString& command)
{
    if (command.size() < 5 || command.size() > 40) { return std::nullopt; }
    const QString name = command.left(4).toUpper();
    const QString arg = command.mid(4);
    if ((name == QLatin1String("ZZGA") || name == QLatin1String("ZZGR")) && arg.toLower() == kG2V2Id) {
        return Event{name == QLatin1String("ZZGA") ? Kind::Register : Kind::Unregister};
    }
    for (const QChar ch : arg) { if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) { return std::nullopt; } }
    bool valid = false;
    const int value = arg.toInt(&valid);
    if (!valid) { return std::nullopt; }
    if ((name == QLatin1String("ZZZU") || name == QLatin1String("ZZZD")) && arg.size() <= 3 && value <= 999) {
        return Event{Kind::Vfo, 0, name == QLatin1String("ZZZU") ? value : -value};
    }
    if (name == QLatin1String("ZZZS") && arg.size() == 6) { return Event{Kind::Identity, value / 100000, value % 100000}; }
    if (name == QLatin1String("ZZZE") && arg.size() == 3) {
        const int encoder = value / 10, steps = value % 10;
        if (encoder >= 1 && encoder <= 20) { return Event{Kind::Encoder, encoder, steps}; }
        if (encoder >= 51 && encoder <= 70) { return Event{Kind::Encoder, encoder - 50, -steps}; }
    }
    if (name == QLatin1String("ZZZP") && arg.size() == 3 && value / 10 >= 1 && value / 10 <= 99 && value % 10 <= 2) {
        return Event{Kind::Button, value / 10, value % 10};
    }
    return std::nullopt;
}
} // namespace AetherSDR::frontpanel
