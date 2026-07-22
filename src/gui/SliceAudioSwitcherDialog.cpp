#include "gui/SliceAudioSwitcherDialog.h"

#include "gui/SliceColorManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QColor>
#include <QGridLayout>
#include <QPushButton>

namespace AetherSDR {

namespace {

// Speaker glyphs (UTF-8): U+1F50A SPEAKER HIGH VOLUME, U+1F507 MUTED SPEAKER.
const QString kUnmutedGlyph = QString::fromUtf8("\xF0\x9F\x94\x8A"); // speaker
const QString kMutedGlyph   = QString::fromUtf8("\xF0\x9F\x94\x87"); // muted speaker

// Slice colors are arbitrary (per-slice defaults plus user overrides across the
// whole wheel), so button text has to pick its own contrast rather than assume
// a fixed foreground.  Standard Rec. 601 luma: dark backgrounds get white text,
// light backgrounds get black.
QColor contrastText(const QColor& bg)
{
    const double luma = 0.299 * bg.redF() + 0.587 * bg.greenF() + 0.114 * bg.blueF();
    return luma > 0.5 ? QColor(Qt::black) : QColor(Qt::white);
}

// Up to 4 buttons per row keeps the panel compact for 8-slot (6700/8600) radios
// without going a single button wide.
constexpr int kColumns = 4;

} // namespace

SliceAudioSwitcherDialog::SliceAudioSwitcherDialog(RadioModel* radio, QWidget* parent)
    : PersistentDialog(QStringLiteral("Slice Audio Switcher"),
                       QStringLiteral("SliceAudioSwitcherGeometry"), parent)
    , m_radio(radio)
{
    auto* grid = new QGridLayout(bodyWidget());

    const int slots = m_radio ? m_radio->maxSlices() : 4;
    m_buttons.reserve(slots);
    for (int slot = 0; slot < slots; ++slot) {
        auto* btn = new QPushButton(bodyWidget());
        btn->setMinimumSize(56, 44);
        connect(btn, &QPushButton::clicked, this, [this, slot] {
            // Resolve the live slice at click time -- the slot may have been
            // filled or emptied since the button was created.  We only *request*
            // the toggle; the radio's audioMuteChanged status is what repaints.
            if (SliceModel* s = m_radio ? m_radio->slice(slot) : nullptr)
                s->setAudioMute(!s->audioMute());
        });
        grid->addWidget(btn, slot / kColumns, slot % kColumns);
        m_buttons.append(btn);
    }

    if (m_radio) {
        connect(m_radio, &RadioModel::sliceAdded,
                this, &SliceAudioSwitcherDialog::onSliceAdded);
        connect(m_radio, &RadioModel::sliceRemoved,
                this, [this](int) { refreshAll(); });
        for (SliceModel* s : m_radio->slices())
            connectSlice(s);
    }
    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged,
            this, &SliceAudioSwitcherDialog::refreshAll);

    refreshAll();
}

void SliceAudioSwitcherDialog::onSliceAdded(SliceModel* slice)
{
    connectSlice(slice);
    refreshAll();
}

void SliceAudioSwitcherDialog::connectSlice(SliceModel* slice)
{
    if (!slice)
        return;
    // `this` context auto-disconnects on dialog teardown; slice destruction
    // disconnects its side, so no manual cleanup on sliceRemoved is needed.
    connect(slice, &SliceModel::audioMuteChanged,
            this, [this] { refreshAll(); });
    connect(slice, &SliceModel::letterChanged,
            this, [this] { refreshAll(); });
}

void SliceAudioSwitcherDialog::refreshAll()
{
    for (int slot = 0; slot < m_buttons.size(); ++slot)
        refreshButton(slot);
}

void SliceAudioSwitcherDialog::refreshButton(int slot)
{
    QPushButton* btn = m_buttons.value(slot);
    if (!btn)
        return;

    SliceModel* s = m_radio ? m_radio->slice(slot) : nullptr;
    if (!s) {
        // Empty slot: disabled + dimmed, but still occupies its cell so the
        // panel's shape doesn't jump as slices come and go.
        btn->setEnabled(false);
        btn->setText(QString(QChar('A' + slot)));
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #303030; color: #707070;"
            " border: 1px solid #454545; border-radius: 4px; }"));
        return;
    }

    const QColor bg = SliceColorManager::instance().activeColor(slot);
    const QColor fg = contrastText(bg);
    const QString glyph = s->audioMute() ? kMutedGlyph : kUnmutedGlyph;

    btn->setEnabled(true);
    btn->setText(glyph + QLatin1Char(' ') + s->letter());
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2;"
        " border: 1px solid %3; border-radius: 4px; font-weight: bold; }")
        .arg(bg.name(), fg.name(), bg.darker(150).name()));
}

} // namespace AetherSDR
