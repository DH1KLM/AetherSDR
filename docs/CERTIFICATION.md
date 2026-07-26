# Radio certification — lessons and roadmap

Why `radiocert` is shaped the way it is, and what it still cannot do.

The reference tables live in [`radio-certification.md`](radio-certification.md);
the Hermes-Lite 2 bring-up narrative lives in `HERMES.md`. This file is the
part that generalises — the reasoning a future agent needs before it trusts a
clean report, and the work queued behind it.

---

## 1. The lessons, in the order they cost us

### 1.1 A convention error is invisible to any test that shares the convention

Transmit ran on the **wrong sideband** for a fortnight. Every internal
instrument agreed it was right, because the panadapter reads the same wire order
as the transmitter and therefore cannot disagree with it.

| Check | Result at the time |
|---|---|
| Modulator sideband assertion | 85 dB suppression, "correct" side — passed |
| Simulator loopback | tone at the expected bin — passed |
| Live panadapter | clean single sideband, correct side of centre |
| USB vs LSB forward power | 3875 vs 3876 — identical |
| TX FIFO depth | healthy, refuting the starvation theory |

Found in one sentence by an operator with a second receiver: *"I heard the LSB
side of AetherSDR on the USB side of the Yaesu."*

**Consequence for certification.** Self-consistency is not correctness, and
adding more internal tests increases confidence without increasing truth. Every
phase must state which of its checks are independent and which merely agree with
the implementation. The report ends with what it cannot determine for this
reason, not as a disclaimer.

### 1.2 Two compensating errors cancel everywhere except one geometry

The receive path handed the demodulator the conjugate and the spectrum the raw
wire — each wired to the other's convention. At normal off-centre tuning the two
errors cancelled and the audio was correct.

**Consequence.** Measure at **zero shift** first. Compensating errors cancel at
every other geometry, so a measurement taken at normal tuning proves nothing.
Force it by tuning far enough that the DDC must re-centre, then landing on the
target.

### 1.3 A single-mode test proves nothing about handedness

`hl2_shift_test` validated in LSB — the one mode the inversion made correct —
and passed throughout.

**Consequence.** Sideband checks run in **all four** SSB-family modes, and never
with the test signal at the pan centre: a mirror is invisible on its own axis.

### 1.4 Test stimulus must use the wire's convention, not the textbook's

Both receive unit tests generated `exp(+jwt)`. The wire sends `exp(-jwt)`.
Correct expectations against the wrong stimulus, so a mirrored panadapter *and*
an inverted demodulator both passed.

**Consequence.** Synthesise stimulus in the convention the hardware actually
uses. This is also the cheapest check available — it needs no radio.

### 1.5 The bridge is not the UI

Three separate bugs reached the operator because the automation bridge drives
`RadioModel` and the GUI's transmit controls drive `TransmitModel`, which emits
Flex command strings that never reach a backend without a command channel.
Keying worked perfectly under test and did nothing when MOX was pressed.

**Consequence.** `radiocert` keys through `TransmitModel::requestPttOn`, the
same path as the MOX button. A diagnostic that keyed the way only the bridge can
would inherit the blindness it exists to remove.

### 1.6 Readback proves nothing

The mode map passed for twelve modes while the backend mapped nine. `RTTY` still
reads back perfectly and is demodulated as USB.

**Consequence.** Controls are certified by **effect**. Halving a linear gain is
−6.02 dB; that is arithmetic rather than a property of any radio, which is what
makes it a threshold that transfers to hardware nobody has characterised.
Measured on the HL2: 6.023 dB.

### 1.7 Validate at the rate the UI actually produces

A filter-pipeline reset was validated with seven writes two seconds apart and
shipped. A pan drag issues a centre command every 33 ms, so the real path fired
~30 resets per second; the board halted its stream and stopped answering
discovery until power-cycled.

**Consequence.** Continuous gestures, not discrete actions. And if a commit
message needs the sentence "this has not been verified against X", verify X or
do not ship that line.

### 1.8 Meters are not trustworthy until something has checked them

The transmit stages originally inferred "no RF" from a missing SWR reading —
which is really "no SWR reading". The two are the same statement only after the
meters have been validated.

**Consequence.** Phases run in dependency order — `tune → rx → tx → meters` —
and meters come **last** precisely because nothing earlier may lean on them.

### 1.9 A measurement that looks in the wrong place reads as absence

The sideband stage hardcoded 24 kHz while the capture tap ran at the device's
48 kHz. It reported −80 to −109 dB for every mode while the RMS plainly showed a
25 dB signal — i.e. "no signal" rather than "I am misconfigured".

**Consequence.** Derive measurement parameters from the data (each capture
chunk reports its own rate) and **report them**, so the number is auditable.

### 1.10 An overloaded instrument must decline to answer

