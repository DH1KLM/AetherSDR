#include "CopyAssistSettingsDialog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace AetherSDR {

CopyAssistSettingsDialog::CopyAssistSettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("CopyAssistSettingsDialog"));
    setWindowTitle(tr("Copy Assist Settings"));
    setModal(false); // modeless: never blocks the main window
    // Float above the app as a tool window (out of the taskbar), and let the
    // window close button just hide it — the panel's ⚙ toggles it back.
    setWindowFlags(windowFlags() | Qt::Tool);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_tier = new QComboBox(this);
    m_tier->setObjectName(QStringLiteral("CopyAssistModelCombo"));
    m_tier->setAccessibleName(tr("Copy Assist model"));
    m_tier->setToolTip(tr("Speech-recognition model (larger = more accurate, slower)"));
    m_tier->setMinimumWidth(240);
    connect(m_tier, &QComboBox::currentIndexChanged, this,
            [this](int) { emit tierChanged(currentTier()); });
    form->addRow(tr("Model:"), m_tier);

    m_gpu = new QComboBox(this);
    m_gpu->setObjectName(QStringLiteral("CopyAssistGpuCombo"));
    m_gpu->setAccessibleName(tr("Copy Assist compute device"));
    m_gpu->setToolTip(tr("Which device runs the model (a GPU, or CPU)"));
    connect(m_gpu, &QComboBox::currentIndexChanged, this,
            [this](int) { emit gpuChanged(currentGpu()); });
    m_gpuLabel = new QLabel(tr("Compute:"), this);
    form->addRow(m_gpuLabel, m_gpu);
    // Hidden until the controller reports a GPU exists (CPU-only hosts show no
    // device picker at all).
    m_gpuLabel->hide();
    m_gpu->hide();

    root->addLayout(form);
    root->addStretch(1); // headroom for further options added here later
}

void CopyAssistSettingsDialog::addTier(const QString& id, const QString& label)
{
    m_tier->addItem(label, id);
}

void CopyAssistSettingsDialog::setCurrentTier(const QString& id)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setCurrentIndex(idx);
    }
}

void CopyAssistSettingsDialog::setTierLabel(const QString& id, const QString& label)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setItemText(idx, label);
    }
}

QString CopyAssistSettingsDialog::currentTier() const
{
    return m_tier->currentData().toString();
}

void CopyAssistSettingsDialog::addGpuDevice(int index, const QString& name)
{
    m_gpu->addItem(name, index);
}

void CopyAssistSettingsDialog::setCurrentGpu(int index)
{
    const int idx = m_gpu->findData(index);
    if (idx >= 0) {
        m_gpu->setCurrentIndex(idx);
    }
}

int CopyAssistSettingsDialog::currentGpu() const
{
    return m_gpu->currentData().toInt();
}

void CopyAssistSettingsDialog::setGpuSelectorVisible(bool on)
{
    m_gpuLabel->setVisible(on);
    m_gpu->setVisible(on);
}

} // namespace AetherSDR
