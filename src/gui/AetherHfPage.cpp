#include "AetherHfPage.h"

#include "core/AetherHfSettings.h"
#include "core/VaraClient.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr const char* kAetherHfPageStyle = R"(
QWidget#AetherHfPage {
    background: #07101c;
}
QFrame#AetherHfHeaderFrame,
QFrame#AetherHfConfigFrame,
QFrame#AetherHfSessionFrame,
QFrame#AetherHfStatusFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QLabel#AetherHfPageTitle,
QLabel#AetherHfPanelTitle {
    color: #d4deea;
    background: transparent;
    font-size: 16px;
    font-weight: 700;
}
QLabel#AetherHfFieldLabel {
    color: #8d99ad;
    background: transparent;
    font-size: 11px;
    font-weight: 700;
}
QLabel#AetherHfMuted {
    color: #8d99ad;
    background: transparent;
}
QLabel#AetherHfValue {
    color: #d4deea;
    background: transparent;
}
QLabel#AetherHfStateDot {
    background: #647187;
    border-radius: 6px;
    min-width: 12px; max-width: 12px;
    min-height: 12px; max-height: 12px;
}
QLabel#AetherHfWarning {
    color: #f0b429;
    background: transparent;
}
QLabel#AetherHfError {
    color: #e05252;
    background: transparent;
}
QGroupBox {
    color: #8d99ad;
    border: 1px solid #26374d;
    border-radius: 5px;
    margin-top: 10px;
    padding-top: 8px;
    font-size: 11px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
}
QLineEdit, QSpinBox, QComboBox {
    background: #0b1626;
    border: 1px solid #26374d;
    border-radius: 4px;
    color: #d4deea;
    padding: 4px 6px;
}
QListWidget {
    background: #060f1a;
    border: 1px solid #1d2b3d;
    border-radius: 4px;
    color: #c3cedd;
}
QPushButton {
    background: #17293f;
    border: 1px solid #2c4159;
    border-radius: 4px;
    color: #d4deea;
    padding: 5px 14px;
}
QPushButton:hover { background: #1e3450; }
QPushButton:disabled { color: #5b6779; background: #101c2b; }
)";

// Keep the logs bounded: this page can be left open for hours on a busy
// frequency and neither list is a record of anything, only a live view.
constexpr int kMaxLogRows = 500;

QFrame* panel(const QString& objectName, QWidget* parent)
{
    auto* f = new QFrame(parent);
    f->setObjectName(objectName);
    f->setFrameShape(QFrame::NoFrame);
    return f;
}

QLabel* fieldLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text.toUpper(), parent);
    l->setObjectName(QStringLiteral("AetherHfFieldLabel"));
    return l;
}

void trimList(QListWidget* list)
{
    while (list->count() > kMaxLogRows)
        delete list->takeItem(0);
    list->scrollToBottom();
}

QString stamp()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

} // namespace

