#include "core/FrontPanelProtocol.h"
#include "core/backends/anan/AnanVitals.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cmath>
#include <limits>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } };
    using namespace AetherSDR::frontpanel;
#ifdef Q_OS_WIN
    check(serialPortName(QStringLiteral(" 3 ")) == QStringLiteral("COM3"), "legacy numeric Windows port resolves to COM3");
#endif
    check(serialPortName(QStringLiteral(" COM3 ")) == QStringLiteral("COM3"), "named port remains stable");
    check(serialPortName(QStringLiteral("/dev/ttyACM0")) == QStringLiteral("/dev/ttyACM0"), "Unix serial path is preserved");
    const auto up = parse(QStringLiteral("ZZZU012"));
    const auto down = parse(QStringLiteral("ZZZD012"));
    check(up && down && up->value == 12 && down->value == -12, "VFO signed step count");
    const auto e = parse(QStringLiteral("ZZZE513"));
    check(e && e->kind == Kind::Encoder && e->number == 1 && e->value == -3, "encoder51 is reverse encoder1");
    const auto e20 = parse(QStringLiteral("ZZZE709"));
    check(e20 && e20->number == 20 && e20->value == -9, "last reverse encoder");
    for (int state = 0; state <= 2; ++state) {
        const auto button = parse(QStringLiteral("ZZZP21%1").arg(state));
        check(button && button->number == 21 && button->value == state, "release press long-press kept distinct");
    }
    const auto identity = parse(QStringLiteral("ZZZS0501123"));
    check(identity && identity->number == 5 && identity->value == 1123, "G2-V2 product hardware firmware identity");
    const auto registration = parse(QStringLiteral("ZZGA") + kG2V2Id);
    check(registration && registration->kind == Kind::Register, "G2-V2 directed CAT registration");
    const auto andromeda = parse(QStringLiteral("ZZZS0101006"));
    check(andromeda && andromeda->number == 1 && andromeda->value == 1006,
          "physical COM3 Andromeda identity uses two product digits, two hardware digits, three firmware digits");
    for (const auto& bad : {"ZZZE000", "ZZZE501", "ZZZE711", "ZZZP001", "ZZZP213", "ZZZU-1", "ZZZU9999", "ZZZS;", "ZZZS101006", "ZZTX1", "ZZGAunknown"}) {
        check(!parse(QString::fromLatin1(bad)), "malformed or non-panel frame refused");
    }
    using namespace AetherSDR::anan;
    check(hasNativeVitals(AnanModel::Anan7000, 5) && !hasNativeVitals(AnanModel::Anan200D, 5), "logical override owns sensor qualification");
    check(!hasNativeVitals(AnanModel::RedPitaya, 5) && !hasNativeVitals(AnanModel::Automatic, 5),
          "DIY and ambiguous board5 profiles require explicit sensor qualification");
    check(defaultVitals(AnanModel::Automatic, 10).sensitivityMvPerAmp == 66.23,
          "automatic Saturn uses its documented current sensor scale");
    const auto calibration = defaultVitals(AnanModel::Anan7000);
    const auto amps = paCurrentAmps(1000, calibration);
    check(amps && std::abs(*amps - ((1000 * 5000.0 / 4095 - 340) / 88)) < 1e-9, "7000 current matches Thetis sensor transfer");
    check(paCurrentAmps(0, calibration) == 0.0 && !paCurrentAmps(4096, calibration), "sensor clamps negative zero and refuses out of range ADC");
    auto invalid = vitalOptions(calibration); invalid.insert(QStringLiteral("sensitivityMvPerAmp"), 0.0);
    check(!parseVitals(invalid, AnanModel::Anan7000), "zero current sensitivity refused");
    auto diy = defaultVitals(AnanModel::Anan200D);
    diy.diy = true;
    const auto stored = QJsonDocument::fromJson(QJsonDocument(QJsonObject::fromVariantMap(vitalOptions(diy))).toJson())
        .object().toVariantMap();
    const auto restored = parseVitals(stored, AnanModel::Anan200D);
    check(restored && restored->diy && restored->offsetMv == diy.offsetMv
          && restored->referenceVolts == diy.referenceVolts && restored->sensitivityMvPerAmp == diy.sensitivityMvPerAmp,
          "DIY flag and integral calibration values survive the settings JSON round trip");
    for (const QVariant& bad : {QVariant(true), QVariant(QStringLiteral("5"))}) {
        auto malformed = stored; malformed.insert(QStringLiteral("referenceVolts"), bad);
        check(!parseVitals(malformed, AnanModel::Anan200D), "boolean and string sensor scales remain rejected");
    }
    return failures ? 1 : 0;
}
