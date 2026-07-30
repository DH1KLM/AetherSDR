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

### 1.13 Fixing a bug once does not fix it everywhere it lives

§1.9 was found and fixed in `stage-rx-sidebands`. Review found the *same* bug
still sitting in `stage-sideband` — the flagship stage, the one this tool is
named for — reading the *same* `output` tap through the *same* wrong constant.
The fix had been applied to the site where the symptom appeared rather than to
the class of defect.

It survived because of a second-order effect worth remembering on its own: the
saturation guard (§1.10) returned INCONCLUSIVE before the tone comparison was
ever reached, so the stage never got far enough to be wrong on the bench. A
guard that suppresses a symptom also suppresses the evidence. The first person
to add attenuation and get a usable measurement would have received a confident
`SIDEBAND LOOKS INVERTED` computed from noise.

**Consequence.** When a defect is found, grep for its *shape* — here, every
`DEFAULT_SAMPLE_RATE` against a device-rate tap — not just its site. The
measurement primitives were moved to `RadioCertificationMath.h` and given a test
that asserts the failure directly: a 48 kHz tone probed at an assumed 24 kHz
reads 228 dB down, while `rms()` reads *identically* at both rates. That last
number is the whole lesson — the rate-independent statistic is exactly the one
that let the bug hide.

### 1.14 A generic tool that hardcodes one radio's facts reports false defects

`radiocert` asserted "`setRfGain` has no runtime path" and applied a
Hermes-Lite-2 meter-expectation table to whatever backend was connected. On a
Flex, the first is a working control reported as broken and the second is a
clean bill of health for meters that were never checked for.

A hardcoded false finding is worse than a missing one: nothing distinguishes it
from a measured result, and the report's own docs tell readers to read
`concern` first.

**Consequence.** Radio-specific claims are gated on `RadioModel::family()` and
say so in the report when they are skipped. This is a stopgap — the real answer
is the radio profile in §2.1.

### 1.15 A diagnostic must leave the radio where it found it

Every stage that moved the dial restored it to the *option* frequency rather
than the operator's. `radiocert tune` — the one phase safe enough to need no TX
permission — silently relocated the VFO to 20 m and left it there. Mic gain and
drive level were both carefully saved and restored, which is what marked this as
an oversight rather than a decision.

Worse, in an `all` run the receive stages parked the dial on WWV and the
transmit stages never re-tuned, so **every keyed stage transmitted a few hundred
Hz off a standards station, out of band** — while the report's `radio` block
faithfully said 14.2 MHz throughout. That is §1.11 again: a value that answers a
different question than the one being asked.

**Consequence.** `run()` saves the operator's frequency and mode at entry and
restores both at exit; the transmit block re-establishes the dial before
anything keys. Restoration is RAII where a throw could strand hardware state.
The `meters` phase keys too, and needed the same treatment — a fix applied to
one keying block is not applied to the class of keying blocks (§1.13 again).

### 1.16 §1.1 recurs one level up: the check shared its subject's convention

The sideband stage was the answer to §1.1 — *a convention error is invisible to
any test that shares the convention* — and it shared one.

It compared the transmitted tone demodulated on the matching sideband against
the opposite one. But `Hl2Backend::setSliceMode` drives the transmit chain and
the receive chain together ("the transmit sideband follows the slice"), so both
legs were *matched pairs*: TX-USB/RX-USB and TX-LSB/RX-LSB. With the
transmitter correct, both legs recover the tone. With it inverted, both go
silent. **The difference between them was noise in either case**, so the
comparison could not discriminate anything — and the stage emitted a confident
`SIDEBAND LOOKS INVERTED` verdict from it.

What *is* observable is the absolute level in each leg, which answers a real
and narrower question: do the transmitter and the demodulator agree about which
side of the carrier a sideband is on. A transmit-only inversion silences both
legs. A **shared** inversion — both chains wrong in the same direction — remains
invisible by construction, which is precisely §1.1 restated one level up.

**Consequence.** The stage was rewritten to measure agreement, not correctness,
and renamed to say so. Its `observation` states what it cannot see, and the
external-receiver check stays in `manualChecks` as the only thing that settles
the absolute question. Writing a check for a class of error does not exempt the
check from that class.

### 1.17 A refused action reads as a broken subject

`TransmitModel::requestPttOn` returns `void` and silently does nothing when
`runPttPreflight` refuses — a band limit, an interlock. `radiocert` never
confirmed the radio actually keyed, so every downstream stage measured an
unkeyed radio and blamed whatever it happened to be testing: "audio never
reached the modulator", "the transmitter is not producing RF".

The same shape as §1.8: a diagnostic reporting a defect in the nearest
subsystem rather than the responsible one.

**Consequence.** `keyViaOperatorPath` returns whether the radio reached the
requested state, refusals are counted, and `keyRefusals` is a top-level report
field — a non-zero count invalidates the transmit stages rather than annotating
them.

Related, and found with it: with Quindar enabled, `requestPttOff` does not
unkey. It starts an outro tone and defers the real unkey behind a timer, so
"the call returned" and "the radio stopped transmitting" are different moments —
the watchdog was being disarmed while the radio still transmitted. And the
Quindar *intro* tone is transmitted as audio, landing inside
`stage-carrier-suppression`'s assertion that nothing is being sent (§1.12). The
diagnostic now silences Quindar for the run and waits for the actual unkey.

### 1.18 A probe needs a band when you do not control the target

`tonePower()` integrates coherently over the whole buffer, so a 1.5 s capture is
a ~0.67 Hz bin. That is right for our own test tone, whose frequency we set. It
is wrong for an off-air reference: WWV is exact, but *our dial* is not, and a
1 ppm oscillator error at 10 MHz moves the carrier ~10 Hz — fifteen bins away.

Measured in `radio_certification_math_test`: a 10 Hz drift reads **−240 dB** on
an exact-bin probe and **−12 dB** on a ±25 Hz band search. The exact-bin result
is indistinguishable from a deaf receiver, and — being the same symptom — would
have been read as the §1.9 wrong-rate bug all over again.

**Consequence.** `tonePowerNear()` searches a band whenever the tone's exact
frequency is not under our control, and the span is reported alongside the
result.

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
