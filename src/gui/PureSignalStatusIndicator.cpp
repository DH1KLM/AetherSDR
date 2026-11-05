#include "PureSignalStatusIndicator.h"
#include "StatusIndicator.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVariantList>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace AetherSDR {
PureSignalStatusIndicator::PureSignalStatusIndicator(RadioModel& model,
    std::function<void()> openWindow, QWidget* parent)
    : QWidget(parent), m_model(model), m_title(new QLabel(this)),
      m_level(new QProgressBar(this)), m_timer(new QTimer(this))
{
    setObjectName(QStringLiteral("pureSignalStatusIndicator"));
    setAccessibleName(tr("Pure Signal status and feedback level"));
    setCursor(Qt::PointingHandCursor);
    setFixedWidth(94);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(3,0,3,0); layout->setSpacing(0);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setObjectName(QStringLiteral("pureSignalFooterState"));
    m_level->setObjectName(QStringLiteral("pureSignalFooterLevel"));
    m_level->setAccessibleName(tr("Pure Signal feedback level"));
    m_level->setRange(0,256); m_level->setTextVisible(true); m_level->setFixedHeight(17);
    m_level->setAlignment(Qt::AlignCenter);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_level->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_title); layout->addWidget(m_level);
    StatusIndicator* activation = StatusIndicator::attach(this);
    connect(activation,&StatusIndicator::activated,this,openWindow);
    connect(activation,&StatusIndicator::doubleClicked,this,std::move(openWindow));
    connect(m_timer,&QTimer::timeout,this,[this] { poll(); });
    connect(&model,&RadioModel::connectionStateChanged,this,[this](bool) { poll(); });
    connect(&model,&RadioModel::capabilitiesChanged,this,[this] { poll(); });
    m_timer->start(500); poll();
}

void PureSignalStatusIndicator::showState(const QString& title, const QString& overlay, int level,
    const QString& description, const QString& tone)
{
    const QString help = description + tr(" Double-click or press Enter to open Pure Signal.");
    const bool changed = accessibleDescription() != help;
    m_title->setText(title); m_level->setFormat(overlay);
    m_level->setValue(std::clamp(level,0,256));
    setToolTip(help); setAccessibleDescription(help);
    m_level->setAccessibleDescription(description);
    if (property("feedbackTone").toString() != tone) {
        setProperty("feedbackTone",tone);
        const QString color = tone == QLatin1String("good") ? QStringLiteral("color.accent.success")
            : tone == QLatin1String("bad") ? QStringLiteral("color.accent.danger") : QStringLiteral("color.text.secondary");
        ThemeManager::instance().applyStyleSheet(this,QStringLiteral(
            "QLabel { color: {{%1}}; font-size: 11px; } "
            "QProgressBar { color: {{%1}}; background: {{color.background.1}}; "
            "border: 1px solid {{%1}}; border-radius: 3px; font-size: 10px; text-align: center; } "
            "QProgressBar::chunk { background: {{color.canon.control}}; }").arg(color));
    }
    if (changed && QAccessible::isActive()) {
        QAccessibleEvent event(this,QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void PureSignalStatusIndicator::acceptState(const QVariantMap& state)
{
    const int mode = state.value(QStringLiteral("mode")).toInt();
    if (mode == 0) {
        showState(tr("PS OFF"),QStringLiteral("—"),0,tr("Pure Signal is off."),QStringLiteral("neutral")); return;
    }
    const bool correcting = state.value(QStringLiteral("correcting")).toBool();
    const bool fresh = state.value(QStringLiteral("feedbackFresh")).toBool();
    const QVariantList information = state.value(QStringLiteral("information")).toList();
    if (!fresh || information.size() != 16) {
        showState(state.value(QStringLiteral("correctionAvailable")).toBool() ? tr("PS READY") : tr("PS ARMED"),
            QStringLiteral("—"),0,tr("Pure Signal is armed; no fresh feedback level is available. A retained correction may apply on the next transmission."),
            QStringLiteral("neutral")); return;
    }
    const int level = information[4].toInt();
    const bool inRange = level > 128 && level <= 181;
    const QString condition = level <= 128 ? tr("LOW") : level > 181 ? tr("HIGH") : tr("OK");
    const QString detail = tr("%1; feedback %2: %3. This does not prove an IMD improvement.")
        .arg(correcting ? tr("Correction applied") : tr("Waiting for correction"),condition).arg(level);
    showState(correcting ? tr("PS ACTIVE") : tr("PS ARMED"),QStringLiteral("%1 %2").arg(condition).arg(level),level,
        detail,inRange ? correcting ? QStringLiteral("good") : QStringLiteral("neutral") : QStringLiteral("bad"));
}

void PureSignalStatusIndicator::poll()
{
    IRadioBackend* current = m_model.backend();
    const bool supported = m_model.isConnected() && m_model.backendDeclaresExtension(QStringLiteral("anan"));
    setEnabled(supported);
    if (!supported || current != m_backend) {
        disconnect(m_resultConnection); disconnect(m_errorConnection);
        m_backend = nullptr; m_pending = 0; m_replyAge.invalidate();
        showState(supported ? tr("PS WAIT") : tr("PS N/A"),QStringLiteral("—"),0,
            supported ? tr("Waiting for the current radio's Pure Signal status.") : tr("Connect an OpenHPSDR device to use Pure Signal."),
            QStringLiteral("neutral"));
        if (!supported || !current) { return; }
        m_backend = current;
        m_resultConnection = connect(current,&IRadioBackend::extensionResult,this,[this,current](quint64 id,const QVariant& value) {
            if (id != m_pending || current != m_model.backend() || !m_model.isConnected()) { return; }
            m_pending = 0; m_replyAge.start(); acceptState(value.toMap());
        });
        m_errorConnection = connect(current,&IRadioBackend::extensionError,this,[this,current](quint64 id,const QString&) {
            if (id != m_pending || current != m_model.backend()) { return; }
            m_pending = 0; m_replyAge.invalidate();
            showState(tr("PS WAIT"),QStringLiteral("—"),0,tr("Pure Signal status is unavailable."),QStringLiteral("neutral"));
        });
    }
    if (m_replyAge.isValid() && m_replyAge.elapsed() > 1500) {
        showState(tr("PS WAIT"),QStringLiteral("—"),0,tr("Pure Signal status is stale."),QStringLiteral("neutral"));
    }
    if (m_pending && m_requestAge.elapsed() <= 1500) { return; }
    static quint64 nextId = 0x505346000000ULL;
    m_pending = ++nextId; m_requestAge.start();
    m_model.invokeBackendExtension(QStringLiteral("anan"),QStringLiteral("puresignal.get"),m_pending,{});
}
}