AetherHfPage::AetherHfPage(RadioModel* radio, QWidget* parent)
    : QWidget(parent)
    , m_radio(radio)
    , m_vara(new VaraClient(this))
{
    // Structural, not merely conventional: with the inhibit set, the client
    // refuses CONNECT, CQFRAME and payload writes outright. Wiring a transmit
    // control to this page by mistake would produce a refusal and a log line
    // rather than a signal on the air.
    m_vara->setTransmitInhibited(true);

    setObjectName(QStringLiteral("AetherHfPage"));
    setAccessibleName(tr("AetherHF"));
    setStyleSheet(QString::fromLatin1(kAetherHfPageStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    buildHeader();

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("AetherHfWorkspaceSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildConfigurationPanel());
    splitter->addWidget(buildSessionPanel());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({390, 690});
    root->addWidget(splitter, 1);

    buildFooter();

    connect(m_vara, &VaraClient::modemConnected, this, [this] {
        appendEvent(tr("Engine connected."));
        m_vara->requestVersion();
        const QString call = AetherHfSettings::callsign();
        if (!call.isEmpty())
            m_vara->setMyCalls({call});
        m_vara->setBandwidth(AetherHfSettings::varaBandwidth());
        m_vara->setCompression(AetherHfSettings::varaCompression());
        // Receive-only. LISTEN does not transmit; nothing on this page does.
        m_vara->listen(true);
        refreshConnectionState();
    });
    connect(m_vara, &VaraClient::modemDisconnected, this, [this] {
        appendEvent(tr("Engine disconnected."));
        refreshConnectionState();
    });
    connect(m_vara, &VaraClient::modemError, this, [this](const QString& m) {
        setError(m);
        refreshConnectionState();
    });
    connect(m_vara, &VaraClient::versionReceived, this, [this](const QString& v) {
        m_engineVersion->setText(v);
        appendEvent(tr("Engine version: %1").arg(v));
    });
    connect(m_vara, &VaraClient::linkEstablished, this,
            [this](const QString& src, const QString& dst, int bw) {
                appendEvent(tr("Link established: %1 to %2 at %3 Hz")
                                .arg(src, dst).arg(bw));
                refreshConnectionState();
            });
    connect(m_vara, &VaraClient::linkClosed, this, [this] {
        appendEvent(tr("Link closed."));
        refreshConnectionState();
    });
    connect(m_vara, &VaraClient::signalToNoiseChanged, this, [this](double db) {
        m_snLabel->setText(QStringLiteral("%1 dB").arg(db, 0, 'f', 1));
    });
    connect(m_vara, &VaraClient::bitrateChanged, this,
            [this](int level, int bps, Vara::BitrateDirection dir) {
                const QString d =
                    dir == Vara::BitrateDirection::Transmit
                        ? tr("TX")
                        : (dir == Vara::BitrateDirection::Receive ? tr("RX")
                                                                  : QString());
                m_bitrateLabel->setText(
                    d.isEmpty()
                        ? tr("level %1, %2 bps").arg(level).arg(bps)
                        : tr("level %1, %2 bps (%3)").arg(level).arg(bps).arg(d));
            });
    connect(m_vara, &VaraClient::bufferChanged, this, [this](int outstanding) {
        m_bufferLabel->setText(QString::number(outstanding));
    });
    connect(m_vara, &VaraClient::cqFrameReceived, this,
            [this](const QString& src, int bw) {
                appendEvent(tr("CQ heard from %1 at %2 Hz").arg(src).arg(bw));
            });
    connect(m_vara, &VaraClient::busyChanged, this, [this](bool busy) {
        appendEvent(busy ? tr("Channel busy.") : tr("Channel clear."));
    });
    connect(m_vara, &VaraClient::missingSoundcard, this, [this] {
        // Worth calling out plainly: the modem reports this when its configured
        // input or output device name is empty, which is indistinguishable from
        // having no audio devices at all and is easy to misdiagnose.
        setError(tr("The engine reports MISSING SOUNDCARD. Check that its audio "
                    "input and output devices are selected in the modem's own "
                    "setup dialog — an empty device name produces this message."));
    });
    connect(m_vara, &VaraClient::dataReceived, this, &AetherHfPage::appendPayload);
    connect(m_vara, &VaraClient::commandRejected, this, [this](const QString& c) {
        appendEvent(tr("Engine rejected: %1").arg(c));
    });
    connect(m_vara, &VaraClient::transmitInhibited, this, [this](const QString& c) {
        setError(tr("%1 was refused: this page is receive-only and cannot "
                    "transmit.").arg(c));
    });

    if (m_radio) {
        connect(m_radio, &RadioModel::sliceAdded, this,
                [this](SliceModel*) { refreshFilterWarning(); });
        connect(m_radio, &RadioModel::sliceRemoved, this,
                [this](int) { refreshFilterWarning(); });
    }

    refreshAll();
}

void AetherHfPage::buildHeader()
{
    auto* frame = panel(QStringLiteral("AetherHfHeaderFrame"), this);
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 12, 16, 12);
    row->setSpacing(12);

    auto* title = new QLabel(tr("AetherHF"), frame);
    title->setObjectName(QStringLiteral("AetherHfPageTitle"));
    row->addWidget(title);

    row->addWidget(fieldLabel(tr("Engine"), frame));
    m_engineCombo = new QComboBox(frame);
    m_engineCombo->addItem(hfEngineDisplayName(HfEngine::Vara),
                           static_cast<int>(HfEngine::Vara));
    m_engineCombo->addItem(hfEngineDisplayName(HfEngine::Mercury),
                           static_cast<int>(HfEngine::Mercury));
    connect(m_engineCombo, &QComboBox::currentIndexChanged,
            this, &AetherHfPage::engineSelectionChanged);
    row->addWidget(m_engineCombo);

    m_stateDot = new QLabel(frame);
    m_stateDot->setObjectName(QStringLiteral("AetherHfStateDot"));
    row->addWidget(m_stateDot);

    m_stateText = new QLabel(tr("Not connected"), frame);
    m_stateText->setObjectName(QStringLiteral("AetherHfMuted"));
    row->addWidget(m_stateText);

    m_engineVersion = new QLabel(QString(), frame);
    m_engineVersion->setObjectName(QStringLiteral("AetherHfMuted"));
    row->addWidget(m_engineVersion);

    row->addStretch(1);

    m_errorLabel = new QLabel(QString(), frame);
    m_errorLabel->setObjectName(QStringLiteral("AetherHfError"));
    m_errorLabel->setWordWrap(true);
    row->addWidget(m_errorLabel, 2);

    m_connectButton = new QPushButton(tr("Connect"), frame);
    connect(m_connectButton, &QPushButton::clicked,
            this, &AetherHfPage::toggleEngineConnection);
    row->addWidget(m_connectButton);

    layout()->addWidget(frame);
}

QWidget* AetherHfPage::buildConfigurationPanel()
{
    auto* frame = panel(QStringLiteral("AetherHfConfigFrame"), this);
    auto* col = new QVBoxLayout(frame);
    col->setContentsMargins(16, 14, 16, 14);
    col->setSpacing(10);

    auto* title = new QLabel(tr("Station"), frame);
    title->setObjectName(QStringLiteral("AetherHfPanelTitle"));
    col->addWidget(title);

    auto* shared = new QGridLayout;
    shared->setHorizontalSpacing(10);
    shared->setVerticalSpacing(8);
    shared->addWidget(fieldLabel(tr("Callsign"), frame), 0, 0);
    m_callsignEdit = new QLineEdit(frame);
    m_callsignEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9/\\-]{0,16}")), this));
    // Shared across engines: it identifies the station, not the modem.
    m_callsignEdit->setToolTip(tr("Used by whichever engine is selected."));
    shared->addWidget(m_callsignEdit, 0, 1);
    col->addLayout(shared);

    // ── VARA ────────────────────────────────────────────────────────────
    m_varaGroup = new QGroupBox(tr("VARA"), frame);
    auto* grid = new QGridLayout(m_varaGroup);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    int r = 0;

    grid->addWidget(fieldLabel(tr("Host"), m_varaGroup), r, 0);
    m_hostEdit = new QLineEdit(m_varaGroup);
    grid->addWidget(m_hostEdit, r++, 1);

    grid->addWidget(fieldLabel(tr("Command port"), m_varaGroup), r, 0);
    m_portSpin = new QSpinBox(m_varaGroup);
    m_portSpin->setRange(AetherHfSettings::kMinPort, AetherHfSettings::kMaxPort);
    grid->addWidget(m_portSpin, r++, 1);

    grid->addWidget(fieldLabel(tr("Data port"), m_varaGroup), r, 0);
    m_dataPortLabel = new QLabel(m_varaGroup);
    m_dataPortLabel->setObjectName(QStringLiteral("AetherHfValue"));
    // Not editable: the modem fixes the data port at command port + 1.
    m_dataPortLabel->setToolTip(
        tr("Fixed by the modem at the command port plus one."));
    grid->addWidget(m_dataPortLabel, r++, 1);

    grid->addWidget(fieldLabel(tr("Bandwidth"), m_varaGroup), r, 0);
    m_bandwidthCombo = new QComboBox(m_varaGroup);
    m_bandwidthCombo->addItem(tr("500 Hz"), 500);
    m_bandwidthCombo->addItem(tr("2300 Hz"), 2300);
    m_bandwidthCombo->addItem(tr("2750 Hz"), 2750);
    grid->addWidget(m_bandwidthCombo, r++, 1);

    grid->addWidget(fieldLabel(tr("Compression"), m_varaGroup), r, 0);
    m_compressionCombo = new QComboBox(m_varaGroup);
    m_compressionCombo->addItem(tr("Off"), 0);
    m_compressionCombo->addItem(tr("Text"), 1);
    m_compressionCombo->addItem(tr("Files"), 2);
    grid->addWidget(m_compressionCombo, r++, 1);

    col->addWidget(m_varaGroup);

    // ── Mercury ─────────────────────────────────────────────────────────
    m_mercuryGroup = new QGroupBox(tr("Mercury"), frame);
    auto* mcol = new QVBoxLayout(m_mercuryGroup);
    m_mercuryNotice = new QLabel(m_mercuryGroup);
    m_mercuryNotice->setObjectName(QStringLiteral("AetherHfWarning"));
    m_mercuryNotice->setWordWrap(true);
    mcol->addWidget(m_mercuryNotice);
    col->addWidget(m_mercuryGroup);

    m_enforceFilterCheck =
        new QCheckBox(tr("Match the receive filter to the bandwidth"), frame);
    m_enforceFilterCheck->setToolTip(
        tr("A filter narrower than the negotiated bandwidth prevents a link "
           "from forming, and nothing reports an error when that happens."));
    col->addWidget(m_enforceFilterCheck);

    m_filterWarning = new QLabel(QString(), frame);
    m_filterWarning->setObjectName(QStringLiteral("AetherHfWarning"));
    m_filterWarning->setWordWrap(true);
    col->addWidget(m_filterWarning);

    col->addStretch(1);

    for (auto* w : {m_hostEdit, m_callsignEdit}) {
        connect(w, &QLineEdit::editingFinished,
                this, &AetherHfPage::saveConfigurationFromWidgets);
    }
    connect(m_portSpin, &QSpinBox::valueChanged,
            this, &AetherHfPage::saveConfigurationFromWidgets);
    connect(m_bandwidthCombo, &QComboBox::currentIndexChanged,
            this, &AetherHfPage::saveConfigurationFromWidgets);
    connect(m_compressionCombo, &QComboBox::currentIndexChanged,
            this, &AetherHfPage::saveConfigurationFromWidgets);
    connect(m_enforceFilterCheck, &QCheckBox::toggled,
            this, &AetherHfPage::saveConfigurationFromWidgets);

    return frame;
}

QWidget* AetherHfPage::buildSessionPanel()
{
    auto* frame = panel(QStringLiteral("AetherHfSessionFrame"), this);
    auto* col = new QVBoxLayout(frame);
    col->setContentsMargins(16, 14, 16, 14);
    col->setSpacing(10);

    auto* title = new QLabel(tr("Session"), frame);
    title->setObjectName(QStringLiteral("AetherHfPanelTitle"));
    col->addWidget(title);

    auto* stats = new QGridLayout;
    stats->setHorizontalSpacing(14);
    stats->setVerticalSpacing(6);
    auto addStat = [&](int c, const QString& name, QLabel** out) {
        stats->addWidget(fieldLabel(name, frame), 0, c);
        *out = new QLabel(QStringLiteral("—"), frame);
        (*out)->setObjectName(QStringLiteral("AetherHfValue"));
        stats->addWidget(*out, 1, c);
    };
    addStat(0, tr("Link"), &m_linkState);
    addStat(1, tr("S/N"), &m_snLabel);
    addStat(2, tr("Bitrate"), &m_bitrateLabel);
    addStat(3, tr("TX buffer"), &m_bufferLabel);
    stats->setColumnStretch(4, 1);
    col->addLayout(stats);

    col->addWidget(fieldLabel(tr("Events"), frame));
    m_eventList = new QListWidget(frame);
    col->addWidget(m_eventList, 1);

    col->addWidget(fieldLabel(tr("Received payload"), frame));
    m_payloadList = new QListWidget(frame);
    col->addWidget(m_payloadList, 1);

    return frame;
}

void AetherHfPage::buildFooter()
{
    auto* frame = panel(QStringLiteral("AetherHfStatusFrame"), this);
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 8, 16, 8);
    row->setSpacing(16);

    m_footerState = new QLabel(QString(), frame);
    m_footerState->setObjectName(QStringLiteral("AetherHfMuted"));
    row->addWidget(m_footerState);

    row->addStretch(1);

    // Stated in the UI, not only in the source: an operator should be able to
    // see that this page cannot transmit without reading the code.
    m_footerTxNote = new QLabel(
        tr("Receive and monitor only — this page does not transmit."), frame);
    m_footerTxNote->setObjectName(QStringLiteral("AetherHfMuted"));
    row->addWidget(m_footerTxNote);

    layout()->addWidget(frame);
}

