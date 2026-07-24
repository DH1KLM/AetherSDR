// aetherd HL2 Phase 1b — Hl2Backend seam test. A capped fake HL2 on localhost
// lets the backend connect and produce a panadapter frame; verifies the
// IRadioBackend contract: capabilities (family=hl2, RX-only), connected on first
// EP6, spectrumFrameReady wired to the data plane, sliceChanged on control
// intents, setKeying as a no-op, invokeExtension's async-error stub, and
// disconnected on stop. (Audio demod itself is covered by hl2_rxdsp_test.)

#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
namespace hl2 = AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static QByteArray fakeEp6(std::uint32_t seq)
{
    QByteArray p(static_cast<int>(hl2::kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;
    b[8 + hl2::kFrameSize] = b[9 + hl2::kFrameSize] = b[10 + hl2::kFrameSize] = 0x7F;
    return p;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    // ---- capped fake HL2: a bounded number of EP6 so the WDSP demod stays quick ----
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();
    std::uint32_t seq = 0;
    constexpr std::uint32_t kCap = 14;   // ~1764 samples: crosses one 1024 spectrum frame
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            if (seq < kCap)
                radio.writeDatagram(fakeEp6(seq++), dg.senderAddress(), dg.senderPort());
        }
    });

    Hl2Backend backend;

    // ---- capabilities: RX-only HL2 ----
    const RadioCapabilities caps = backend.capabilities();
    check(caps.family == QLatin1String("hl2"), "family is hl2");
    check(!caps.canTransmit, "canTransmit is false (RX-only)");
    check(caps.maxSlices == 1, "one slice");
    check(caps.sampleRatesHz.contains(48000) && caps.sampleRatesHz.contains(384000), "sample rates");
    check(caps.extensionNamespaces.isEmpty(), "no extension namespaces advertised");

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy disconnectedSpy(&backend, &IRadioBackend::disconnected);
    QSignalSpy errSpy(&backend, &IRadioBackend::extensionError);
    int specCount = 0, sliceCount = 0;
    qsizetype lastSpecBytes = 0;
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &backend,
                     [&](int, const QByteArray& ba) { ++specCount; lastSpecBytes = ba.size(); });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int, const SliceDelta&) { ++sliceCount; });

    // ---- connect ----
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radioPort;
    backend.connectRadio(req);
    check(sliceCount >= 1 && !connectedSpy.count(), "connect publishes initial slice state, connected still pending");

    spin(5000);   // let the capped ping-pong deliver EP6 + one spectrum frame

    check(connectedSpy.count() == 1, "connected() on the first EP6");
    check(backend.isConnected(), "isConnected() true");
    check(specCount >= 1, "spectrumFrameReady wired through the seam");
    check(lastSpecBytes == static_cast<qsizetype>(1024 * sizeof(float)),
          "spectrum payload is fftSize float32");

    // ---- control intents each emit a slice delta ----
    const int sliceBefore = sliceCount;
    backend.setSliceFrequency(0, 14'100'000.0);
    backend.setSliceMode(0, QStringLiteral("LSB"));
    backend.setSliceFilter(0, 300, 2700);
    check(sliceCount >= sliceBefore + 3, "freq/mode/filter each emit sliceChanged");

    // ---- keying is a no-op (RX-only) ----
    backend.setKeying(true);
    check(backend.isConnected(), "setKeying does not disrupt / cannot key");

    // ---- invokeExtension honors the async contract ----
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("noop"), 42, {});
    check(errSpy.count() == 1, "awaited invokeExtension -> one extensionError");
    check(errSpy.first().at(0).toULongLong() == 42u, "extensionError carries the requestId");

    // ---- disconnect ----
    backend.disconnectRadio();
    spin(50);
    check(disconnectedSpy.count() == 1, "disconnected() on stop");
    check(!backend.isConnected(), "isConnected() false after disconnect");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
