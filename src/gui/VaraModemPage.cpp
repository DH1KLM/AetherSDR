#include "VaraModemPage.h"

#include "core/VaraClient.h"
#include "core/VaraSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
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

constexpr const char* kVaraPageStyle = R"(
QWidget#VaraModemPage {
    background: #07101c;
}
QFrame#VaraHeaderFrame,
QFrame#VaraConfigFrame,
QFrame#VaraSessionFrame,
QFrame#VaraStatusFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QLabel#VaraPageTitle,
QLabel#VaraPanelTitle {
    color: #d4deea;
    background: transparent;
    font-size: 16px;
    font-weight: 700;
}
QLabel#VaraFieldLabel,
QLabel#VaraFooterLabel {
    color: #8d99ad;
    background: transparent;
    font-size: 11px;
    font-weight: 700;
}
QLabel#VaraMuted {
    color: #8d99ad;
    background: transparent;
}
QLabel#VaraValue {
    color: #d4deea;
    background: transparent;
}
QLabel#VaraStateDot {
    background: #647187;
    border-radius: 6px;
    min-width: 12px; max-width: 12px;
    min-height: 12px; max-height: 12px;
}
QLabel#VaraWarning {
    color: #f0b429;
    background: transparent;
}
QLabel#VaraError {
    color: #e05252;
    background: transparent;
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
    l->setObjectName(QStringLiteral("VaraFieldLabel"));
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

