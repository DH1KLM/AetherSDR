#include "FrontPanelModel.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include "PanadapterModel.h"
#include "TransmitModel.h"
#include "core/FrontPanelProtocol.h"
#include "core/RadioSettingsScope.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include <QList>
#include <QHostAddress>
#include <QJsonArray>
#include <QVariantMap>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <memory>

namespace AetherSDR {
namespace {
QPointer<FrontPanelModel> activePanel;
QStringList menuActions(int menu)
{
    static const QList<QStringList> pages{
        {"menuNext", "nr", "unavailable:ANAN noise blanker", "unavailable:Spectral noise blanker", "unavailable:ANAN automatic notch", "agc", "attenuationStep", "antennaStep"},
        {"menuNext", "unavailable:Diversity", "unavailable:Diversity setup", "gainMenu", "filterMenu", "mute", "antennaStep", "none"},
        {"menuNext", "bandMenu", "modeMenu", "bandstackMenu", "swapVfo", "stepMenu", "bandstack", "none"},
        {"menuNext", "txSettings", "unavailable:PureSignal setup", "unavailable:PureSignal", "mox", "tune", "unavailable:PureSignal two-tone", "unavailable:ANAN ATU"},
        {"menuNext", "unavailable:RX meter mode", "unavailable:TX meter mode", "unavailable:Thetis display mode cycle", "centerDisplay", "displayMenu", "zoomIn", "none"},
        {"menuNext", "recordAudio", "playAudio", "none", "audioSettings", "unavailable:VAC1", "unavailable:VAC2", "none"},
        {"menuNext", "unavailable:Duplex", "monitor", "receiverToggle", "unavailable:Sub receiver", "equalizer", "setup", "menuTest"},
        {"menuFirst", "quickSave", "quickRestore", "filterUp", "filterDown", "bandUp", "bandDown", "none"}
    };
    return pages.at(std::clamp(menu, 0, 7));
}
QString menuTitle(int menu)
{
    return QStringList{"Quick", "RX", "VFO", "TX", "Display", "Audio", "Setup", "Test"}.at(std::clamp(menu, 0, 7));
}
QJsonObject defaults()
{
    QJsonObject bindings;
    const auto put = [&](const QString& key, const QString& action, int target = 0) {
        bindings.insert(key, QJsonObject{{QStringLiteral("action"), action}, {QStringLiteral("target"), target}});
    };
    for (int n = 1; n <= 20; ++n) {
        put(QStringLiteral("e%1").arg(n), QStringLiteral("none"));
        put(QStringLiteral("l%1").arg(n), QStringLiteral("none"));
    }
    for (int n = 1; n <= 99; ++n) {
        put(QStringLiteral("p%1").arg(n), QStringLiteral("none"));
        put(QStringLiteral("h%1").arg(n), QStringLiteral("none"));
    }
    put(QStringLiteral("e1"), QStringLiteral("afGain"), 1); put(QStringLiteral("e2"), QStringLiteral("agcT"), 1);
    put(QStringLiteral("e3"), QStringLiteral("afGain"), 2); put(QStringLiteral("e4"), QStringLiteral("agcT"), 2);
    put(QStringLiteral("e5"), QStringLiteral("filterHigh")); put(QStringLiteral("e6"), QStringLiteral("filterLow"));
    put(QStringLiteral("e9"), QStringLiteral("rit")); put(QStringLiteral("e10"), QStringLiteral("xit"));
    put(QStringLiteral("e12"), QStringLiteral("drive"));
    put(QStringLiteral("e7"), QStringLiteral("unavailable:Diversity gain"));
    put(QStringLiteral("e8"), QStringLiteral("unavailable:Diversity phase"));
    put(QStringLiteral("e11"), QStringLiteral("multifunction"));
    put(QStringLiteral("p1"), QStringLiteral("mute"), 1); put(QStringLiteral("p3"), QStringLiteral("mute"), 2);
    put(QStringLiteral("p9"), QStringLiteral("clearOffsets"));
    put(QStringLiteral("p31"), QStringLiteral("modeUp")); put(QStringLiteral("p34"), QStringLiteral("modeDown"));
    put(QStringLiteral("p43"), QStringLiteral("receiver"));
    put(QStringLiteral("p44"), QStringLiteral("lock"));
    put(QStringLiteral("p47"), QStringLiteral("mox"));
    put(QStringLiteral("p48"), QStringLiteral("tune"));
    const QMap<int, QString> unsupportedButtons{{5, "Filter reset"}, {7, "Diversity"}, {11, "Multifunction selection"},
        {29, "Band shift"}, {30, "Band up"}, {32, "Filter up"}, {33, "Band down"}, {35, "Filter down"},
        {36, "VFO A to B"}, {37, "VFO B to A"}, {38, "Split"}, {42, "RIT/XIT selection"},
        {45, "CTUN"}, {46, "Start/stop radio"}, {49, "PureSignal"}, {50, "PureSignal two-tone"}};
    for (auto it = unsupportedButtons.cbegin(); it != unsupportedButtons.cend(); ++it) {
        put(QStringLiteral("p%1").arg(it.key()), QStringLiteral("unavailable:") + it.value());
    }
    for (int n = 21; n <= 28; ++n) {
        put(QStringLiteral("p%1").arg(n), QStringLiteral("softkey%1").arg(n - 20));
    }
    const QMap<int, QString> connectedButtons{{5, "filterReset"}, {11, "multifunctionMenu"}, {29, "shift"},
        {30, "bandUp"}, {32, "filterUp"}, {33, "bandDown"}, {35, "filterDown"},
        {36, "copyAtoB"}, {37, "copyBtoA"}, {45, "ctun"}};
    for (auto it = connectedButtons.cbegin(); it != connectedButtons.cend(); ++it) {
        put(QStringLiteral("p%1").arg(it.key()), it.value());
    }
    put(QStringLiteral("h31"), QStringLiteral("modeMenu")); put(QStringLiteral("h34"), QStringLiteral("modeMenu"));
    put(QStringLiteral("h32"), QStringLiteral("filterMenu")); put(QStringLiteral("h35"), QStringLiteral("filterMenu"));
    put(QStringLiteral("l1"), QStringLiteral("mox")); put(QStringLiteral("l3"), QStringLiteral("tune"));
    put(QStringLiteral("l8"), QStringLiteral("rit")); put(QStringLiteral("l9"), QStringLiteral("xit"));
    put(QStringLiteral("l10"), QStringLiteral("receiver")); put(QStringLiteral("l11"), QStringLiteral("lock"));
    const QMap<int, QString> unsupportedIndicators{{2, "ATU ready"}, {4, "PureSignal"}, {5, "Diversity"}, {6, "Band shift"}, {7, "CTUN"}};
    for (auto it = unsupportedIndicators.cbegin(); it != unsupportedIndicators.cend(); ++it) {
        put(QStringLiteral("l%1").arg(it.key()), QStringLiteral("unavailable:") + it.value());
    }
    put(QStringLiteral("l6"), QStringLiteral("shift")); put(QStringLiteral("l7"), QStringLiteral("ctun"));
    return {{QStringLiteral("enabled"), false}, {QStringLiteral("panel"), QStringLiteral("andromeda")},
        {QStringLiteral("serialPort"), QString()}, {QStringLiteral("listenAddress"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("tcpPort"), 31001}, {QStringLiteral("peerAddress"), QString()},
        {QStringLiteral("radioId"), QString()}, {QStringLiteral("bindings"), bindings}};
}
QString bindingKind(QChar prefix)
{
    return prefix == QLatin1Char('e') ? QStringLiteral("encoder")
        : prefix == QLatin1Char('l') ? QStringLiteral("indicator") : QStringLiteral("button");
}
}
QStringList FrontPanelModel::actions(const QString& kind)
{
    QStringList result;
    if (kind == QLatin1String("encoder")) {
        result = {QStringLiteral("none"), QStringLiteral("vfo"), QStringLiteral("afGain"), QStringLiteral("agcT"),
            QStringLiteral("rfGain"), QStringLiteral("filterLow"), QStringLiteral("filterHigh"), QStringLiteral("drive"),
            QStringLiteral("rit"), QStringLiteral("xit"), QStringLiteral("cwSpeed"), QStringLiteral("monitorLevel")};
    } else if (kind == QLatin1String("indicator")) {
        result = {QStringLiteral("none"), QStringLiteral("mox"), QStringLiteral("tune"), QStringLiteral("mute"),
            QStringLiteral("lock"), QStringLiteral("rit"), QStringLiteral("xit"), QStringLiteral("receiver"),
            QStringLiteral("nb"), QStringLiteral("nr"), QStringLiteral("anf"), QStringLiteral("monitor")};
    } else {
        result = {QStringLiteral("none"), QStringLiteral("mox"), QStringLiteral("pttHold"), QStringLiteral("tune"),
            QStringLiteral("mute"), QStringLiteral("lock"), QStringLiteral("rit"), QStringLiteral("xit"),
            QStringLiteral("clearOffsets"), QStringLiteral("receiver"), QStringLiteral("nb"), QStringLiteral("nr"),
            QStringLiteral("anf"), QStringLiteral("agc"), QStringLiteral("modeUp"), QStringLiteral("modeDown"), QStringLiteral("monitor")};
    }
    if (kind == QLatin1String("encoder")) { result.append({"multifunction", "audioPan", "micGain", "compression", "cwPitch", "displayPan", "displayZoom", "tuneStep", "masterGain"}); }
    else if (kind == QLatin1String("indicator")) { result.append({QStringLiteral("shift"), QStringLiteral("ctun")}); }
    else {
        result.append({"filterReset", "multifunctionMenu", "shift", "bandUp", "bandDown", "filterUp", "filterDown",
            "copyAtoB", "copyBtoA", "swapVfo", "ctun", "modeMenu", "filterMenu", "bandMenu", "stepMenu",
            "gainMenu", "setup", "txSettings", "displayMenu", "audioSettings", "equalizer", "centerDisplay",
            "zoomIn", "zoomOut", "recordAudio", "playAudio", "receiverMenu", "receiverToggle", "bandstack", "bandstackMenu", "quickSave", "quickRestore", "attenuationStep", "antennaStep"});
        for (int n = 1; n <= 8; ++n) { result.append(QStringLiteral("softkey%1").arg(n)); }
    }
    const QJsonObject bindings = standardAndromedaBindings();
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        const QString action = it.value().toObject().value(QStringLiteral("action")).toString();
        if (bindingKind(it.key().front()) == kind && action.startsWith(QLatin1String("unavailable:")) && !result.contains(action)) {
            result.append(action);
        }
    }
    return result;
}
QJsonObject FrontPanelModel::standardAndromedaBindings()
{
    static const QJsonObject bindings = defaults().value(QStringLiteral("bindings")).toObject();
    return bindings;
}
void FrontPanelModel::setMultifunctionAction(const QString& action)
{
    if (action == QLatin1String("multifunction") || action == QLatin1String("rit") || action == QLatin1String("xit")
        || action.startsWith(QLatin1String("unavailable:")) || !actions(QStringLiteral("encoder")).contains(action)) { return; }
    m_multifunction = action;
    emit stateChanged();
}
void FrontPanelModel::requestCtun(bool toggle)
{
    QPointer<IRadioBackend> backend = m_model.backend();
    if (!backend || !m_identified || !m_model.isConnected()) { return; }
    static quint64 next = 0x465043000000ULL;
    const quint64 request = ++next, epoch = m_transportEpoch;
    auto* guard = new QTimer(this); guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, guard, &QObject::deleteLater); guard->start(2000);
    connect(backend, &IRadioBackend::extensionResult, guard, [this, guard, backend, request, epoch, toggle](quint64 id, const QVariant& value) {
        if (id != request || !guard->isActive()) { return; }
        guard->stop();
        guard->deleteLater();
        if (!backend || backend != m_model.backend() || epoch != m_transportEpoch || !m_identified || !m_model.isConnected()) { return; }
        const QVariantMap state = value.toMap();
        m_ctun = state.value(QStringLiteral("ctun")).toBool();
        if (toggle && state.value(QStringLiteral("ctunAvailable")).toBool()) {
            m_model.invokeBackendExtension(QStringLiteral("anan"), QStringLiteral("ctun.set"), ++next, !m_ctun);
            requestCtun(false);
        }
        updateIndicators(); emit stateChanged();
    });
    m_model.invokeBackendExtension(QStringLiteral("anan"), QStringLiteral("hw.get"), request);
}

