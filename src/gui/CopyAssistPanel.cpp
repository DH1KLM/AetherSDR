#include "CopyAssistPanel.h"

#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
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

    // A checkable button (not a checkbox) whose label shows the state
    // (Enabled/Disabled); the app layer gives it the applet-toggle style so the
    // enabled state reads as a distinct colour.
    m_enable = new QPushButton(tr("Disabled"), this);
    m_enable->setCheckable(true);
    m_enable->setAccessibleName(tr("Enable Copy Assist"));
    m_enable->setToolTip(tr("Transcribe received voice to text"));
    connect(m_enable, &QPushButton::toggled, this, [this](bool on) {
        m_enable->setText(on ? tr("Enabled") : tr("Disabled"));
        emit enableToggled(on);
    });
    controls->addWidget(m_enable);

    controls->addWidget(new QLabel(tr("Model:"), this));
    m_tier = new QComboBox(this);
    m_tier->setAccessibleName(tr("Copy Assist model"));
    m_tier->setToolTip(tr("Speech-recognition model (larger = more accurate, slower)"));
    m_tier->setMinimumWidth(160);
    connect(m_tier, &QComboBox::currentIndexChanged, this, [this](int) {
        emit tierChanged(currentTier());
    });
    controls->addWidget(m_tier);

    // GPU selector — hidden unless the controller finds more than one GPU.
    m_gpu = new QComboBox(this);
    m_gpu->setAccessibleName(tr("Copy Assist GPU"));
    m_gpu->setToolTip(tr("Which GPU runs the model"));
    m_gpu->hide();
    connect(m_gpu, &QComboBox::currentIndexChanged, this, [this](int) {
        emit gpuChanged(currentGpu());
    });
    controls->addWidget(m_gpu);

    // Compact inline tuning sliders on the same row (mirrors the CW decode bar).
    m_buffer = addSliderInline(controls, tr("Buffer:"), tr("Decode buffer seconds"),
                               1, 20, 20, &m_bufferValue);
    m_bufferValue->setText(tr("%1 s").arg(m_buffer->value()));
    connect(m_buffer, &QSlider::valueChanged, this, [this](int s) {
        m_bufferValue->setText(tr("%1 s").arg(s));
        emit bufferMsChanged(s * 1000);
    });

    controls->addSpacing(40); // gap between the tuning controls
    m_sensitivity = addSliderInline(controls, tr("Sens:"), tr("VAD sensitivity percent"),
                                    1, 100, 80, &m_sensitivityValue);
    m_sensitivityValue->setText(tr("%1%").arg(m_sensitivity->value()));
    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int pct) {
        m_sensitivityValue->setText(tr("%1%").arg(pct));
        emit sensitivityChanged(pct);
    });

    controls->addSpacing(40); // gap between the tuning controls
    m_silence = addSliderInline(controls, tr("Silence:"), tr("Silence duration milliseconds"),
                                100, 2000, 300, &m_silenceValue);
    m_silence->setSingleStep(50);
    m_silenceValue->setText(tr("%1 ms").arg(m_silence->value()));
    connect(m_silence, &QSlider::valueChanged, this, [this](int ms) {
        m_silenceValue->setText(tr("%1 ms").arg(ms));
        emit silenceMsChanged(ms);
    });

    controls->addStretch(1);

    // Transcript font size (mirrors the CW decode bar's A-/A+).
    auto* fontDown = new QPushButton(tr("A−"), this);
    fontDown->setToolTip(tr("Smaller transcript text"));
    fontDown->setAccessibleName(tr("Decrease transcript font size"));
    connect(fontDown, &QPushButton::clicked, this, [this] { adjustFont(-1); });
    controls->addWidget(fontDown);
    auto* fontUp = new QPushButton(tr("A+"), this);
    fontUp->setToolTip(tr("Larger transcript text"));
    fontUp->setAccessibleName(tr("Increase transcript font size"));
    connect(fontUp, &QPushButton::clicked, this, [this] { adjustFont(1); });
    controls->addWidget(fontUp);

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

    // --- Status line (with an indeterminate loading indicator) --------------
    auto* statusRow = new QHBoxLayout;
    m_busy = new QProgressBar(this);
    m_busy->setRange(0, 0); // indeterminate "busy" animation
    m_busy->setTextVisible(false);
    m_busy->setFixedSize(90, 12);
    m_busy->setAccessibleName(tr("Copy Assist loading"));
    m_busy->hide();
    statusRow->addWidget(m_busy);

    m_status = new QLabel(tr("Disabled"), this);
    m_status->setObjectName(QStringLiteral("CopyAssistStatus"));
    m_status->setAccessibleName(tr("Copy Assist status"));
    statusRow->addWidget(m_status, 1);

    root->addLayout(statusRow);

    applyFont();
}