Monitoring our own transmitter saturates the receiver. Both sidebands then read
alike, and the stage confidently reported `SIDEBAND LOOKS INVERTED` — a false
alarm produced by the tool built to prevent them.

**Consequence.** Detect saturation and return INCONCLUSIVE. A diagnostic's worst
failure is a confident wrong answer, because it sends the next agent somewhere
specific.

### 1.11 Stale values answer a different question

`MeterModel` keeps last-known readings and never clears them, so "is there
forward power now" was answered by the SWR left over from the previous keyed
stage — reported as a carrier while the transmitter sent silence.

**Consequence.** Every meter read carries an age, and stages ignore anything
older than 3 s.

### 1.12 "No audio" has to mean it

The carrier-suppression stage keyed without enabling the test tone — but the
microphone was live, and the ALC's whole job is to lift a quiet room to full
modulation.

**Consequence.** Silence the source, do not merely stop driving it.

---

## 2. Next steps

### 2.1 The radio profile — highest leverage

Have `radiocert` emit the invariants it **established**, as a machine-readable
block, and verify them on later runs:

```
wire handedness      : conjugate-of-analytic
demod consumes       : raw wire
spectrum consumes    : conjugated
slice shift sign     : slice - NCO
TX IQ                : conjugated for wire
audio rate / IQ rate : 24000 / 48000
mode map             : {usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty} → all mapped
```

**Why this matters more than another stage.** Right now every one of those facts
is prose spread across `HERMES.md`. As a profile they become a checklist a new
backend fills in, and a regression in any of them is a **diff** rather than a
debugging session. Bring-up stops being exploration and becomes "determine these
seven facts, then run the cert".

Design notes:
- The profile is per-backend and lives with the backend, not the tool.
- `radiocert` reports **measured** against **declared** and flags disagreement.
  A backend that declares the wrong handedness and behaves consistently with its
  declaration is still wrong, but it is wrong *visibly*.
- Include the rates: the 24/48 kHz mismatch cost a whole measurement (§1.9).

### 2.2 Automate `consumer-agreement`

**The most valuable stage in the tool, and the one that cannot run.** It compares
where the panadapter draws a signal against which sideband recovers it — the
check that actually found the receive inversion, because the panadapter was the
only consumer with no compensating error.

Blocked on: the panadapter's bins are not reachable through the seam. The
spectrum arrives as `spectrumFrameReady(int, QByteArray)` into `RadioModel`, but
nothing exposes a snapshot a diagnostic can correlate against demodulated audio.

Sketch:
- A read-only spectrum snapshot accessor (last frame, with its centre and span).
- Park a known carrier **off-centre**, capture both the bin index of the peak and
  the demodulated audio frequency, and assert they describe the same side.
- Report the two independently, so a disagreement names which consumer is
  compensating rather than just failing.

Until then it is emitted as an **operator check** in `manualChecks`, deliberately
visible rather than quietly skipped.

### 2.3 Smaller, already identified

| Item | Note |
|---|---|
| `TX:FWDPWR` / `TX:REFPWR` defined but never fed | two power meters that can never move; publish with a documented scale or stop defining them |
| `SliceModel::setRfGain` has no runtime path | LNA gain is connect-parameters only; the preamp control does nothing after connect |
| `TX:ALC` computed and discarded | `Hl2TxDsp` emits `alcGain`; nothing consumes it |
| Tune power not separable from RF power | TUNE keys at full drive on a fresh connect |
| `RTTY` unmapped | silently demodulated as USB; conventionally lower-sideband on HF, so it wants a decision rather than a default |
| Sideband stage saturates | even at 5 % drive into a dummy load a few inches away; needs inline attenuation or a second receiver |
| Wire-convention stimulus harness (§1.4) | inject synthetic IQ at the backend boundary; needs no radio and would have caught the receive inversion |

---

## 3. Using this on a new radio

1. **`radiocert tune`** — no permission needed. Dial goes where told; every mode
   the app can emit survives a round trip.
2. **`radiocert rx`** — no permission needed. Establish wire handedness at
   **zero shift**, off-centre, in all four SSB modes, against a known carrier.
   WWV is the default reference: free, always on, exactly known, and not us.
3. **`radiocert tx`** — keys. Modulation, sideband, lifecycle.
4. **`radiocert meters`** — keys. The instruments, against stimuli with known
   answers.

Read the `concern` fields first, then the measurements. The tool does not pass
or fail: thresholds meaningful for one radio are guesses for the next, and a
tool that prints PASS for hardware nobody has characterised is worse than one
that prints the numbers.

**Then do the manual checks it lists.** They are the ones no amount of internal
measurement can replace, and skipping them is how this project shipped a
transmitter on the wrong sideband with every test green.
