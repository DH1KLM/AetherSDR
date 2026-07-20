#include "CopyAssistWindow.h"

#include "CopyAssistController.h"
#include "CopyAssistPanel.h"

#include <QVBoxLayout>

namespace AetherSDR {

CopyAssistWindow::CopyAssistWindow(AudioEngine* audio, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_panel(new CopyAssistPanel(this))
    , m_controller(new CopyAssistController(audio, m_panel, this))
{
    setObjectName(QStringLiteral("CopyAssistWindow"));
    setWindowTitle(tr("Copy Assist — Speech to Text"));
    resize(420, 320);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_panel);
}

} // namespace AetherSDR
