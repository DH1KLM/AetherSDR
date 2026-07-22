#pragma once

#include "gui/PersistentDialog.h"

#include <QVector>

class QPushButton;

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Compact "Slice Audio Switcher" panel (Settings -> "Slice Audio Switcher...").
// One button per slice slot, sized to RadioModel::maxSlices() so it shows 4
// slots on standard radios and grows to 8 on dual-SCU models (6700/8600).
// Each occupied slot is painted with its SliceColorManager color, shows a
// speaker glyph + the slice's display letter, and a click toggles that slice's
// audioMute.  Empty slots render disabled/dimmed so the panel keeps a stable
// shape as slices come and go.
//
// This is a pure client-side view over the radio's per-slice audioMute state:
// it mirrors audioMuteChanged/letterChanged/colorsChanged and issues
// setAudioMute() *requests* only -- it never writes a remembered value back over
// the radio (Principle II -- the radio is authoritative on live state).
class SliceAudioSwitcherDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit SliceAudioSwitcherDialog(RadioModel* radio, QWidget* parent = nullptr);

private slots:
    void onSliceAdded(SliceModel* slice);
    void refreshAll();

private:
    void connectSlice(SliceModel* slice);
    void refreshButton(int slot);

    RadioModel*           m_radio{nullptr};
    QVector<QPushButton*> m_buttons;
};

} // namespace AetherSDR
