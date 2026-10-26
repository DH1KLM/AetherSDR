// Socket-free behavior at the shared GUI/model seam. Real capability records
// select Flex pixels, native analyzer points, or a fixed-geometry local no-op.
#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"
#include "gui/PanSpanControlGate.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
class NativeBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QStringList ids;
    QList<int> points;
    bool refuseClose{false};
    bool deferredCreate{false};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setPanPixelWidth(const QString& id, int width) override { ids.append(id); points.append(width); }
    bool createPanadapter() override
    {
        if (!deferredCreate) { publishCreated(); }
        return true;
    }
    void publishCreated()
    {
        emit panCenterBandwidthChanged(QStringLiteral("native-new"), 7.1, 0.384);
    }
    bool removePanadapter(const QString&) override { return !refuseClose; }
};
void native(const QString& family)
{
    RadioModel specimen;
    check(specimen.rebuildBackendForTest(family), "production family backend constructed without dialing");
    const RadioCapabilities caps = specimen.backendCapabilities();
    const bool spanDeclared = caps.panDisplayControl && caps.panDisplayControl->declaresSpan();
    check(spanFixedOnPan(true, caps.panDisplayControl.has_value(), spanDeclared, false)
              == (family == QLatin1String("sim")),
          "only the Demo's fixed span dims the -/+ pair");
    RadioModel radio;
    auto owned = std::make_unique<NativeBackend>();
    NativeBackend* backend = owned.get();
    backend->caps = caps;
    // The injected topology has capacity; cold discovery defaults can be one.
    backend->caps.maxPanadapters = 5;
    radio.setBackendForTest(std::move(owned), family);
    emit backend->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
    PanadapterModel* pan = radio.panadapter(RadioModel::neutralPanIdStringForTest(0));
    check(pan != nullptr, "native pan published through production mapping");
    const ReceiveDispatch result = radio.requestPanDimensions(pan, 1111, 700, 2222);
    const bool analyzer = caps.panDisplayControl && caps.panDisplayControl->supports(PanDisplayField::SpectrumPoints);
    check(result == (analyzer ? ReceiveDispatch::Dispatched : ReceiveDispatch::LocalOnly),
          "native dimensions distinguish actual analyzer dispatch from fixed geometry");
    check(analyzer ? backend->points == QList<int>{2222} && backend->ids == QStringList{QStringLiteral("native-first")}
                   : backend->points.isEmpty(), "only declared analyzer receives actual local points and backend ID");
    check(radio.requestPanDimensions(pan, 0, 700, 2222) == ReceiveDispatch::Unsupported
        && radio.requestPanDimensions(nullptr, 1111, 700, 2222) == ReceiveDispatch::Unsupported,
          "invalid dimensions and missing pan are refused");
    if (caps.panLifecycleControl && caps.panLifecycleControl->creation == PanLifecycleControl::Creation::Native) {
        int calls = 0;
        QPointer<PanadapterModel> created;
        const ReceiveDispatch dispatched = radio.requestPanCreation(&radio, [&](const RadioControlReply& reply, PanadapterModel* value) {
            ++calls; if (reply.kind == RadioControlReply::Kind::Accepted) { created = value; }
        });
        check(dispatched == ReceiveDispatch::Dispatched && calls == 1 && created
            && created->panId() == RadioModel::neutralPanIdStringForTest(1), "native immediate creation timing and exact identity preserved");
        backend->refuseClose = true;
        check(!radio.removePanadapter(created->panId(), PanCloseIntent::ExplicitTeardown)
            && radio.panadapter(created->panId()) == created, "explicit teardown never overrides native refusal");
    }
}

void deferredNativeCreation()
{
    RadioModel specimen;
    check(specimen.rebuildBackendForTest(QStringLiteral("anan")), "ANAN capabilities available without hardware");
    RadioModel radio;
    auto owned = std::make_unique<NativeBackend>();
    NativeBackend* backend = owned.get();
    backend->caps = specimen.backendCapabilities();
    backend->caps.maxPanadapters = 5;
    backend->deferredCreate = true;
    radio.setBackendForTest(std::move(owned), QStringLiteral("anan"));
    emit backend->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
    int calls = 0;
    QPointer<PanadapterModel> created;
    check(radio.requestPanCreation(&radio, [&](const RadioControlReply& reply, PanadapterModel* pan) {
        ++calls;
        if (reply.kind == RadioControlReply::Kind::Accepted) { created = pan; }
    }, false) == ReceiveDispatch::Dispatched && calls == 0,
          "ANAN asynchronous planning is admitted without a premature refusal");
    check(radio.requestPanCreation(&radio, {}, false) == ReceiveDispatch::Unsupported,
          "overlapping native creation cannot claim the same publication");
    backend->publishCreated();
    check(calls == 1 && created && created->panId() == RadioModel::neutralPanIdStringForTest(1),
          "deferred ANAN publication completes once with its mapped pan identity");
    backend->publishCreated();
    check(calls == 1, "repeated native status cannot complete creation twice");
    check(created && radio.removePanadapter(created->panId()), "additional ANAN window remains closable");
}
} // namespace
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("pan-dimension-routing"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    for (const char* family : {"hl2", "anan", "icom", "sim"}) { native(QString::fromLatin1(family)); }
    deferredNativeCreation();
#ifdef AETHER_BACKEND_RTL
    native(QStringLiteral("rtl"));
#else
    {
        RadioModel radio;
        check(!radio.rebuildBackendForTest(QStringLiteral("rtl")), "optional RTL backend remains unavailable when not compiled");
    }
#endif
    {
        PanControlFixture f;
        PanadapterModel* first = f.publish(QStringLiteral("0x40000000"));
        f.publish(QStringLiteral("0x40000001"));
        f.clear();
        check(f.radio.requestPanDimensions(first, 1111, 700, 2222) == ReceiveDispatch::Dispatched
            && f.backend->commands() == QStringList{QStringLiteral("display pan set 0x40000000 xpixels=1111 ypixels=700")},
              "Flex sends actual widget dimensions as one paired request");
        const auto flexDisplay = f.radio.backendCapabilities().panDisplayControl;
        check(!spanFixedOnPan(true, flexDisplay.has_value(),
                  flexDisplay && flexDisplay->declaresSpan(), false),
              "Flex declares its span, so its -/+ pair stays live");
    }
    QFile source(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
    check(source.open(QIODevice::ReadOnly), "GUI dimension source readable");
    const QByteArray code = source.readAll();
    check(code.contains("requestPanDimensions(pan, xpix, ypix, panLocalSpectrumPointsFor(sw))")
        && !code.contains("display pan set %1 xpixels="), "GUI uses typed dimensions with distinct radio and analyzer counts");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
