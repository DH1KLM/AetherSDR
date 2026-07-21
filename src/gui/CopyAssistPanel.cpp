#include "CopyAssistPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

CopyAssistPanel::CopyAssistPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("CopyAssistPanel"));
    setAccessibleName(tr("Copy Assist"));
    setAccessibleDescription(tr("Speech-to-text transcript of received voice, "
                                "color-coded by recognition confidence."));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // --- Controls row -------------------------------------------------------
    auto* controls = new QHBoxLayout;

    m_enable = new QCheckBox(tr("Enable"), this);
    m_enable->setAccessibleName(tr("Enable Copy Assist"));
    m_enable->setToolTip(tr("Transcribe received voice to text"));
    connect(m_enable, &QCheckBox::toggled, this, &CopyAssistPanel::enableToggled);
    controls->addWidget(m_enable);

    controls->addWidget(new QLabel(tr("Model:"), this));
    m_tier = new QComboBox(this);
    m_tier->setAccessibleName(tr("Copy Assist model"));
    m_tier->setToolTip(tr("Speech-recognition model (larger = more accurate, slower)"));
    connect(m_tier, &QComboBox::currentIndexChanged, this, [this](int) {
        emit tierChanged(currentTier());
    });
    controls->addWidget(m_tier, 1);

    m_clear = new QPushButton(tr("Clear"), this);
    m_clear->setAccessibleName(tr("Clear transcript"));
    connect(m_clear, &QPushButton::clicked, this, [this] {
        clearText();
        emit clearRequested();
    });
    controls->addWidget(m_clear);

    auto* closeBtn = new QPushButton(QString::fromUtf8("\xC3\x97"), this); // ×
    closeBtn->setFixedSize(18, 18);
    closeBtn->setAccessibleName(tr("Close Copy Assist"));
    closeBtn->setToolTip(tr("Close the Copy Assist panel"));
    connect(closeBtn, &QPushButton::clicked, this, &CopyAssistPanel::closeRequested);
    controls->addWidget(closeBtn);

    root->addLayout(controls);

    // --- Tuning sliders (decode buffer / VAD sensitivity / silence) ---------
    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    m_buffer = addSliderRow(grid, 0, tr("Buffer:"), tr("Decode buffer seconds"),
                            1, 20, 20, &m_bufferValue);
    m_bufferValue->setText(tr("%1 s").arg(m_buffer->value()));
    connect(m_buffer, &QSlider::valueChanged, this, [this](int s) {
        m_bufferValue->setText(tr("%1 s").arg(s));
        emit bufferMsChanged(s * 1000);
    });

    m_sensitivity = addSliderRow(grid, 1, tr("Sensitivity:"), tr("VAD sensitivity percent"),
                                 1, 100, 80, &m_sensitivityValue);
    m_sensitivityValue->setText(tr("%1%").arg(m_sensitivity->value()));
    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int pct) {
        m_sensitivityValue->setText(tr("%1%").arg(pct));
        emit sensitivityChanged(pct);
    });

    m_silence = addSliderRow(grid, 2, tr("Silence:"), tr("Silence duration milliseconds"),
                             100, 2000, 300, &m_silenceValue);
    m_silence->setSingleStep(50);
    m_silenceValue->setText(tr("%1 ms").arg(m_silence->value()));
    connect(m_silence, &QSlider::valueChanged, this, [this](int ms) {
        m_silenceValue->setText(tr("%1 ms").arg(ms));
        emit silenceMsChanged(ms);
    });

    root->addLayout(grid);

    // --- Transcript (mirrors the CW decode QTextEdit) -----------------------
    m_text = new QTextEdit(this);
    m_text->setObjectName(QStringLiteral("CopyAssistTranscript"));
    m_text->setAccessibleName(tr("Copy Assist transcript"));
    m_text->setReadOnly(true);
    m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_text->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_text, &QTextEdit::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu* menu = m_text->createStandardContextMenu();
        menu->addSeparator();
        QAction* clear = menu->addAction(tr("Clear"));
        connect(clear, &QAction::triggered, this, [this] {
            clearText();
            emit clearRequested();
        });
        menu->exec(m_text->mapToGlobal(pos));
        delete menu;
    });
    root->addWidget(m_text, 1);

    // --- Status line --------------------------------------------------------
    m_status = new QLabel(tr("Disabled"), this);
    m_status->setObjectName(QStringLiteral("CopyAssistStatus"));
    m_status->setAccessibleName(tr("Copy Assist status"));
    root->addWidget(m_status);
}

QSlider* CopyAssistPanel::addSliderRow(QGridLayout* grid, int row, const QString& label,
                                       const QString& accessibleName, int lo, int hi,
                                       int value, QLabel** valueLabelOut)
{
    grid->addWidget(new QLabel(label, this), row, 0);
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(lo, hi);
    slider->setValue(value);
    slider->setAccessibleName(accessibleName);
    grid->addWidget(slider, row, 1);
    auto* valueLabel = new QLabel(this);
    valueLabel->setMinimumWidth(48);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(valueLabel, row, 2);
    *valueLabelOut = valueLabel;
    return slider;
}

void CopyAssistPanel::setBufferMs(int ms)
{
    m_buffer->setValue(std::clamp(ms, 1000, 20000) / 1000);
}

int CopyAssistPanel::bufferMs() const
{
    return m_buffer->value() * 1000;
}

void CopyAssistPanel::setSensitivity(int percent)
{
    m_sensitivity->setValue(std::clamp(percent, 1, 100));
}

int CopyAssistPanel::sensitivity() const
{
    return m_sensitivity->value();
}

void CopyAssistPanel::setSilenceMs(int ms)
{
    m_silence->setValue(std::clamp(ms, 100, 2000));
}

int CopyAssistPanel::silenceMs() const
{
    return m_silence->value();
}

void CopyAssistPanel::addTier(const QString& id, const QString& label)
{
    m_tier->addItem(label, id);
}

void CopyAssistPanel::setCurrentTier(const QString& id)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setCurrentIndex(idx);
    }
}

QString CopyAssistPanel::currentTier() const
{
    return m_tier->currentData().toString();
}

void CopyAssistPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}

bool CopyAssistPanel::isAsrEnabled() const
{
    return m_enable->isChecked();
}

void CopyAssistPanel::setAsrEnabled(bool on)
{
    m_enable->setChecked(on);
}

QString CopyAssistPanel::colorForConfidence(float confidence)
{
    // Higher whisper confidence = better copy (inverse of the CW cost scale).
    if (confidence >= 0.85f) {
        return QStringLiteral("#00ff88"); // green  — high confidence
    }
    if (confidence >= 0.65f) {
        return QStringLiteral("#e0e040"); // yellow — medium
    }
    if (confidence >= 0.45f) {
        return QStringLiteral("#ff9020"); // orange — low
    }
    return QStringLiteral("#ff4040");     // red    — very low
}

void CopyAssistPanel::appendText(const QString& text, float confidence)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    m_text->moveCursor(QTextCursor::End);
    m_text->insertHtml(QStringLiteral("<span style=\"color:%1\">%2</span> ")
                           .arg(colorForConfidence(confidence), trimmed.toHtmlEscaped()));
    m_text->moveCursor(QTextCursor::End);
}

void CopyAssistPanel::clearText()
{
    m_text->clear();
}

} // namespace AetherSDR