void AetherHfPage::refreshAll()
{
    refreshEngine();
    refreshConfiguration();
    refreshConnectionState();
    refreshFilterWarning();
}

void AetherHfPage::refreshEngine()
{
    const HfEngine e = AetherHfSettings::engine();
    const QSignalBlocker b(m_engineCombo);
    const int idx = m_engineCombo->findData(static_cast<int>(e));
    m_engineCombo->setCurrentIndex(idx >= 0 ? idx : 0);

    const bool vara = e == HfEngine::Vara;
    m_varaGroup->setVisible(vara);
    m_mercuryGroup->setVisible(!vara);
    if (!vara) {
        m_mercuryNotice->setText(
            AetherHfSettings::mercuryAvailable()
                ? tr("MercuryV2 selected.")
                : tr("MercuryV2 is not integrated in this build yet, so this "
                     "engine cannot be started. The selection is remembered."));
    }
}

void AetherHfPage::engineSelectionChanged()
{
    if (m_updating)
        return;
    const auto e = static_cast<HfEngine>(m_engineCombo->currentData().toInt());
    // Changing engine while a session is open would leave the old one running
    // with nothing showing its state, so close it first.
    if (m_vara->isModemConnected() && e != HfEngine::Vara) {
        appendEvent(tr("Closing the VARA session before switching engine."));
        m_vara->disconnectFromModem();
    }
    AetherHfSettings::setEngine(e);
    appendEvent(tr("Engine set to %1.").arg(hfEngineDisplayName(e)));
    refreshEngine();
    refreshConnectionState();
    refreshFilterWarning();
}