FrontPanelModel::FrontPanelModel(RadioModel& model)
    : QObject(&model), m_model(model), m_producer(model.registerTxProducer(this, true)), m_config(defaults())
{
    int version = 0;
    AppSettings::FeatureReadStatus readStatus;
    const auto stored = RadioSettingsScope(QStringLiteral("local"), QStringLiteral("Andromeda"))
        .featureExact(QStringLiteral("FrontPanel"), &version, &readStatus);
    m_futureVersion = version > 1 || readStatus == AppSettings::FeatureReadStatus::Corrupt
        || readStatus == AppSettings::FeatureReadStatus::Unavailable;
    if (!stored.isEmpty() && !m_futureVersion) {
        const QVariantMap values = stored.toVariantMap();
        const QJsonObject old = m_config;
        if (!applyConfiguration(values, false)) { m_config = old; }
    }
    m_bindings = m_config.value(QStringLiteral("bindings")).toObject();
    m_tick.setInterval(200);
    connect(&m_tick, &QTimer::timeout, this, &FrontPanelModel::updateIndicators);
    m_tick.start();
    m_retry.setInterval(3000);
    connect(&m_retry, &QTimer::timeout, this, &FrontPanelModel::refresh);
    m_retry.start();
    connect(&m_model, &RadioModel::capabilitiesChanged, this, &FrontPanelModel::refresh, Qt::QueuedConnection);
    connect(&m_model, &RadioModel::connectionStateChanged, this, [this](bool) { closeTransport(); refresh(); });
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (m_server.hasPendingConnections()) {
            QTcpSocket* peer = m_server.nextPendingConnection();
            const QHostAddress expected(m_config.value(QStringLiteral("peerAddress")).toString());
            if (m_peer || (!expected.isNull() && peer->peerAddress() != expected)) { peer->abort(); peer->deleteLater(); continue; }
            m_peer = peer; m_buffer.clear(); m_identified = m_registered = false;
            connect(peer, &QTcpSocket::readyRead, this, [this, peer] { if (m_peer == peer) { consume(peer->readAll()); } });
            connect(peer, &QTcpSocket::disconnected, this, [this, peer] {
                if (m_peer == peer) { m_model.abortTxProducerInputs(m_producer, TransmitModel::PttSource::Footswitch); closeTransport(); }
                peer->deleteLater();
            });
            m_status = QStringLiteral("TCP panel connected; waiting for registration and identity");
            emit stateChanged();
        }
    });
