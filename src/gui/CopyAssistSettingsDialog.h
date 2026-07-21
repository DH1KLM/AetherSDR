#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;

namespace AetherSDR {

// Modeless settings dialog for Copy Assist (RFC #4333). Houses the model-tier
// and compute-device (GPU/CPU) selectors — moved out of the panel's cramped
// control row — behind the panel's ⚙ button, with room for further options.
//
// Like CopyAssistPanel it stays ThemeManager-free (so it links in the
// lightweight offscreen unit test); the controller populates it, wires its
// signals, and applies any theming. It mirrors the panel's old model/GPU API so
// the controller's call sites move over unchanged.
class CopyAssistSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CopyAssistSettingsDialog(QWidget* parent = nullptr);

    // Model tier selector (id + human label).
    void addTier(const QString& id, const QString& label);
    void setCurrentTier(const QString& id);
    void setTierLabel(const QString& id, const QString& label);
    QString currentTier() const;

    // Compute-device selector — shown only when the controller finds a GPU.
    void addGpuDevice(int index, const QString& name);
    void setCurrentGpu(int index);
    int currentGpu() const;
    void setGpuSelectorVisible(bool on);

signals:
    void tierChanged(const QString& tierId);
    void gpuChanged(int index);

private:
    QComboBox* m_tier = nullptr;
    QComboBox* m_gpu = nullptr;
    QLabel* m_gpuLabel = nullptr; // paired with m_gpu so both hide together
};

} // namespace AetherSDR
