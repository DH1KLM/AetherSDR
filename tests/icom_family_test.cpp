// IcomCIV — backend selection. Proves `family = "icom"` actually reaches
// IcomCivBackend through RadioModel's real swap path, and that the capabilities
// the model then reports are Icom-shaped rather than the Flex defaults.
//
// Modelled on hl2_family_transition_test: connectToRadio() rebuilds the backend
// SYNCHRONOUSLY before any network I/O, so an unroutable address exercises the
// whole post-swap state without hardware.
//
// This is the test that would have caught the gap where every layer below was
// written, tested and green while nothing in the application could construct it.

#include "models/RadioModel.h"
#include "core/RadioDiscovery.h"
#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static RadioInfo infoFor(const QString& family)
{
    RadioInfo i;
    i.family  = family;
    i.serial  = QStringLiteral("ICOM-TEST-1");
    i.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    i.port    = 50001;
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    RadioModel model;

    // ---- the factory selects it ------------------------------------------
    model.connectToRadio(infoFor(QStringLiteral("icom")));
    check(model.family() == QStringLiteral("icom"), "the model adopts the icom family");
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) != nullptr,
          "and makeBackend() actually produced an IcomCivBackend");

    // ---- and the capabilities are Icom-shaped, not Flex defaults ---------
    const RadioCapabilities caps = model.backend()->capabilities();
    check(caps.family == QStringLiteral("icom"), "capabilities report the icom family");

    // The three that would be silently wrong if the fall-through to FlexBackend
    // were still happening, and each has a real consequence:
    check(!caps.hostModulates,
          "the RADIO modulates — true would open the host mic on a radio that never uses it");
    check(!caps.hasDaxStreams,
          "no IQ on any networked Icom — a true here offers a DAX-IQ path that cannot exist");
    check(caps.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "an Icom remembers its own state, so the client restores NOTHING");

    // A pure seam backend owns no RadioConnection and no PanadapterStream, so
    // setupBackend()'s dynamic_cast chain must SKIP it — the same shape as HL2.
    check(model.backend()->ownsRxAudio(),
          "audio arrives over the seam, which is what the RX-audio wiring keys off");

    // ---- unknown families still fall through to Flex ----------------------
    model.connectToRadio(infoFor(QStringLiteral("nonsuch")));
    check(model.backend()->capabilities().family != QStringLiteral("icom"),
          "an unrecognised family does NOT get the Icom backend");

    // ---- round trip leaves a clean model ----------------------------------
    // Flex -> Icom -> Flex. The swap destroys the old backend, and every
    // connection made in setupBackend() has it as sender or receiver, so a
    // leaked one would show up here as a crash rather than a wrong value.
    model.connectToRadio(infoFor(QStringLiteral("flex")));
    model.connectToRadio(infoFor(QStringLiteral("icom")));
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) != nullptr,
          "flex -> icom swaps in cleanly");
    model.connectToRadio(infoFor(QStringLiteral("flex")));
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) == nullptr,
          "and icom -> flex swaps back out");
    check(model.family() == QStringLiteral("flex"), "leaving the model Flex-capable");

    if (g_failures == 0)
        std::printf("icom_family_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