#ifdef HAVE_SERIALPORT
    connect(&m_serial, &QSerialPort::readyRead, this, [this] { consume(m_serial.readAll()); });
    connect(&m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (!m_opening && error != QSerialPort::NoError && error != QSerialPort::TimeoutError) {
            m_status = m_serial.errorString(); closeTransport(); emit stateChanged();
        }
    });
#endif
}
FrontPanelModel::~FrontPanelModel() { closeTransport(); m_producer.invalidate(); }
QVariantMap FrontPanelModel::state() const
{
    QVariantMap result = m_config.toVariantMap();
    result.insert(QStringLiteral("supported"), m_model.backendDeclaresExtension(QStringLiteral("anan")));
    result.insert(QStringLiteral("identified"), m_identified);
    result.insert(QStringLiteral("hardwareVersion"), m_hardwareVersion);
    result.insert(QStringLiteral("firmwareVersion"), m_firmwareVersion);
    result.insert(QStringLiteral("menu"), menuTitle(m_menu));
    result.insert(QStringLiteral("softkeys"), menuActions(m_menu));
    result.insert(QStringLiteral("multifunction"), m_multifunction);
    result.insert(QStringLiteral("selectingMultifunction"), m_selectingMultifunction);
    result.insert(QStringLiteral("shift"), m_shift);
    result.insert(QStringLiteral("ctun"), m_ctun);
    result.insert(QStringLiteral("status"), m_status);
    result.insert(QStringLiteral("lastInput"), m_lastInput);
    result.insert(QStringLiteral("lastControl"), m_lastControl);
    return result;
}
bool FrontPanelModel::configure(const QVariantMap& values) { return applyConfiguration(values, true); }
bool FrontPanelModel::applyConfiguration(const QVariantMap& values, bool persist)
{
    if (m_futureVersion) { m_status = QStringLiteral("Controller settings are unavailable or use a newer schema"); emit stateChanged(); return false; }
    QJsonObject next = QJsonObject::fromVariantMap(values);
    if (persist) { next.insert(QStringLiteral("radioId"), m_model.settingsScope().radioId()); }
    const QString panel = next.value(QStringLiteral("panel")).toString();
    const bool enabled = next.value(QStringLiteral("enabled")).toBool();
    if (persist && enabled && !m_model.isConnected()) {
        m_status = QStringLiteral("Connect the selected ANAN before enabling its panel"); emit stateChanged(); return false;
    }
    const int port = next.value(QStringLiteral("tcpPort")).toInt();
    const QString serial = frontpanel::serialPortName(next.value(QStringLiteral("serialPort")).toString());
    next.insert(QStringLiteral("serialPort"), serial);
    const QHostAddress bind(next.value(QStringLiteral("listenAddress")).toString());
    const QString peerText = next.value(QStringLiteral("peerAddress")).toString();
    const QHostAddress peer(peerText);
    auto bindings = next.value(QStringLiteral("bindings")).toObject();
    const QMap<QString, QString> completed{{"Filter reset", "filterReset"}, {"Multifunction selection", "multifunctionMenu"},
        {"Multifunction encoder", "multifunction"}, {"Band shift", "shift"}, {"Band up", "bandUp"}, {"Band down", "bandDown"},
        {"Filter up", "filterUp"}, {"Filter down", "filterDown"}, {"VFO A to B", "copyAtoB"}, {"VFO B to A", "copyBtoA"}, {"CTUN", "ctun"}};
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        QJsonObject entry = it.value().toObject();
        const QString old = entry.value(QStringLiteral("action")).toString();
        QString replacement;
        if (old.startsWith(QLatin1String("unavailable:"))) {
            replacement = completed.value(old.mid(12));
            if (old.startsWith(QLatin1String("unavailable:Softkey "))) {
                replacement = QStringLiteral("softkey") + old.mid(20);
            }
        }
        if (!replacement.isEmpty()) { entry.insert(QStringLiteral("action"), replacement); it.value() = entry; }
    }
    next.insert(QStringLiteral("bindings"), bindings);
    const QStringList fields{QStringLiteral("enabled"), QStringLiteral("panel"), QStringLiteral("serialPort"),
        QStringLiteral("listenAddress"), QStringLiteral("tcpPort"), QStringLiteral("peerAddress"), QStringLiteral("radioId"), QStringLiteral("bindings")};
    for (const QString& key : next.keys()) { if (!fields.contains(key)) { next.remove(key); } }
    bool valid = next.value(QStringLiteral("enabled")).isBool()
        && (panel == QLatin1String("andromeda") || panel == QLatin1String("g2v2"))
        && next.value(QStringLiteral("bindings")).isObject()
        && next.value(QStringLiteral("tcpPort")).isDouble()
        && next.value(QStringLiteral("tcpPort")).toDouble() == port
        && serial.size() <= 128 && port >= 1 && port <= 65535 && !bind.isNull()
        && (peerText.isEmpty() || !peer.isNull()) && bindings.size() <= 238;
    if (enabled && (next.value(QStringLiteral("radioId")).toString().isEmpty()
        || (persist && !m_model.isConnected()))) { valid = false; }
    if (enabled && panel == QLatin1String("andromeda") && serial.isEmpty()) { valid = false; }
    if (enabled && panel == QLatin1String("g2v2") && !bind.isLoopback() && peer.isNull()) { valid = false; }
    for (auto it = bindings.begin(); it != bindings.end() && valid; ++it) {
        const QString key = it.key(); bool ok = false;
        const int number = key.mid(1).toInt(&ok);
        const auto entry = it.value().toObject();
        valid = ok && QString::number(number) == key.mid(1) && number >= 1 && number <= ((key.startsWith(QLatin1Char('e')) || key.startsWith(QLatin1Char('l'))) ? 20 : 99)
            && QStringLiteral("ephl").contains(key.front())
            && actions(bindingKind(key.front())).contains(entry.value(QStringLiteral("action")).toString())
            && entry.value(QStringLiteral("target")).isDouble()
            && entry.value(QStringLiteral("target")).toDouble() == entry.value(QStringLiteral("target")).toInt(-1)
            && entry.value(QStringLiteral("target")).toInt(-1) >= 0 && entry.value(QStringLiteral("target")).toInt(-1) <= 2;
    }
    if (!valid) { m_status = QStringLiteral("Invalid panel settings. A LAN listener requires the panel peer IP."); emit stateChanged(); return false; }
    const RadioSettingsScope scope(QStringLiteral("local"), QStringLiteral("Andromeda"));
    int version = 0; AppSettings::FeatureReadStatus status;
    scope.featureExact(QStringLiteral("FrontPanel"), &version, &status);
    if (version > 1 || status == AppSettings::FeatureReadStatus::Corrupt
        || status == AppSettings::FeatureReadStatus::Unavailable || (persist && !scope.setFeature(QStringLiteral("FrontPanel"), 1, next))) {
        m_status = QStringLiteral("Panel settings could not be saved"); emit stateChanged(); return false;
    }
    if (persist && activePanel && activePanel != this) { activePanel->closeTransport(); }
    closeTransport(); m_config = next; m_bindings = bindings; refresh(); emit stateChanged(); return true;
}
void FrontPanelModel::setLearning(bool on)
{
    m_learning = on;
    m_buffer.clear();
    if (on) {
        m_model.abortTxProducerInputs(m_producer, TransmitModel::PttSource::Footswitch);
        m_producer.discardInputs(); m_mox = {}; m_tune = {}; m_heldPtt.clear(); m_pressed.clear();
    }
}
void FrontPanelModel::closeTransport()
{
    ++m_transportEpoch;
    m_ctun = m_shift = m_selectingMultifunction = false; m_menu = 0;
    m_quickMemory.clear();
    if (activePanel == this) { activePanel = nullptr; }
    m_model.abortTxProducerInputs(m_producer, TransmitModel::PttSource::Footswitch);
    m_producer.discardInputs(); m_mox = {}; m_tune = {}; m_heldPtt.clear(); m_pressed.clear();
    m_identified = m_registered = false; m_buffer.clear(); m_leds.clear(); m_lastControl.clear();
    m_hardwareVersion = m_firmwareVersion = -1;
#ifdef HAVE_SERIALPORT
    if (m_serial.isOpen()) { m_serial.close(); }
#endif
    if (m_peer) { auto* peer = m_peer.data(); m_peer = nullptr; peer->disconnect(this); peer->abort(); peer->deleteLater(); }
    m_server.close();
    emit stateChanged();
}
void FrontPanelModel::refresh()
{
    const auto saved = RadioSettingsScope(QStringLiteral("local"), QStringLiteral("Andromeda"))
        .featureExact(QStringLiteral("FrontPanel"));
    if (!saved.isEmpty() && saved.value(QStringLiteral("radioId")) != m_config.value(QStringLiteral("radioId"))) {
        closeTransport(); m_status = QStringLiteral("Panel is assigned to another radio; Apply rebinds it here"); emit stateChanged(); return;
    }
    if (m_futureVersion || !m_config.value(QStringLiteral("enabled")).toBool()
        || !m_model.isConnected() || !m_model.backendDeclaresExtension(QStringLiteral("anan"))
        || m_config.value(QStringLiteral("radioId")).toString() != m_model.settingsScope().radioId()) {
        closeTransport(); m_status = QStringLiteral("Disabled or no connected extended ANAN selected"); emit stateChanged(); return;
    }
    if (activePanel && activePanel != this) { m_status = QStringLiteral("Panel transport is in use by another radio session"); emit stateChanged(); return; }
    activePanel = this;
    if (m_config.value(QStringLiteral("panel")).toString() == QLatin1String("g2v2")) {
        if (!m_server.isListening()) {
            if (!m_server.listen(QHostAddress(m_config.value(QStringLiteral("listenAddress")).toString()),
                quint16(m_config.value(QStringLiteral("tcpPort")).toInt()))) { m_status = m_server.errorString(); }
            else { m_status = QStringLiteral("Listening for G2-V2 panel"); }
            emit stateChanged();
        }
        return;
    }
#ifdef HAVE_SERIALPORT
    if (m_serial.isOpen()) {
        if (!m_identified) { send("ZZZS;"); }
        else { requestCtun(false); }
        return;
    }
    m_serial.setPortName(m_config.value(QStringLiteral("serialPort")).toString());
    m_serial.setBaudRate(9600); m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity); m_serial.setStopBits(QSerialPort::OneStop); m_serial.setFlowControl(QSerialPort::NoFlowControl);
    m_opening = true;
    const bool opened = m_serial.open(QIODevice::ReadWrite);
    if (opened) {
        if (m_serial.setDataTerminalReady(true) && m_serial.setRequestToSend(true)) {
            m_serial.clear(); m_serial.write("ZZZS;");
            m_status = QStringLiteral("Serial panel open; identifying");
        } else {
            m_status = QStringLiteral("Could not assert panel DTR/RTS: %1").arg(m_serial.errorString());
            closeTransport();
        }
    }
    else { m_status = m_serial.errorString(); }
    m_opening = false;
