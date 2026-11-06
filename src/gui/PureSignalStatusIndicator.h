#pragma once

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QProgressBar;
class QTimer;

namespace AetherSDR {
class IRadioBackend;
class RadioModel;

class PureSignalStatusIndicator final : public QWidget {
public:
    PureSignalStatusIndicator(RadioModel& model, QWidget* parent);

private:
    friend struct PureSignalStatusIndicatorTestAccess;
    void poll();
    void activate();
    QVariantMap activationFields() const;
    void acceptState(const QVariantMap& state);
    void showState(const QString& title, const QString& overlay, int level,
        const QString& description, const QString& tone);
    RadioModel& m_model;
    QLabel* m_title;
    QProgressBar* m_level;
    QTimer* m_timer;
    QPointer<IRadioBackend> m_backend;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
    QElapsedTimer m_replyAge;
    QElapsedTimer m_requestAge;
    quint64 m_pending = 0;
    quint64 m_activation = 0;
    QElapsedTimer m_activationAge;
    QVariantMap m_state;
    QString m_activationError;
};
}
