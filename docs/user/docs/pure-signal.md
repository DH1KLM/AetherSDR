---
title: "Pure Signal"
slug: "/pure-signal"
description: "OpenHPSDR calibration controls and AmpView in one independent workspace window."
status: "Experimental"
applies_to: ["OpenHPSDR (two-ADC devices)"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** OpenHPSDR (two-ADC devices)

:::

Open **Tools → Pure Signal**, directly below **Phasing Control...**. The
command is available only with a connected OpenHPSDR device. Calibration
controls and **AmpView** share one independent window.

The footer also shows **PS ACTIVE** while correction is applied. Its feedback
bar overlays the numeric calibration level: **LOW** (0–128) and **HIGH**
(above 181) are red; **OK** (129–181) is green only with active correction
and fresh paired feedback. **PS OFF**, **PS ARMED**, **PS READY**, and **PS WAIT**
are neutral. A dash means there is no fresh level; a retained correction does
not leave an old green level showing during reception or a data interruption.
The indication remains available when the Pure Signal window is closed.
Double-click the PS indicator, or focus it and press Enter, to open that window.

## Requirements

- A validated two-ADC OpenHPSDR device and a prepared TX session.
- Diversity disabled so the feedback receivers are available.
- For Protocol 1, a **192 kHz** receive sample rate and at most two receiver
  streams. Protocol 2 needs two spare physical DDCs.
- A suitable attenuated PA feedback connection. Actual correction performance
  requires an operator-controlled hardware test.

## Using Pure Signal

**Single Cal** arms one calibration. **Start Auto** arms repeated calibration.
Changes to MOX wait, calibration wait, amplifier delay, and SetPk apply when
you press either calibration action.
Neither action starts transmission. Calibration receives feedback only during
an operator-initiated transmission. Finish transmission before changing the
calibration configuration or pressing **OFF**.

**OFF** installs a fresh, uncorrected TX processing path. Closing the window
does not disable correction. Disabling the TX session restores an uncorrected
path; reconnecting does not automatically arm Pure Signal.

**AmpView** selects the diagram inside the same window. Select **Calibration**
to return. **Show Gain**, **Phase Zoom**, and **Low Res** affect presentation.
The diagram waits for real calibration samples rather than showing simulated
amplifier curves.

## Reference

The action row is **Two-Tone / Single Cal / Start Auto / Apply Current /
AmpView / Advanced / Save / Restore / OFF**. **Advanced** expands or collapses
raw calibration information. **Always On Top** changes window placement.

**Apply Current** selects retained coefficients without continuing automatic
calibration. **Save** writes the actual current coefficients atomically.
**Restore** validates and loads a WDSP version 2 correction into a fresh TX
processing path. Neither action starts transmission. Finish transmission
before using correction files. Older version 1 files require recalibration.

**Two-Tone** starts a 700/1900 Hz test signal through the TX DSP using the
configured TUNE power. It requires a prepared USB or LSB TX session with
both tones inside the TX filter. Press **Stop Two-Tone** or close the window
to end this window's test transmission. Microphone audio is excluded while
the generator runs. The normal transmit interlocks and authorization limits
apply.

**Auto-Attenuate** adjusts the feedback step attenuator during single or
automatic calibration. Enable it before pressing **Single Cal** or **Start Auto**.
Each valid new calibration result can request one adjustment within 0–31 dB.
Calibration pauses until CALCC and the IQC correction have drained, and resumes
after 100 ms of settling. Ending transmission cancels uncommitted attenuation
changes. The displayed attenuation is the client's request, not hardware readback.

**Display PS-RX and PS-TX spectra** opens **Feedback spectra** inside the same
window. It plots the paired TX reference and PA feedback in digital dBFS over
±10 kHz. Arm Pure Signal and transmit deliberately to supply these samples.
The levels are digital sample levels, not calibrated RF power.

**Show 2Tone measurements** displays windowed IMD3, IMD5, IMD7, and IMD9 plus
noise during the built-in **Two-Tone** transmission. Each value uses the
stronger of the lower/upper products relative to the mean power of one
feedback carrier, in dBc per tone. Missing carriers, possible sample clipping,
and stale feedback make the measurements unavailable. Gaps restart acquisition;
a complete fresh window is required before values appear again.

**Quick Attenuate Response** remains unavailable with an explanatory tooltip.
Other OpenHPSDR hardware shapes also show a reason when calibration is unavailable.

The feedback level, calibration attempts, successful calibration count, and
correction state represent different facts. An applied correction does not
by itself establish adequate feedback or a measured IMD improvement.
During transmission the status reports paired blocks received by calibration
and the latest TX-reference and PA-feedback sample peaks (digital full scale
is 1). A zero **Feedback Level** does not prove that no packets arrive: it is
a calibration result, not a packet counter. Native **state** 4 means collecting
samples; a usable calibration can still be missing. A stale or missing stream
is reported explicitly. The ADC page shows configured feedback routes separately.
The TX-reference sample peak and spectra show raw received levels. Calibration
internally compensates the TX path's 6 dB digital headroom, so these raw levels
are not directly comparable with **SetPk**. This compensation does not change
transmit power, the feedback attenuator or the hardware-specific **SetPk**.

## Making a change

Edit this page in `docs/user/docs/pure-signal.md` and run `npm run build`
from `docs/user` to check links before opening a pull request.

## See also

- [ANAN and OpenHPSDR](./anan-g2.md)
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