#else
    m_status = QStringLiteral("This build has no Qt SerialPort support");
#endif
    emit stateChanged();
}
void FrontPanelModel::send(const QByteArray& bytes)
{
    if (m_peer && m_peer->state() == QAbstractSocket::ConnectedState) {
        if (m_peer->bytesToWrite() < 4096) { m_peer->write(bytes); }
        return;
    }
#ifdef HAVE_SERIALPORT
    if (m_serial.isOpen() && m_serial.bytesToWrite() < 4096) { m_serial.write(bytes); }
#endif
}
QJsonObject FrontPanelModel::binding(const QString& key) const { return m_bindings.value(key).toObject(); }
SliceModel* FrontPanelModel::target(int selector) const
{
    if (selector == 1 || selector == 2) { return m_model.slice(selector - 1); }
    for (SliceModel* slice : m_model.slices()) { if (slice->isActive()) { return slice; } }
    return m_model.slice(0);
}
void FrontPanelModel::consume(const QByteArray& bytes)
{
    if (m_buffer.size() + bytes.size() > 4096) { closeTransport(); m_status = QStringLiteral("Panel input exceeded frame limit"); emit stateChanged(); return; }
    m_buffer += bytes;
    int frames = 0;
    while (m_buffer.contains(';') && frames++ < 128) {
        const int end = m_buffer.indexOf(';');
        const QString command = QString::fromLatin1(m_buffer.first(end)).trimmed();
        m_buffer.remove(0, end + 1);
        const auto event = frontpanel::parse(command);
        if (!event) { send("?;"); continue; }
        using Kind = frontpanel::Kind;
        if (event->kind == Kind::Register || event->kind == Kind::Unregister) {
            if (!m_peer) { continue; }
            m_registered = event->kind == Kind::Register;
            send((m_registered ? QByteArray("ZZGA") : QByteArray("ZZGR")) + frontpanel::kG2V2Id.toLatin1() + ';');
            if (m_registered) { send("ZZZS;"); } else { closeTransport(); }
            continue;
        }
        if (event->kind == Kind::Identity) {
            const bool tcp = m_config.value(QStringLiteral("panel")).toString() == QLatin1String("g2v2");
            m_identified = tcp ? m_registered && event->number == 5 : event->number == 1;
            m_hardwareVersion = m_identified ? event->value / 1000 : -1;
            m_firmwareVersion = m_identified ? event->value % 1000 : -1;
            m_status = m_identified ? QStringLiteral("Panel identified: hardware %1, firmware %2")
                .arg(event->value / 1000).arg(event->value % 1000) : QStringLiteral("Panel identity does not match selected type");
            m_leds.clear(); updateIndicators(); emit stateChanged(); continue;
        }
        if (!m_identified || !m_model.isConnected() || !m_model.backendDeclaresExtension(QStringLiteral("anan"))) { continue; }
        m_lastControl = event->kind == Kind::Vfo ? QString()
            : QString(event->kind == Kind::Encoder ? QLatin1Char('e') : event->value == 2 ? QLatin1Char('h') : QLatin1Char('p')) + QString::number(event->number);
        m_lastInput = command.toUpper(); emit stateChanged();
        if (m_learning) { continue; }
        if (event->kind == Kind::Vfo) { turn(QStringLiteral("vfo"), event->value, target(0)); continue; }
        const QString key = m_lastControl;
        const auto entry = binding(key);
        SliceModel* slice = target(entry.value(QStringLiteral("target")).toInt());
        if (event->kind == Kind::Encoder) { turn(entry.value(QStringLiteral("action")).toString(), event->value, slice); }
        else { press(entry.value(QStringLiteral("action")).toString(), event->number, event->value, slice); }
    }
    if (m_buffer.size() > 128) { closeTransport(); m_status = QStringLiteral("Panel sent an unterminated frame"); emit stateChanged(); }
}
void FrontPanelModel::turn(const QString& action, int steps, SliceModel* slice)
{
    if (steps == 0) { return; }
    if (action == QLatin1String("multifunction")) {
        if (m_selectingMultifunction) {
            const QStringList choices{"afGain", "agcT", "rfGain", "filterLow", "filterHigh", "drive", "cwSpeed", "monitorLevel", "vfo", "audioPan", "micGain", "compression", "cwPitch", "displayPan", "displayZoom", "tuneStep", "masterGain"};
            const int index = std::max(0, int(choices.indexOf(m_multifunction)));
            setMultifunctionAction(choices.at((index + steps % choices.size() + choices.size()) % choices.size()));
        } else { turn(m_multifunction, steps, slice); }
        return;
    }
    if (action.startsWith(QLatin1String("unavailable:"))) {
        m_status = action.mid(12) + QStringLiteral(" is not integrated for this panel"); emit stateChanged(); return;
    }
    if (action == QLatin1String("rit") || action == QLatin1String("xit")) {
        m_status = QStringLiteral("RIT/XIT panel dispatch is unavailable for extended ANAN"); emit stateChanged(); return;
    }
    auto& tx = m_model.transmitModel();
    if (action == QLatin1String("micGain")) { tx.setMicLevel(std::clamp(tx.micLevel() + steps, 0, 100)); return; }
    if (action == QLatin1String("compression")) { tx.setSpeechProcessorLevel(std::clamp(tx.speechProcessorLevel() + steps, 0, tx.speechProcessorLevelMaximum())); return; }
    if (action == QLatin1String("cwPitch")) { tx.setCwPitch(std::clamp(tx.cwPitch() + steps * 10, 100, 2000)); return; }
    if (action == QLatin1String("displayPan") || action == QLatin1String("displayZoom") || action == QLatin1String("tuneStep") || action == QLatin1String("masterGain")) {
        emit desktopActionRequested(action, slice ? slice->sliceId() : 0, steps); return;
    }
    if (action == QLatin1String("drive")) { tx.setRfPower(std::clamp(tx.rfPower() + steps, 0, 100)); return; }
    if (action == QLatin1String("cwSpeed")) { tx.setCwSpeed(std::clamp(tx.cwSpeed() + steps, 5, 100)); return; }
    if (action == QLatin1String("monitorLevel")) { tx.setMonGainSb(std::clamp(tx.monGainSb() + steps, 0, 100)); return; }
    if (!slice) { return; }
    if (action == QLatin1String("audioPan")) { slice->setAudioPan(std::clamp(slice->audioPan() + steps, 0, 100)); return; }
    if (action == QLatin1String("vfo") && !slice->isLocked()) { m_model.tuneSliceForCat(slice, slice->frequency() + steps * slice->stepHz() / 1e6); }
    else if (action == QLatin1String("afGain")) { slice->setAudioGain(std::clamp(slice->audioGain() + steps, 0.0f, 100.0f)); }
    else if (action == QLatin1String("agcT")) { slice->setAgcTKnobLevel(std::clamp(slice->agcTKnobLevel() + steps, slice->agcTKnobMinimum(), slice->agcTKnobMaximum())); }
    else if (action == QLatin1String("rfGain")) {
        if (const auto* pan = m_model.panadapter(slice->panId())) {
            m_model.setPanRfGainFor(slice->panId(), std::clamp(pan->rfGain() + steps, -61, 0));
        }
    }
    else if (action == QLatin1String("filterLow")) { const int low = std::clamp(slice->filterLow() + steps * 50, -12000, 12000);
        if (low < slice->filterHigh() - 50) { slice->setFilterWidth(low, slice->filterHigh()); } }
    else if (action == QLatin1String("filterHigh")) { const int high = std::clamp(slice->filterHigh() + steps * 50, -12000, 12000);
        if (high > slice->filterLow() + 50) { slice->setFilterWidth(slice->filterLow(), high); } }
    else if (action == QLatin1String("rit")) { slice->setRit(slice->ritOn(), std::clamp(slice->ritFreq() + steps * 10, -99999, 99999)); }
    else if (action == QLatin1String("xit")) { slice->setXit(slice->xitOn(), std::clamp(slice->xitFreq() + steps * 10, -99999, 99999)); }
}
void FrontPanelModel::press(const QString& action, int number, int state, SliceModel* slice)
{
    const auto source = TransmitModel::PttSource::Footswitch;
    if (state == 0) {
        m_pressed.remove(number);
        const auto held = m_heldPtt.take(number);
        if (held.originalSessionCurrent()) { m_model.requestProducerPttOff(held, source); }
        return;
    }
    if (state == 1 && m_pressed.contains(number)) { return; }
    if (state == 1) { m_pressed.insert(number); }
    if (m_shift && state == 1 && number >= 30 && number <= 41) {
        const QStringList bands{"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "GEN"};
        emit desktopActionRequested(QStringLiteral("band:") + bands.at(number - 30), slice ? slice->sliceId() : 0, 1);
        return;
    }
    if (action.startsWith(QLatin1String("softkey"))) {
        bool valid = false; const int key = action.mid(7).toInt(&valid);
        if (valid && key >= 1 && key <= 8) {
            m_pressed.remove(number);
            press(menuActions(m_menu).at(key - 1), number, state, slice);
        }
        return;
    }
    if (action == QLatin1String("menuNext") || action == QLatin1String("menuFirst") || action == QLatin1String("menuTest")) {
        m_menu = action == QLatin1String("menuFirst") ? 0 : action == QLatin1String("menuTest") ? 7 : (m_menu + 1) % 7;
        emit stateChanged(); return;
    }
    if (action == QLatin1String("shift")) { m_shift = !m_shift; emit stateChanged(); return; }
    if (action == QLatin1String("multifunctionMenu")) { m_selectingMultifunction = !m_selectingMultifunction; emit stateChanged(); return; }
    if (action == QLatin1String("ctun")) { requestCtun(true); return; }
    const QStringList desktopActions{"filterReset", "multifunctionMenu", "bandUp", "bandDown", "filterUp", "filterDown",
        "modeMenu", "filterMenu", "bandMenu", "stepMenu", "gainMenu", "setup", "txSettings", "displayMenu",
        "audioSettings", "equalizer", "centerDisplay", "zoomIn", "zoomOut", "recordAudio", "playAudio", "receiverMenu", "receiverToggle",
        "bandstack", "bandstackMenu", "nr", "anf"};
    if (desktopActions.contains(action)) {
        emit desktopActionRequested(action, slice ? slice->sliceId() : 0, 1); return;
    }
    if (action.startsWith(QLatin1String("unavailable:"))) {
        m_status = action.mid(12) + QStringLiteral(" is not integrated for this panel"); emit stateChanged(); return;
    }
    if (action == QLatin1String("rit") || action == QLatin1String("xit") || action == QLatin1String("clearOffsets")) {
        m_status = QStringLiteral("RIT/XIT panel dispatch is unavailable for extended ANAN"); emit stateChanged(); return;
    }
    if (action == QLatin1String("mox")) {
        if (m_model.producerRequestHasWork(m_mox)) { m_model.requestProducerPttOff(m_mox, source); m_mox = {}; }
        else { const auto input = m_producer.request(); if (m_model.requestProducerPttOn(input, source)) { m_mox = input; } }
        return;
    }
    if (action == QLatin1String("pttHold") && state == 1) {
        const auto input = m_producer.request(); if (m_model.requestProducerPttOn(input, source)) { m_heldPtt.insert(number, input); } return;
    }
    if (action == QLatin1String("tune")) {
        if (m_model.producerRequestHasWork(m_tune)) { m_model.requestProducerTune(m_tune, false); m_tune = {}; }
        else { const auto input = m_producer.request(); if (m_model.requestProducerTune(input, true)) { m_tune = input; } }
        return;
    }
    if (action == QLatin1String("monitor")) { m_model.transmitModel().setSbMonitor(!m_model.transmitModel().sbMonitor()); return; }
    if (!slice) { return; }
    if (action == QLatin1String("quickSave")) {
        m_quickMemory = {{"frequency", slice->frequency()}, {"mode", slice->mode()},
            {"low", slice->filterLow()}, {"high", slice->filterHigh()}};
        m_status = QStringLiteral("Panel quick memory saved for this session"); emit stateChanged(); return;
    }
    if (action == QLatin1String("quickRestore")) {
        if (!m_quickMemory.isEmpty() && !slice->isLocked()) {
            slice->setMode(m_quickMemory.value(QStringLiteral("mode")).toString());
            m_model.tuneSliceForCat(slice, m_quickMemory.value(QStringLiteral("frequency")).toDouble());
            slice->setFilterWidth(m_quickMemory.value(QStringLiteral("low")).toInt(), m_quickMemory.value(QStringLiteral("high")).toInt());
        }
        return;
    }
    if (action == QLatin1String("attenuationStep")) { turn(QStringLiteral("rfGain"), -10, slice); return; }
    if (action == QLatin1String("antennaStep")) {
        const QStringList antennas = slice->rxAntennaList();
        if (!antennas.isEmpty()) { slice->setRxAntenna(antennas.at((antennas.indexOf(slice->rxAntenna()) + 1) % antennas.size())); }
        return;
    }
    if (action == QLatin1String("copyAtoB") || action == QLatin1String("copyBtoA") || action == QLatin1String("swapVfo")) {
        SliceModel* a = m_model.slice(0); SliceModel* b = m_model.slice(1);
        if (!a || !b || a->isLocked() || b->isLocked()) {
            m_status = QStringLiteral("VFO copy/swap requires two unlocked receivers"); emit stateChanged(); return;
        }
        const double first = a->frequency(), second = b->frequency();
        if (action != QLatin1String("copyAtoB")) { m_model.tuneSliceForCat(a, second); }
        if (action != QLatin1String("copyBtoA")) { m_model.tuneSliceForCat(b, first); }
        return;
    }
    if (action == QLatin1String("mute")) { slice->setAudioMute(!slice->audioMute()); }
    else if (action == QLatin1String("lock")) { slice->setLocked(!slice->isLocked()); }
    else if (action == QLatin1String("rit")) { slice->setRit(!slice->ritOn(), slice->ritFreq()); }
    else if (action == QLatin1String("xit")) { slice->setXit(!slice->xitOn(), slice->xitFreq()); }
    else if (action == QLatin1String("clearOffsets")) { slice->setRit(slice->ritOn(), 0); slice->setXit(slice->xitOn(), 0); }
    else if (action == QLatin1String("nb")) { slice->setNb(!slice->nbOn()); }
    else if (action == QLatin1String("nr")) { m_model.requestRadioNoiseReduction(slice, !slice->nrOn()); }
    else if (action == QLatin1String("anf")) { m_model.requestRadioAutoNotch(slice, !slice->anfOn()); }
    else if (action == QLatin1String("receiver")) { if (SliceModel* other = m_model.slice(slice->sliceId() == 0 ? 1 : 0)) { other->setActive(true); } }
    else if (action == QLatin1String("agc")) {
        const QStringList modes{QStringLiteral("off"), QStringLiteral("slow"), QStringLiteral("med"), QStringLiteral("fast")};
        slice->setAgcMode(modes[(modes.indexOf(slice->agcMode().toLower()) + 1) % modes.size()]);
    } else if (action == QLatin1String("modeUp") || action == QLatin1String("modeDown")) {
        const QStringList modes{QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("CWU"), QStringLiteral("CWL"), QStringLiteral("AM"), QStringLiteral("SAM"), QStringLiteral("FM"), QStringLiteral("DIGU"), QStringLiteral("DIGL")};
        const int index = std::max(0, int(modes.indexOf(slice->mode())));
        slice->setMode(modes[(index + (action == QLatin1String("modeUp") ? 1 : modes.size() - 1)) % modes.size()]);
    }
}
void FrontPanelModel::updateIndicators()
{
    if (!m_identified || !m_model.isConnected()) { return; }
    for (int number = 1; number <= 20; ++number) {
        const auto entry = binding(QStringLiteral("l%1").arg(number));
        const QString action = entry.value(QStringLiteral("action")).toString();
        SliceModel* slice = target(entry.value(QStringLiteral("target")).toInt());
        const auto& tx = m_model.transmitModel();
        bool on = action == QLatin1String("mox") ? m_model.isRadioTransmitting()
            : action == QLatin1String("tune") ? tx.isTuning()
            : action == QLatin1String("monitor") ? tx.sbMonitor() : false;
        if (slice) {
            if (action == QLatin1String("mute")) { on = slice->audioMute(); }
            else if (action == QLatin1String("lock")) { on = slice->isLocked(); }
            else if (action == QLatin1String("rit")) { on = slice->ritOn(); }
            else if (action == QLatin1String("xit")) { on = slice->xitOn(); }
            else if (action == QLatin1String("receiver")) { on = slice->sliceId() != 0; }
            else if (action == QLatin1String("nb")) { on = slice->nbOn(); }
            else if (action == QLatin1String("nr")) { on = slice->nrOn(); }
            else if (action == QLatin1String("anf")) { on = slice->anfOn(); }
            else if (action == QLatin1String("ctun")) { on = m_ctun; }
        }
        if (action == QLatin1String("shift")) { on = m_shift; }
        if (!m_leds.contains(number) || m_leds[number] != on) {
            send(QStringLiteral("ZZZI%1%2;").arg(number, 2, 10, QLatin1Char('0')).arg(on ? 1 : 0).toLatin1());
            m_leds.insert(number, on);
        }
    }
}
FrontPanelModel& RadioModel::frontPanel()
{
    if (!m_frontPanel) { m_frontPanel = std::make_unique<FrontPanelModel>(*this); }
    return *m_frontPanel;
}
} // namespace AetherSDR
