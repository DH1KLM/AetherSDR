#pragma once

#include "CanonWindow.h"
#include "models/TxController.h"

#include <QPointer>
#include <QVariantMap>
#include <QString>
#include <array>

class QCloseEvent;
class QCheckBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QLabel;
class QPushButton;
class QTimer;
class QTabWidget;

namespace AetherSDR {
class RadioModel;

class PureSignalWindow final : public CanonWindow {
    Q_OBJECT
public:
    explicit PureSignalWindow(RadioModel& model, QWidget* parent = nullptr);
    void retire();
protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void request(bool write = false, int mode = 0, const QString& operation = {}, const QString& path = {});
    void chooseCorrectionFile(bool save);
    void toggleTwoTone(const TxController::Input& input);
    void stopTwoTone();
    void acceptState(const QVariantMap& state);
    void showAmpView();
    void renderAmpView();
    void setAvailability(bool available, const QString& reason);

    RadioModel& m_model;
    TxController::Input m_twoToneInput;
    QTimer* m_poll;
    QLabel* m_status;
    QLabel* m_actionStatus;
    QLabel* m_routing;
    QLabel* m_feedback;
    QLabel* m_correcting;
    std::array<QDoubleSpinBox*, 4> m_settings{};
    std::array<QLabel*, 16> m_information{};
    QLabel* m_peak;
    QPushButton* m_single;
    QPushButton* m_auto;
    QPushButton* m_off;
    QPushButton* m_twoTone;
    QPushButton* m_apply;
    QPushButton* m_save;
    QPushButton* m_restore;
    QTabWidget* m_views;
    QPointer<QWidget> m_ampView;
    QGraphicsScene* m_scene = nullptr;
    QCheckBox* m_gain = nullptr;
    QCheckBox* m_phaseZoom = nullptr;
    QCheckBox* m_lowRes = nullptr;
    QVariantMap m_state;
    QString m_identity;
    bool m_pending = false;
    bool m_filling = false;
    bool m_dirty = false;
};
} // namespace AetherSDR