void CopyAssistPanel::applyFont()
{
    QFont f = m_text->font();
    f.setPixelSize(m_fontPx);
    m_text->setFont(f);                    // default for newly appended text
    m_text->document()->setDefaultFont(f);

    // Resize text already inserted via insertHtml. Merging only the font onto the
    // whole document leaves the per-utterance confidence colours untouched.
    QTextCursor cursor(m_text->document());
    cursor.select(QTextCursor::Document);
    QTextCharFormat fmt;
    fmt.setFont(f);
    cursor.mergeCharFormat(fmt);

    QTextCursor atEnd(m_text->document());
    atEnd.movePosition(QTextCursor::End);
    m_text->setTextCursor(atEnd);          // clear the selection, keep scrolled to end
}

void CopyAssistPanel::adjustFont(int deltaPx)
{
    const int next = std::clamp(m_fontPx + deltaPx, 8, 32);
    if (next == m_fontPx) {
        return;
    }
    m_fontPx = next;
    applyFont();
    emit fontPxChanged(m_fontPx);
}

void CopyAssistPanel::setFontPx(int px)
{
    m_fontPx = std::clamp(px, 8, 32);
    applyFont();
}

QSlider* CopyAssistPanel::addSliderInline(QHBoxLayout* bar, const QString& label,
                                          const QString& accessibleName, int lo, int hi,
                                          int value, QLabel** valueLabelOut)
{
    bar->addWidget(new QLabel(label, this));
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(lo, hi);
    slider->setValue(value);
    slider->setFixedWidth(90); // compact, like the CW decode bar's sliders
    slider->setAccessibleName(accessibleName);
    bar->addWidget(slider);
    auto* valueLabel = new QLabel(this);
    valueLabel->setMinimumWidth(44);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(valueLabel);
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

void CopyAssistPanel::setTierLabel(const QString& id, const QString& label)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setItemText(idx, label);
    }
}

QString CopyAssistPanel::currentTier() const
{
    return m_tier->currentData().toString();
}

void CopyAssistPanel::addGpuDevice(int index, const QString& name)
{
    m_gpu->addItem(name, index);
}

void CopyAssistPanel::setCurrentGpu(int index)
{
    const int idx = m_gpu->findData(index);
    if (idx >= 0) {
        m_gpu->setCurrentIndex(idx);
    }
}

int CopyAssistPanel::currentGpu() const
{
    return m_gpu->currentData().toInt();
}

void CopyAssistPanel::setGpuSelectorVisible(bool on)
{
    m_gpu->setVisible(on);
}

void CopyAssistPanel::setBusy(bool on)
{
    m_busy->setVisible(on);
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
    // Bake the current size into the span so new text renders at the saved size
    // even before any font-change (applyFont on an empty document can't stick).
    m_text->insertHtml(QStringLiteral("<span style=\"color:%1; font-size:%2px\">%3</span> ")
                           .arg(colorForConfidence(confidence), QString::number(m_fontPx),
                                trimmed.toHtmlEscaped()));
    m_text->moveCursor(QTextCursor::End);
}

void CopyAssistPanel::clearText()
{
    m_text->clear();
}

} // namespace AetherSDR