VaraModemPage::VaraModemPage(RadioModel* radio, QWidget* parent)
    : QWidget(parent)
    , m_radio(radio)
    , m_client(new VaraClient(this))
{
    // Structural, not merely conventional: with the inhibit set, the client
    // refuses CONNECT, CQFRAME and payload writes outright. Wiring a transmit
    // button to this page by mistake would produce a refusal and a log line
    // rather than a signal on the air.
    m_client->setTransmitInhibited(true);

    setObjectName(QStringLiteral("VaraModemPage"));
    setAccessibleName(tr("VARA HF modem"));
    setStyleSheet(QString::fromLatin1(kVaraPageStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    buildHeader();

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("VaraWorkspaceSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildConfigurationPanel());
    splitter->addWidget(buildSessionPanel());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({390, 690});
    root->addWidget(splitter, 1);

    buildFooter();

    connect(m_client, &VaraClient::modemConnected, this, [this] {
        appendEvent(tr("Modem connected."));
        m_client->requestVersion();
        const QString call = VaraSettings::callsign();
        if (!call.isEmpty())
            m_client->setMyCalls({call});
        m_client->setBandwidth(VaraSettings::bandwidth());
        m_client->setCompression(VaraSettings::compression());
        // Receive-only. LISTEN does not transmit; nothing on this page does.
        m_client->listen(true);
        refreshConnectionState();
    });
    connect(m_client, &VaraClient::modemDisconnected, this, [this] {
        appendEvent(tr("Modem disconnected."));
        refreshConnectionState();
    });
    connect(m_client, &VaraClient::modemError, this, [this](const QString& m) {
        setError(m);
        refreshConnectionState();
    });
    connect(m_client, &VaraClient::versionReceived, this, [this](const QString& v) {
        m_modemVersion->setText(v);
        appendEvent(tr("Modem version: %1").arg(v));
    });
    connect(m_client, &VaraClient::linkEstablished, this,
            [this](const QString& src, const QString& dst, int bw) {
                appendEvent(tr("Link established: %1 to %2 at %3 Hz")
                                .arg(src, dst).arg(bw));
                refreshConnectionState();
            });
    connect(m_client, &VaraClient::linkClosed, this, [this] {
        appendEvent(tr("Link closed."));
        refreshConnectionState();
    });
    connect(m_client, &VaraClient::signalToNoiseChanged, this, [this](double db) {
        m_snLabel->setText(QStringLiteral("%1 dB").arg(db, 0, 'f', 1));
    });
    connect(m_client, &VaraClient::bitrateChanged, this,
            [this](int level, int bps, Vara::BitrateDirection dir) {
                const QString d = dir == Vara::BitrateDirection::Transmit
                                      ? tr("TX")
                                      : (dir == Vara::BitrateDirection::Receive ? tr("RX")
                                                                           : QString());
                m_bitrateLabel->setText(
                    d.isEmpty() ? tr("level %1, %2 bps").arg(level).arg(bps)
                                : tr("level %1, %2 bps (%3)").arg(level).arg(bps).arg(d));
            });
    connect(m_client, &VaraClient::bufferChanged, this, [this](int outstanding) {
        m_bufferLabel->setText(QString::number(outstanding));
    });
    connect(m_client, &VaraClient::cqFrameReceived, this,
            [this](const QString& src, int bw) {
                appendEvent(tr("CQ heard from %1 at %2 Hz").arg(src).arg(bw));
            });
    connect(m_client, &VaraClient::busyChanged, this, [this](bool busy) {
        appendEvent(busy ? tr("Channel busy.") : tr("Channel clear."));
    });
    connect(m_client, &VaraClient::missingSoundcard, this, [this] {
        // Worth calling out plainly: the modem reports this when its configured
        // input or output device name is empty, which is indistinguishable from
        // having no audio devices at all and is easy to misdiagnose.
        setError(tr("The modem reports MISSING SOUNDCARD. Check that its audio "
                    "input and output devices are selected in VARA's own setup "
                    "dialog — an empty device name produces this message."));
    });
    connect(m_client, &VaraClient::dataReceived, this, &VaraModemPage::appendPayload);
    connect(m_client, &VaraClient::commandRejected, this, [this](const QString& c) {
        appendEvent(tr("Modem rejected: %1").arg(c));
    });
    connect(m_client, &VaraClient::transmitInhibited, this, [this](const QString& c) {
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

void VaraModemPage::buildHeader()
{
    auto* frame = panel(QStringLiteral("VaraHeaderFrame"), this);
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 12, 16, 12);
    row->setSpacing(12);

    auto* title = new QLabel(tr("VARA HF"), frame);
    title->setObjectName(QStringLiteral("VaraPageTitle"));
    row->addWidget(title);

    m_stateDot = new QLabel(frame);
    m_stateDot->setObjectName(QStringLiteral("VaraStateDot"));
    row->addWidget(m_stateDot);

    m_stateText = new QLabel(tr("Modem not connected"), frame);
    m_stateText->setObjectName(QStringLiteral("VaraMuted"));
    row->addWidget(m_stateText);

    m_modemVersion = new QLabel(QString(), frame);
    m_modemVersion->setObjectName(QStringLiteral("VaraMuted"));
    row->addWidget(m_modemVersion);

    row->addStretch(1);

    m_errorLabel = new QLabel(QString(), frame);
    m_errorLabel->setObjectName(QStringLiteral("VaraError"));
    m_errorLabel->setWordWrap(true);
    row->addWidget(m_errorLabel, 2);

    m_connectButton = new QPushButton(tr("Connect modem"), frame);
    connect(m_connectButton, &QPushButton::clicked,
            this, &VaraModemPage::toggleModemConnection);
    row->addWidget(m_connectButton);

    layout()->addWidget(frame);
}

QWidget* VaraModemPage::buildConfigurationPanel()
{
    auto* frame = panel(QStringLiteral("VaraConfigFrame"), this);
    auto* col = new QVBoxLayout(frame);
    col->setContentsMargins(16, 14, 16, 14);
    col->setSpacing(10);

    auto* title = new QLabel(tr("Modem"), frame);
    title->setObjectName(QStringLiteral("VaraPanelTitle"));
    col->addWidget(title);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    int r = 0;

    grid->addWidget(fieldLabel(tr("Host"), frame), r, 0);
    m_hostEdit = new QLineEdit(frame);
    grid->addWidget(m_hostEdit, r++, 1);

    grid->addWidget(fieldLabel(tr("Command port"), frame), r, 0);
    m_portSpin = new QSpinBox(frame);
    m_portSpin->setRange(VaraSettings::kMinPort, VaraSettings::kMaxPort);
    grid->addWidget(m_portSpin, r++, 1);

    grid->addWidget(fieldLabel(tr("Data port"), frame), r, 0);
    m_dataPortLabel = new QLabel(frame);
    m_dataPortLabel->setObjectName(QStringLiteral("VaraValue"));
    // Not editable: the modem fixes the data port at command port + 1.
    m_dataPortLabel->setToolTip(
        tr("Fixed by the modem at the command port plus one."));
    grid->addWidget(m_dataPortLabel, r++, 1);

    grid->addWidget(fieldLabel(tr("Callsign"), frame), r, 0);
    m_callsignEdit = new QLineEdit(frame);
    m_callsignEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9/\\-]{0,16}")), this));
    grid->addWidget(m_callsignEdit, r++, 1);

    grid->addWidget(fieldLabel(tr("Bandwidth"), frame), r, 0);
    m_bandwidthCombo = new QComboBox(frame);
    m_bandwidthCombo->addItem(tr("500 Hz"), 500);
    m_bandwidthCombo->addItem(tr("2300 Hz"), 2300);
    m_bandwidthCombo->addItem(tr("2750 Hz"), 2750);
    grid->addWidget(m_bandwidthCombo, r++, 1);

    grid->addWidget(fieldLabel(tr("Compression"), frame), r, 0);
    m_compressionCombo = new QComboBox(frame);
    m_compressionCombo->addItem(tr("Off"), 0);
    m_compressionCombo->addItem(tr("Text"), 1);
    m_compressionCombo->addItem(tr("Files"), 2);
    grid->addWidget(m_compressionCombo, r++, 1);

    col->addLayout(grid);

    m_enforceFilterCheck = new QCheckBox(
        tr("Match the receive filter to the bandwidth"), frame);
    m_enforceFilterCheck->setToolTip(
        tr("A filter narrower than the negotiated bandwidth prevents a link "
           "from forming, and the modem reports no error when that happens."));
    col->addWidget(m_enforceFilterCheck);

    m_filterWarning = new QLabel(QString(), frame);
    m_filterWarning->setObjectName(QStringLiteral("VaraWarning"));
    m_filterWarning->setWordWrap(true);
    col->addWidget(m_filterWarning);

    col->addStretch(1);

    for (auto* w : {m_hostEdit, m_callsignEdit}) {
        connect(w, &QLineEdit::editingFinished,
                this, &VaraModemPage::saveConfigurationFromWidgets);
    }
    connect(m_portSpin, &QSpinBox::valueChanged,
            this, &VaraModemPage::saveConfigurationFromWidgets);
    connect(m_bandwidthCombo, &QComboBox::currentIndexChanged,
            this, &VaraModemPage::saveConfigurationFromWidgets);
    connect(m_compressionCombo, &QComboBox::currentIndexChanged,
            this, &VaraModemPage::saveConfigurationFromWidgets);
    connect(m_enforceFilterCheck, &QCheckBox::toggled,
            this, &VaraModemPage::saveConfigurationFromWidgets);

    return frame;
}

QWidget* VaraModemPage::buildSessionPanel()
{
    auto* frame = panel(QStringLiteral("VaraSessionFrame"), this);
    auto* col = new QVBoxLayout(frame);
    col->setContentsMargins(16, 14, 16, 14);
    col->setSpacing(10);

    auto* title = new QLabel(tr("Session"), frame);
    title->setObjectName(QStringLiteral("VaraPanelTitle"));
    col->addWidget(title);

    auto* stats = new QGridLayout;
    stats->setHorizontalSpacing(14);
    stats->setVerticalSpacing(6);
    auto addStat = [&](int c, const QString& name, QLabel** out) {
        stats->addWidget(fieldLabel(name, frame), 0, c);
        *out = new QLabel(QStringLiteral("—"), frame);
        (*out)->setObjectName(QStringLiteral("VaraValue"));
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

void VaraModemPage::buildFooter()
{
    auto* frame = panel(QStringLiteral("VaraStatusFrame"), this);
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 8, 16, 8);
    row->setSpacing(16);

    m_footerState = new QLabel(QString(), frame);
    m_footerState->setObjectName(QStringLiteral("VaraMuted"));
    row->addWidget(m_footerState);

    row->addStretch(1);

    // Stated in the UI, not only in the source: an operator should be able to
    // see that this page cannot transmit without reading the code.
    m_footerTxNote = new QLabel(tr("Receive and monitor only — this page does "
                                   "not transmit."), frame);
    m_footerTxNote->setObjectName(QStringLiteral("VaraMuted"));
    row->addWidget(m_footerTxNote);

    layout()->addWidget(frame);
}

void VaraModemPage::refreshAll()
{
    refreshConfiguration();
    refreshConnectionState();
    refreshFilterWarning();
}

void VaraModemPage::refreshConfiguration()
{
    m_updating = true;
    const QSignalBlocker b1(m_hostEdit), b2(m_portSpin), b3(m_callsignEdit),
        b4(m_bandwidthCombo), b5(m_compressionCombo), b6(m_enforceFilterCheck);

    m_hostEdit->setText(VaraSettings::host());
    m_portSpin->setValue(VaraSettings::commandPort());
    m_dataPortLabel->setText(QString::number(VaraSettings::dataPort()));
    m_callsignEdit->setText(VaraSettings::callsign());

    const int hz = VaraSettings::bandwidthHz();
    const int bwIndex = m_bandwidthCombo->findData(hz);
    m_bandwidthCombo->setCurrentIndex(bwIndex >= 0 ? bwIndex : 1);

    m_compressionCombo->setCurrentIndex(
        static_cast<int>(VaraSettings::compression()));
    m_enforceFilterCheck->setChecked(VaraSettings::enforceFilterWidth());
    m_updating = false;
}

void VaraModemPage::saveConfigurationFromWidgets()
{
    if (m_updating)
        return;
    VaraSettings::setHost(m_hostEdit->text());
    VaraSettings::setCommandPort(m_portSpin->value());
    VaraSettings::setCallsign(m_callsignEdit->text());
    switch (m_bandwidthCombo->currentData().toInt()) {
    case 500:  VaraSettings::setBandwidth(Vara::Bandwidth::Bw500);  break;
    case 2750: VaraSettings::setBandwidth(Vara::Bandwidth::Bw2750); break;
    default:   VaraSettings::setBandwidth(Vara::Bandwidth::Bw2300); break;
    }
    VaraSettings::setCompression(
        static_cast<Vara::Compression>(m_compressionCombo->currentIndex()));
    VaraSettings::setEnforceFilterWidth(m_enforceFilterCheck->isChecked());

    // The settings layer normalises (callsign case, blank host, port range), so
    // read back rather than leaving the widgets showing what was typed.
    refreshConfiguration();
    refreshFilterWarning();
}

void VaraModemPage::refreshConnectionState()
{
    const bool up = m_client && m_client->isModemConnected();
    const bool linked = m_client && m_client->isLinked();

    m_stateDot->setStyleSheet(
        QStringLiteral("background:%1;border-radius:6px;")
            .arg(linked ? QStringLiteral("#3ecf8e")
                        : (up ? QStringLiteral("#f0b429")
                              : QStringLiteral("#647187"))));
    m_stateText->setText(linked ? tr("Linked to %1").arg(m_client->remoteCall())
                                : (up ? tr("Listening") : tr("Modem not connected")));
    m_connectButton->setText(up ? tr("Disconnect modem") : tr("Connect modem"));
    m_linkState->setText(linked ? m_client->remoteCall() : tr("idle"));
    m_footerState->setText(up ? tr("Modem %1:%2")
                                    .arg(VaraSettings::host())
                                    .arg(VaraSettings::commandPort())
                              : tr("Modem interface closed"));
    // The modem's own settings are pushed on connect, so editing them while a
    // session is open would silently disagree with what the modem is using.
    for (QWidget* w : {static_cast<QWidget*>(m_hostEdit),
                       static_cast<QWidget*>(m_portSpin),
                       static_cast<QWidget*>(m_callsignEdit)}) {
        w->setEnabled(!up);
    }
}

void VaraModemPage::refreshFilterWarning()
{
    if (!m_radio) {
        m_filterWarning->clear();
        return;
    }
    SliceModel* slice = m_radio->txSlice();
    if (!slice) {
        const QList<SliceModel*> all = m_radio->slices();
        slice = all.isEmpty() ? nullptr : all.first();
    }
    if (!slice) {
        m_filterWarning->clear();
        return;
    }
    const int widthHz = std::abs(slice->filterHigh() - slice->filterLow());
    const int wantHz = VaraSettings::bandwidthHz();
    if (widthHz >= wantHz) {
        m_filterWarning->clear();
        return;
    }
    m_filterWarning->setText(
        tr("The receive filter is %1 Hz wide but the configured bandwidth is "
           "%2 Hz. A link will not form, and the modem will not report why.")
            .arg(widthHz)
            .arg(wantHz));
}

void VaraModemPage::toggleModemConnection()
{
    setError(QString());
    if (m_client->isModemConnected()) {
        m_client->disconnectFromModem();
        return;
    }
    if (VaraSettings::callsign().isEmpty()) {
        setError(tr("Set a callsign before connecting: the modem needs one "
                    "registered before it will accept a session."));
        return;
    }
    appendEvent(tr("Opening %1:%2…")
                    .arg(VaraSettings::host())
                    .arg(VaraSettings::commandPort()));
    m_client->connectToModem(VaraSettings::host(),
                             static_cast<quint16>(VaraSettings::commandPort()),
                             static_cast<quint16>(VaraSettings::dataPort()));
}

void VaraModemPage::appendEvent(const QString& text)
{
    m_eventList->addItem(QStringLiteral("%1  %2").arg(stamp(), text));
    trimList(m_eventList);
}

void VaraModemPage::appendPayload(const QByteArray& payload)
{
    // Payload is arbitrary bytes. Render printable ASCII as text and anything
    // else as hex, rather than letting control characters into a list widget.
    QString rendered;
    bool printable = true;
    for (char c : payload) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u != '\r' && u != '\n' && u != '\t' && (u < 0x20 || u > 0x7e)) {
            printable = false;
            break;
        }
    }
    rendered = printable ? QString::fromLatin1(payload).simplified()
                         : QString::fromLatin1(payload.toHex(' '));
    m_payloadList->addItem(
        QStringLiteral("%1  (%2 bytes) %3").arg(stamp()).arg(payload.size()).arg(rendered));
    trimList(m_payloadList);
}

void VaraModemPage::setError(const QString& message)
{
    m_errorLabel->setText(message);
    if (!message.isEmpty())
        appendEvent(message);
}

} // namespace AetherSDR