void AetherHfPage::refreshConfiguration()
{
    m_updating = true;
    const QSignalBlocker b1(m_hostEdit), b2(m_portSpin), b3(m_callsignEdit),
        b4(m_bandwidthCombo), b5(m_compressionCombo), b6(m_enforceFilterCheck);

    m_callsignEdit->setText(AetherHfSettings::callsign());
    m_hostEdit->setText(AetherHfSettings::varaHost());
    m_portSpin->setValue(AetherHfSettings::varaCommandPort());
    m_dataPortLabel->setText(QString::number(AetherHfSettings::varaDataPort()));

    const int hz = AetherHfSettings::varaBandwidthHz();
    const int bwIndex = m_bandwidthCombo->findData(hz);
    m_bandwidthCombo->setCurrentIndex(bwIndex >= 0 ? bwIndex : 1);

    m_compressionCombo->setCurrentIndex(
        static_cast<int>(AetherHfSettings::varaCompression()));
    m_enforceFilterCheck->setChecked(AetherHfSettings::enforceFilterWidth());
    m_updating = false;
}

void AetherHfPage::saveConfigurationFromWidgets()
{
    if (m_updating)
        return;
    AetherHfSettings::setCallsign(m_callsignEdit->text());
    AetherHfSettings::setVaraHost(m_hostEdit->text());
    AetherHfSettings::setVaraCommandPort(m_portSpin->value());
    switch (m_bandwidthCombo->currentData().toInt()) {
    case 500:  AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Bw500);  break;
    case 2750: AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Bw2750); break;
    default:   AetherHfSettings::setVaraBandwidth(Vara::Bandwidth::Bw2300); break;
    }
    AetherHfSettings::setVaraCompression(
        static_cast<Vara::Compression>(m_compressionCombo->currentIndex()));
    AetherHfSettings::setEnforceFilterWidth(m_enforceFilterCheck->isChecked());

    // The settings layer normalises (callsign case, blank host, port range), so
    // read back rather than leaving the widgets showing what was typed.
    refreshConfiguration();
    refreshFilterWarning();
}

