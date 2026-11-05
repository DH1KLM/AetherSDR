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

Automatic attenuation and feedback-spectrum measurements are unavailable.
Their disabled controls explain the missing path. Other OpenHPSDR hardware shapes also show a reason when calibration is unavailable.

The feedback level, calibration attempts, successful calibration count, and
correction state represent different facts. An applied correction does not
by itself establish adequate feedback or a measured IMD improvement.

## Making a change

Edit this page in `docs/user/docs/pure-signal.md` and run `npm run build`
from `docs/user` to check links before opening a pull request.

## See also

- [ANAN and OpenHPSDR](./anan-g2.md)
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
