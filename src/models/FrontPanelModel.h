#pragma once
#include "core/TxCoordinator.h"
#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QMap>
#include <QStringList>
#include <QVariantMap>
#include <QTcpServer>
#include <QTcpSocket>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
namespace AetherSDR {
class RadioModel;
class SliceModel;
class FrontPanelModel : public QObject {
    Q_OBJECT
public:
    explicit FrontPanelModel(RadioModel& model);
    ~FrontPanelModel() override;
    QVariantMap state() const;
    bool configure(const QVariantMap& values);
    void refresh();
    void setLearning(bool on);
    static QStringList actions(const QString& kind);
    static QJsonObject standardAndromedaBindings();
signals:
    void stateChanged();
private:
    friend class FrontPanelTestAccess;
    bool applyConfiguration(const QVariantMap& values, bool persist);
    void closeTransport();
    void consume(const QByteArray& bytes);
    void send(const QByteArray& bytes);
    void updateIndicators();
    void turn(const QString& action, int steps, SliceModel* slice);
    void press(const QString& action, int number, int state, SliceModel* slice);
    SliceModel* target(int selector) const;
    QJsonObject binding(const QString& key) const;
    RadioModel& m_model;
    TxCoordinator::Producer m_producer;
    TxCoordinator::Request m_mox;
    TxCoordinator::Request m_tune;
    QMap<int, TxCoordinator::Request> m_heldPtt;
    QJsonObject m_config;
    QJsonObject m_bindings;
    bool m_futureVersion = false;
    bool m_identified = false;
    bool m_registered = false;
    bool m_learning = false;
    bool m_opening = false;
    int m_hardwareVersion = -1;
    int m_firmwareVersion = -1;
    QString m_status;
    QString m_lastInput;
    QString m_lastControl;
    QByteArray m_buffer;
    QSet<int> m_pressed;
    QMap<int, bool> m_leds;
    QTimer m_tick;
    QTimer m_retry;
    QTcpServer m_server;
    QPointer<QTcpSocket> m_peer;
#ifdef HAVE_SERIALPORT
    QSerialPort m_serial;
#endif
};
} // namespace AetherSDR
