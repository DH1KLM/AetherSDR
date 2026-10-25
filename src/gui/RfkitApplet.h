#pragma once

#include "core/RfkitProtocol.h"

#include <QPushButton>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

//DH1KLM Dedicated applet for an RF-KIT amplifier — a sibling of AmpApplet (PGXL),
//DH1KLM AcomApplet, SpeApplet, VkampApplet and Kpa1500Applet, not a variant of any
//DH1KLM of them. See docs/architecture/rfkit-amplifier-design.md.
//DH1KLM
//DH1KLM The device's REST API exposes the tuner read-only (there is no tune
//DH1KLM command), so the tuner appears here as a status line, not as controls. What
//DH1KLM the API does let the operator change — operate/standby, the active antenna,
//DH1KLM and clearing an error — are the controls below. The operational interface
//DH1KLM (UNIV/CAT/UDP/TCI) is shown but deliberately not switchable from here:
//DH1KLM changing it can silently cut the amplifier off from the radio that is
//DH1KLM driving it.
class RfkitApplet : public QWidget {
    Q_OBJECT

public:
    explicit RfkitApplet(QWidget* parent = nullptr);

    //DH1KLM One call per status snapshot — the applet reads the fields it shows.
    //DH1KLM Unset optionals render as "—", never as 0: the device answers each
    //DH1KLM endpoint independently, so "not reported yet" and "reported as zero" are
    //DH1KLM genuinely different states here.
    void setStatus(const Rfkit::Status& status);
    void setConnected(bool connected);

signals:
    void operateToggled(bool operate);
    void antennaSelected(const AetherSDR::Rfkit::Antenna& antenna);
    void errorResetRequested();

private:
    void updateValueLabels();
    void refreshControls();
    void refreshAntennas();
    QString extIndicatorDescription() const;
    //DH1KLM Colour the reflected-power bar by SWR: orange past 1.5:1, red past 2:1.
    void updateReflectedZones(float forwardWatts);

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_refGauge{nullptr};
    HGauge* m_swrGauge{nullptr};
    //DH1KLM Reflected-power watts at which the REF bar turns orange / red (-1 = unset).
    float m_refYellowW{-1.0f};
    float m_refRedW{-1.0f};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_refLabel{nullptr};
    QLabel* m_swrLabel{nullptr};

    QLabel* m_nameLabel{nullptr};
    QLabel* m_statusPill{nullptr};
    QLabel* m_tempLabel{nullptr};
    QLabel* m_voltsLabel{nullptr};
    QLabel* m_currentLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_freqLabel{nullptr};
    QLabel* m_ifaceLabel{nullptr};
    QLabel* m_tunerLabel{nullptr};
    QLabel* m_deviceStatusLabel{nullptr};
    QLabel* m_faultLabel{nullptr};

    QPushButton* m_operateBtn{nullptr};
    QLabel* m_tunerIndicator{nullptr};  //DH1KLM read-only tuner on/off indicator
    QPushButton* m_resetErrorBtn{nullptr};
    //DH1KLM One button per internal antenna (the API allows 1-4), plus a read-only
    //DH1KLM indicator for the amplifier's single external selection.
    QVector<QPushButton*> m_antBtns;
    QLabel* m_extIndicator{nullptr};  //DH1KLM read-only external-antenna indicator

    QTimer m_labelTimer;

    Rfkit::Status m_status;
    bool m_connected{false};

    //DH1KLM setStatus() arrives at the connection's poll rate — funneled through
    //DH1KLM the m_labelTimer tick, the same throttle convention the other amplifier
    //DH1KLM applets use, rather than repainting and re-announcing accessibility text
    //DH1KLM on every reply.
    bool m_valuesDirty{false};
};

}  //DH1KLM namespace AetherSDR