void AetherHfPage::refreshConnectionState()
{
    const HfEngine e = AetherHfSettings::engine();
    const bool vara = e == HfEngine::Vara;
    const bool up = vara && m_vara->isModemConnected();
    const bool linked = vara && m_vara->isLinked();
    const bool startable = vara || AetherHfSettings::mercuryAvailable();

    m_stateDot->setStyleSheet(
        QStringLiteral("background:%1;border-radius:6px;")
            .arg(linked ? QStringLiteral("#3ecf8e")
                        : (up ? QStringLiteral("#f0b429")
                              : QStringLiteral("#647187"))));
    m_stateText->setText(
        linked ? tr("Linked to %1").arg(m_vara->remoteCall())
               : (up ? tr("Listening")
                     : (startable ? tr("Not connected")
                                  : tr("%1 unavailable")
                                        .arg(hfEngineDisplayName(e)))));
    m_connectButton->setText(up ? tr("Disconnect") : tr("Connect"));
    m_connectButton->setEnabled(startable);
    m_linkState->setText(linked ? m_vara->remoteCall() : tr("idle"));
    m_footerState->setText(
        up ? tr("%1 at %2:%3")
                 .arg(hfEngineDisplayName(e), AetherHfSettings::varaHost())
                 .arg(AetherHfSettings::varaCommandPort())
           : tr("%1 selected").arg(hfEngineDisplayName(e)));

    // The engine's own settings are pushed on connect, so editing them while a
    // session is open would silently disagree with what it is using.
    for (QWidget* w : {static_cast<QWidget*>(m_hostEdit),
                       static_cast<QWidget*>(m_portSpin),
                       static_cast<QWidget*>(m_callsignEdit),
                       static_cast<QWidget*>(m_engineCombo)}) {
        w->setEnabled(!up);
    }
}

