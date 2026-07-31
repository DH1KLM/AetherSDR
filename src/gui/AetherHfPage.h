#pragma once

#include "core/AetherHfSettings.h"

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QCheckBox;

namespace AetherSDR {

class RadioModel;
class VaraClient;
class MercuryProcess;

// AetherHF — the HF data page inside AetherModem.
//
// The page is named for what it does, not for whose modem it drives. The engine
// is chosen inside it: VARA today, MercuryV2 when that is integrated. Nothing
// about the engine appears in the tab strip, so adding one does not add a tab.
//
// RECEIVE AND MONITOR ONLY, DELIBERATELY. The page can open an engine's control
// interface, register a callsign, configure bandwidth and compression, put the
// engine in listen, and display what arrives. It offers no control that keys a
// transmitter.
//
// This is enforced, not merely observed: VaraClient carries a transmit inhibit
// that refuses connectTo, sendCq and payload writes outright, and the page sets
// it at construction. Wiring a transmit control here by mistake produces a
// refusal and a log line rather than a signal on the air. Lifting the inhibit
// is a separate change that needs review against Constitution Principle VI —
// never transmit without operator intent.
//
// BANDWIDTH AND THE RECEIVE FILTER. A link only forms when the receive filter is
// at least as wide as the negotiated bandwidth, and a mismatch produces no error
// of any kind — the link simply never completes. The page compares the two and
// warns, rather than leaving an operator to discover it as an unexplained
// failure to connect.
class AetherHfPage : public QWidget
{
    Q_OBJECT

public:
    explicit AetherHfPage(RadioModel* radio, QWidget* parent = nullptr);

private:
    QWidget* buildConfigurationPanel();
    QWidget* buildSessionPanel();
    void buildHeader();
    void buildFooter();

    void refreshAll();
    void refreshEngine();
    void refreshConnectionState();
    void refreshConfiguration();
    void refreshFilterWarning();
    void saveConfigurationFromWidgets();
    void engineSelectionChanged();
    void toggleEngineConnection();
    void connectToSelectedEngine();
    void appendEvent(const QString& text);
    void appendPayload(const QByteArray& payload);
    void setError(const QString& message);

    RadioModel* m_radio{nullptr};
    VaraClient* m_vara{nullptr};
    // Mercury speaks the same protocol on the same port layout, so the same
    // client drives it; only the process lifecycle is separate.
    bool m_updating{false};

    QComboBox* m_engineCombo{nullptr};
    QLabel* m_stateDot{nullptr};
    QLabel* m_stateText{nullptr};
    QLabel* m_engineVersion{nullptr};
    QPushButton* m_connectButton{nullptr};
    QLabel* m_errorLabel{nullptr};

    QLineEdit* m_callsignEdit{nullptr};
    QCheckBox* m_enforceFilterCheck{nullptr};
    QLabel* m_filterWarning{nullptr};

    QGroupBox* m_varaGroup{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QSpinBox* m_portSpin{nullptr};
    QLabel* m_dataPortLabel{nullptr};
    QComboBox* m_bandwidthCombo{nullptr};
    QComboBox* m_compressionCombo{nullptr};

    QGroupBox* m_mercuryGroup{nullptr};
    QLabel* m_mercuryNotice{nullptr};
    QLineEdit* m_mercuryHostEdit{nullptr};
    QSpinBox* m_mercuryPortSpin{nullptr};
    QLabel* m_mercuryDataPortLabel{nullptr};
    QComboBox* m_mercuryBandwidthCombo{nullptr};
    QCheckBox* m_mercuryLaunchCheck{nullptr};
    QLineEdit* m_mercuryBinaryEdit{nullptr};
    QLineEdit* m_mercurySoundSystemEdit{nullptr};
    QLineEdit* m_mercuryCaptureEdit{nullptr};
    QLineEdit* m_mercuryPlaybackEdit{nullptr};
    QCheckBox* m_mercuryPublicCheck{nullptr};
    MercuryProcess* m_mercury{nullptr};

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
