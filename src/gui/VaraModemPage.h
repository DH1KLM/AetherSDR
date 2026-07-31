#pragma once

#include "core/VaraProtocol.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QCheckBox;

namespace AetherSDR {

class RadioModel;
class VaraClient;

// The VARA HF page inside AetherModem.
//
// RECEIVE AND MONITOR ONLY, DELIBERATELY. This page can open the modem's TCP
// host interface, register a callsign, set bandwidth and compression, put the
// modem in LISTEN, and display what arrives. It offers **no** control that keys
// a transmitter: no CONNECT, no CQ, no TUNE, no payload send. Those exist on
// VaraClient and are documented there as transmitting; wiring them up is a
// separate change that needs review against Constitution Principle VI (never
// transmit without operator intent), and doing it in the same step as the first
// UI would mean shipping a transmit path before anyone had looked at it.
//
// Opening a TCP socket to a local modem is not transmitting, and LISTEN is
// receive-only, so nothing here can put a signal on the air.
//
// BANDWIDTH AND THE RECEIVE FILTER. A VARA link only forms when the receive
// filter is at least as wide as the negotiated bandwidth, and a mismatch
// produces no error of any kind — the link simply never completes. The page
// therefore shows the slice's current filter width alongside the configured
// bandwidth and warns when they disagree, rather than leaving an operator to
// discover it as an unexplained failure to connect.
class VaraModemPage : public QWidget
{
    Q_OBJECT

public:
    explicit VaraModemPage(RadioModel* radio, QWidget* parent = nullptr);

private:
    QWidget* buildConfigurationPanel();
    QWidget* buildSessionPanel();
    void buildHeader();
    void buildFooter();

    void refreshAll();
    void refreshConnectionState();
    void refreshConfiguration();
    void refreshFilterWarning();
    void saveConfigurationFromWidgets();
    void toggleModemConnection();
    void appendEvent(const QString& text);
    void appendPayload(const QByteArray& payload);
    void setError(const QString& message);

    RadioModel* m_radio{nullptr};
    VaraClient* m_client{nullptr};
    bool m_updating{false};

    QLabel* m_stateDot{nullptr};
    QLabel* m_stateText{nullptr};
    QLabel* m_modemVersion{nullptr};
    QPushButton* m_connectButton{nullptr};
    QLabel* m_errorLabel{nullptr};

    QLineEdit* m_hostEdit{nullptr};
    QSpinBox* m_portSpin{nullptr};
    QLabel* m_dataPortLabel{nullptr};
    QLineEdit* m_callsignEdit{nullptr};
    QComboBox* m_bandwidthCombo{nullptr};
    QComboBox* m_compressionCombo{nullptr};
    QCheckBox* m_enforceFilterCheck{nullptr};
    QLabel* m_filterWarning{nullptr};

    QLabel* m_linkState{nullptr};
    QLabel* m_snLabel{nullptr};
    QLabel* m_bitrateLabel{nullptr};
    QLabel* m_bufferLabel{nullptr};
    QListWidget* m_eventList{nullptr};
    QListWidget* m_payloadList{nullptr};

    QLabel* m_footerState{nullptr};
    QLabel* m_footerTxNote{nullptr};
};

} // namespace AetherSDR