void AetherHfPage::refreshFilterWarning()
{
    m_filterWarning->clear();
    if (!m_radio)
        return;
    const int wantHz = AetherHfSettings::activeBandwidthHz();
    if (wantHz <= 0)
        return;   // the selected engine cannot report a bandwidth

    SliceModel* slice = m_radio->txSlice();
    if (!slice) {
        const QList<SliceModel*> all = m_radio->slices();
        slice = all.isEmpty() ? nullptr : all.first();
    }
    if (!slice)
        return;

    const int widthHz = std::abs(slice->filterHigh() - slice->filterLow());
    if (widthHz >= wantHz)
        return;
    m_filterWarning->setText(
        tr("The receive filter is %1 Hz wide but the configured bandwidth is "
           "%2 Hz. A link will not form, and nothing will report why.")
            .arg(widthHz)
            .arg(wantHz));
}

void AetherHfPage::toggleEngineConnection()
{
    setError(QString());
    const HfEngine e = AetherHfSettings::engine();
    if (e != HfEngine::Vara) {
        if (!AetherHfSettings::mercuryAvailable()) {
            setError(tr("%1 is not integrated in this build yet.")
                         .arg(hfEngineDisplayName(e)));
            return;
        }
        return;   // a Mercury session will start here once it is integrated
    }
    if (m_vara->isModemConnected()) {
        m_vara->disconnectFromModem();
        return;
    }
    if (AetherHfSettings::callsign().isEmpty()) {
        setError(tr("Set a callsign before connecting: the engine needs one "
                    "registered before it will accept a session."));
        return;
    }
    appendEvent(tr("Opening %1:%2…")
                    .arg(AetherHfSettings::varaHost())
                    .arg(AetherHfSettings::varaCommandPort()));
    m_vara->connectToModem(
        AetherHfSettings::varaHost(),
        static_cast<quint16>(AetherHfSettings::varaCommandPort()),
        static_cast<quint16>(AetherHfSettings::varaDataPort()));
}

void AetherHfPage::appendEvent(const QString& text)
{
    m_eventList->addItem(QStringLiteral("%1  %2").arg(stamp(), text));
    trimList(m_eventList);
}

void AetherHfPage::appendPayload(const QByteArray& payload)
{
    // Payload is arbitrary bytes. Render printable ASCII as text and anything
    // else as hex, rather than letting control characters into a list widget.
    bool printable = true;
    for (char c : payload) {
        const auto u = static_cast<unsigned char>(c);
        if (u != '\r' && u != '\n' && u != '\t' && (u < 0x20 || u > 0x7e)) {
            printable = false;
            break;
        }
    }
    const QString rendered = printable
                                 ? QString::fromLatin1(payload).simplified()
                                 : QString::fromLatin1(payload.toHex(' '));
    m_payloadList->addItem(QStringLiteral("%1  (%2 bytes) %3")
                               .arg(stamp())
                               .arg(payload.size())
                               .arg(rendered));
    trimList(m_payloadList);
}

void AetherHfPage::setError(const QString& message)
{
    m_errorLabel->setText(message);
    if (!message.isEmpty())
        appendEvent(message);
}

} // namespace AetherSDR
