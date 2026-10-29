#pragma once

#include <QAccessible>
#include <QAccessibleEvent>
#include <QLabel>
#include <QEvent>
#include <QFontMetrics>
#include <QSizePolicy>
#include <QStatusBar>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// Notices are right-aligned overlays outside the layout, so telemetry and
// station controls keep their geometry while a message is visible.
class StatusBarNoticeLabel : public QLabel {
public:
    explicit StatusBarNoticeLabel(QStatusBar* bar)
        : QLabel(bar)
    {
        setObjectName(QStringLiteral("statusBarNoticeLabel"));
        setTextFormat(Qt::PlainText);
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        setMinimumWidth(1);
        m_hideTimer.setSingleShot(true);
        QObject::connect(&m_hideTimer, &QTimer::timeout, this, [this] { clearNotice(); });
        if (bar) {
            bar->installEventFilter(this);
        }
        hide();
    }

    void showNotice(const QString& text, int durationMs)
    {
        setText(text);
        setAccessibleName(text);
        updateOverlayGeometry();
        show();
        raise();
        m_hideTimer.stop();
        if (durationMs > 0) {
            m_hideTimer.start(durationMs);
        }
        // Polite: it never interrupts speech the operator asked for.
        if (QAccessible::isActive()) {
            QAccessibleAnnouncementEvent ev(this, text);
            ev.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
            QAccessible::updateAccessibility(&ev);
        }
    }

    void clearNotice()
    {
        m_hideTimer.stop();
        hide();
        clear();
        setAccessibleName(QString());
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            updateOverlayGeometry();
        }
        return QLabel::eventFilter(watched, event);
    }

private:
    void updateOverlayGeometry()
    {
        if (const QWidget* bar = parentWidget()) {
            const int textWidth = fontMetrics().horizontalAdvance(text())
                + contentsMargins().left() + contentsMargins().right();
            const int width = qMin(bar->width(), textWidth);
            setGeometry(bar->width() - width, 0, width, bar->height());
        }
    }

    QTimer m_hideTimer;
};

} // namespace AetherSDR
