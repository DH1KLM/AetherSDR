# Hermes-Lite 2 Bring-Up — Field Notes

Working notes from the HL2 receive bring-up on `feat/hl2-backend` (2026-07-24,
macOS 26.5.2 / arm64). Written to be *studied*, not just read: the last section
turns what happened into a proposed automated bring-up sequence.

Status at the end of the session: HL2 receives, tunes, and demodulates AM and
SSB with correct pitch on live hardware. Seven commits, `e556ad01..74f10f53`.

---

## 1. What makes HL2 different, and why it broke things

Flex hardware demodulates and ships **cooked audio + a hardware spectrum**.
HL2 ships **raw IQ and nothing else**, so the backend owns an engine-side WDSP
chain. It is the first backend to exercise that branch of the seam.

Almost every defect found in this session traces to one of two root shapes:

| Shape | Consequence |
|---|---|
| Code assumes a Flex-only object exists | Null deref, or a silently dropped intent |
| Code assumes Flex firmware will interpret a value | We hand the raw value to WDSP, which has different conventions |

That is the lens to bring to the *next* non-Flex backend. Neither shape is
visible from the interface; both are only visible at runtime.

---

## 2. The single most important lesson

**The decisive bug was found by reading reference implementations, not by
measuring.**

`Hl2RxDsp` opened its WDSP channel with `dsp_rate` = the 24 kHz audio rate.
WDSP's RXA stages are built around a 48 kHz internal rate. Both reference
clients hold it there unconditionally:

```c
// Thetis — Project Files/Source/ChannelMaster/cmaster.c, create_rcvr()
OpenChannel(chid, xcm_insize, 4096, xcm_inrate,
            48000,             // dsp rate — literal
            rcvr.ch_outrate,   // output rate — independent
            ...);

// pihpsdr — receiver.c
OpenChannel(rx->id, rx->buffer_size, rx->fft_size, rx->sample_rate,
            48000,             // dsp rate
            48000,             // output rate
            ...);
```

Neither derives `dsp_rate` from input or output rate. We did.

Why measurement never found it: `dsp_rate = 24000` is **not wrong in
isolation**. It passes `validateConfig()`, it is internally consistent, the
frame arithmetic balances (1024 in @48k → 512 out @24k), and the delivered
frame rate measured 23,936/s against 24,000 nominal — correct. It is only
wrong against a convention that exists solely in the reference clients.

Effect of the fix, identical capture conditions:

| | Before | After |
|---|---|---|
| Peak sample | 1.779 (5 dB over FS) | **0.1433** |
| RMS | 0.1209 | 0.0353 |
| 93.75 Hz comb + harmonics | strong | **gone** |

Time cost of not doing this first: roughly four rounds of measurement and two
wrong hypotheses (below).

---

## 3. Wrong turns, and what each one cost

Recording these because an automated process should be designed to make them
cheap or impossible.

| Hypothesis | Why it looked right | How it died |
|---|---|---|
| macOS broadcast discovery is broken | Python `sendto` to `255.255.255.255` → `OSError 65` with two interfaces up | Qt's in-app sweep works fine. **Tested before "fixing".** |
| `dsp_size` mismatch causes the warble | Autocorrelation showed peaks at every multiple of 1024 | Peaks were local maxima on a smoothly decaying autocorrelation — any continuous audio does that. r was 0.610 before, 0.700 after. |
| Spectrum is I/Q-inverted | Sim tones landed at negative offsets | Sim builds `I=sin, Q=cos`; `sin θ + j cos θ = j·e^(−jθ)` is negative-frequency *by construction*. Our decode was right. |
| Clipping masks the tones | Peak 2.64, 10% of samples at FS | Comb survived with AGC fully off. |
| Half of each 512-frame block is stale | Would explain both comb and 2× stretch | Correlation between block halves = 0.048. Not a repeat. |
| AM filter is the pitch bug | AM really does get an SSB passband (real bug!) | Operator reported USB *also* low-pitched. |

**Pattern:** four of six died on a cheap measurement that took minutes. The
expensive part was never the test — it was choosing which test to run. A
reference-comparison step up front would have skipped all of them.

---

## 4. Protocol facts (HPSDR Protocol 1 / Metis)

### The C&C bank we were missing

`MetisClient` sent three banks: config `0x00`, RX1 frequency `0x04`, LNA gain
`0x14`. Protocol 1 also defines **`C0=0x1C`** (address `0x0e`) — per-receiver
ADC assignment: C1 holds RX1–4 (2 bits each, LSB first), C2 holds RX5–7, C3
bits[4:0] TX attenuation.

The HL2 has one ADC and works without it. A conforming multi-ADC device leaves
every receiver **unassigned** and emits:

> correctly framed, correctly sequenced, correctly paced, **all-zero** IQ

This is the nastiest failure mode encountered all session, because every health
signal reads nominal — packet count, sequence continuity, sample rate, 0.00%
loss — and only the sample *values* give it away. Both AetherSDR and the
Phase-0 Python spike had this bug; neither could have found it on HL2 hardware.

**Automation requirement:** a data-plane health check must assert on sample
statistics (RMS, peak, non-zero fraction), never only on packet counts.

### Measured wire behaviour (48 kHz, against hpsdrsim)

| Quantity | Measured | Expected |
|---|---|---|
| IQ sample rate | 47,974/s | 48,000 |
| EP6 payload | 126 samples/packet | 126 |
| Inter-arrival mean | 2.625 ms | 2.625 ms |
| Inter-arrival p50 / p99 / max | 2.615 / 3.25 / 6.08 ms | — |

The p99/max figures are the real input for sizing the SPSC queue between the
UDP thread and DSP: it needs ≥3 packets of slack to absorb observed jitter.

### Ordering

A stream started before any C&C frame has landed emits ADC-idle samples. Prime
with C&C **before** `metis-start`. (The earlier `CONFIG_MERCURY` diagnosis was
wrong — HL2 gateware never decodes that bit; ordering was the real cause. Both
the design note and `prototypes/hl2/README.md` carry the correction.)

---

## 5. WDSP configuration facts

```
in_size   = 1024                    complex samples per fexchange2 call, at in_rate
dsp_size  = in_size * dsp_rate / in_rate     → 1024 @48k, 512/256/128 @96/192/384k
in_rate   = HL2 IQ rate             48/96/192/384 kHz
dsp_rate  = 48000                   CONSTANT. Not the input rate. Not the audio rate.
out_rate  = 24000                   AudioEngine::DEFAULT_SAMPLE_RATE
out_size  = in_size / (in_rate/out_rate)  → 512 frames
```

From WDSP's own `channel.c:40-52`:

```c
dsp_insize  = dsp_size * (in_rate  / dsp_rate);
dsp_outsize = dsp_size * (out_rate / dsp_rate);
out_size    = in_size  / (in_rate  / out_rate);
```

Note `out_size` depends **only** on `in_size` and the input/output rates. It is
independent of `dsp_size`, so `dsp_size` can never affect pitch — useful for
ruling things out quickly.

`validateConfig()` checks rate divisibility and the output-block arithmetic but
**not** the `dsp_size`/`dsp_rate` relationship, which is how a bad value passed.

### AGC

- `SetRXAAGCTop` is the **maximum gain in dB**, and 120 dB is the top of WDSP's
  range. Inheriting that default ran the HL2 wide open: peak 3.186, **10.31% of
  samples at or beyond full scale**. At a 65 dB ceiling: peak 2.664, 0.27%.
- Mode vocabulary: `off/slow/med/fast` → WDSP RXA 0/2/3/4. WDSP's "long" (1)
  has no representation in the four-way UI control.

---

## 6. Seam gaps found (the reusable checklist)

Each of these is "a Flex assumption that a DSP-owning backend violates".

| # | Gap | Symptom | Fix |
|---|---|---|---|
| 1 | `RadioModel::m_panStream` only assigned in the Flex `dynamic_cast` branch (`RadioModel.cpp:443`) | `startDax()` deref'd null → **SIGSEGV 3 s after every connect** | Guard at `startDax()` entry (`e556ad01`) |
| 2 | Missing ADC-assign C&C bank | All-zero IQ on conforming devices | `5c6c2fdd` |
| 3 | AGC never reached the backend | **Dead slider** — UI moved, DSP unchanged | `4d2bc494` |
| 4 | `dsp_rate` derived from audio rate | Low-pitched, warbling audio | `74f10f53` |
| 5 | Mode change mirrors the passband in the model **without** emitting operator intent | Model and DSP silently diverge | *Open* — `slice filter` verb works around it |
| 6 | AM is in neither filter-polarity family (`SliceModel.cpp:47-57`) | AM gets an SSB passband that excludes the carrier | *Open* |
| 7 | No pan-geometry down-verb on `IRadioBackend` | Zoom/pan can't reach the backend; waterfall and pan disagree | *Open* — structural |
| 8 | Slice frequency **is** pan center (`Hl2Backend.cpp:165`) | Click-to-tune recenters the world instead of landing | *Open* — needs slice-offset-within-passband |
| 9 | Same null-deref shape in the RADE path (`MainWindow_DigitalModes.cpp:461`) | Will crash HL2 whenever RADE starts | *Open* |
| 10 | `AETHER_AUTOMATION_NO_AUTOCONNECT` appears not to suppress autoconnect on the HL2 path | Test instance grabs a radio | *Open* |
| 11 | `SpectrumWidget` **drops** inbound pan geometry during a gesture, assuming another status is coming | View parks at the old centre while slice/pan/waterfall move — measured **permanently 6.3 kHz** out after one drag-tune | `3d52d07d` |

| 12 | Slice frequency WAS the DDC NCO, so the pan centre tracked every tune | Display re-centred on every click; a slice offset from centre was unrepresentable | `a1cbe154` |
| 13 | RX filter set via `SetRXABandpassFreqs` alone, leaving the NBP stage — the filter actually in circuit — untouched | No sideband selection and no filtering AT ALL; 0 dB rejection of a tone outside the passband | `86a3d27b` |
| 14 | HPSDR wire IQ handedness is opposite to WDSP's | USB demodulated the lower sideband and LSB the upper — audibly swapped, while the panadapter looked correct | `79c54266` |
| 15 | AM in neither filter-polarity family | Switching to AM kept an SSB passband that filters the carrier OUT, so the envelope detector distorts rather than going quiet | `2996f0eb` |

**Gap 13 is the second instance of the §2 lesson** — a plausible low-level API
used where both reference clients use the canonical composite one
(`RXASetPassband`). Neither call is wrong in isolation. Add to the Phase-0
reference diff: *for every vendor call we make, check whether the references use
a higher-level wrapper instead* — a wrapper usually exists because it sets more
than one stage.

**Gap 14 hid behind gap 13.** Until something actually selected a sideband, USB
and LSB sounded equally wrong and the swap was indistinguishable from general
breakage. Fixing the filter is what made it measurable. Expect this ordering:
some defects are only observable once a more basic one is repaired.

**Gap 11 is the most transferable lesson in this file.** The suppression is
correct — an echo arriving mid-drag is stale. It was *safe* only because Flex
re-echoes pan status continuously, so a dropped value is replaced within
milliseconds. That assumption is nowhere in the code. A backend that publishes
geometry only when it **changes** (the HL2 emits its pan centre from the RX NCO,
once, on tune) loses it forever.

Generalised rule, worth applying to every inbound path when adding a backend:

> **Ask whether each producer is level-triggered (re-asserts state) or
> edge-triggered (announces changes). Any code that drops an update "because
> another will arrive" is only correct for the first kind.**

The fix is the inbound half of #4142's "defer, never drop" — but re-read the
*model* on release rather than replaying the suppressed value, or you resurrect
the stale echo the suppression existed to reject.

**Principle II trap (hit twice):** `agcModeChanged`/`agcThresholdChanged` and
`filterChanged` are emitted from *both* operator setters and status
application. Driving a backend command off them echoes the radio's own state
back at it as a request. Operator-only intent signals are required —
`frequencyCommandIssued`, `filterCommandIssued`, and now `agcCommandIssued`.

---

## 7. The test fixture: hpsdrsim

Built from `g0orx/pihpsdr` and kept **outside** the AetherSDR tree at
`/Users/patj/aether/tools-external/pihpsdr` (GPL-3; behavioural reference only,
no code incorporated).

```bash
make hpsdrsim
./hpsdrsim -hermeslite2 -P1
```

Appears as serial `AA:BB:CC:DD:88:FF` (the `88` is its `-hermeslite2` MAC
byte), distinguishable from the real HL2 (`00:1C:C0:A2:13:DD`, gateware 7.4,
192.168.1.21).

### What it gives you

- Broadband ADC noise (amplitude 0.00003) plus two tones at **800 Hz and
  4000 Hz**, both at **−73 dBm** (= S9).
- Convention: **0 dBFS ≡ 0 dBm**. This is what let us confirm the dBFS→dBm
  constant, which the design note lists as an open question.

### Fixture gotchas — all cost time

1. Its header comment says "5000 Hz"; the actual phase increment
   (`0.016362461737… × 1536000 / 2π`) is **4000 Hz**. Trust the code.
2. Its tones are **negative-frequency by construction** (`I=sin, Q=cos`), so
   they only appear in **LSB**.
3. It **never models the receiver NCO** — tones sit at fixed baseband offsets
   regardless of tuning, so it cannot test frequency-offset behaviour.
4. `rx_adc[]` defaults to `-1` → all-zero IQ until `C0=0x1C` arrives.
5. Its C&C logging is **change-only**, so a reconnect can look silent. Restart
   the sim between test runs rather than trusting a quiet log.
6. It carries a strong **DC offset on I**. Any stage that translates frequency
   moves that spur too, where it impersonates the signal. Use a synthetic tone
   for sign/scale questions, not the simulator.
7. Stale instances hold UDP 1024. `pkill -f hpsdrsim` — note a `./hpsdrsim`
   invocation won't match a full-path pattern.

**Open question:** with everything correct, the sim's tones still don't resolve
in demodulated audio while the panadapter shows them ~55 dB above the floor.
Live audio is correct, so this is a fixture artifact — but understand it before
leaning on the sim for audio-path assertions.

---

## 8. Automation: what existed, what was added, what's still missing

### Added this session

| Verb | Why |
|---|---|
| `slice filter <lowHz> <highHz>` | Passband was unassertable, making every audio measurement untrustworthy |
| `slice agc <mode> [threshold]` | A control that can't be driven headlessly can't be regression-tested |
| `wheel <target> <x> <y> <steps>` | Of the four ways to move the VFO, the wheel was the only one with no verb — so the only one that could not be regression-tested |
| `wfRowLowMhz`/`wfRowHighMhz` + `wfCenterErrorHz` (state, not a verb) | Pan/waterfall alignment was eyeball-only; now it is a number |

**Reusable artifact:** `tools/tune_conformance.py` drives all four tuning modes
and asserts `slice == pan model == view == waterfall row` to 1 Hz after each.
Run it against any new backend before calling receive "done" — it is precisely
the check a new backend is most likely to fail, for the reason in gap 11.

Gotcha found while writing it: `SpectrumWidget` clamps the wheel to ±1 step per
event and debounces within 50 ms (#504/#556, inflated deltas on some desktops).
One synthetic event carrying five detents is **one** step, by design. Space
notches >50 ms apart or the test silently under-drives the control.

### Documentation drift cost real time

`slice mode` **already existed** but was absent from both the verb's own error
text and the docs table. Two separate detours into `dump_tree` and UI-clicking
resulted, on the belief that mode was undrivable.

**Requirement:** the verb's error text and the docs table must be generated
from one source. `gen_bridge_docs.py` tracks top-level verbs (53) but not
sub-actions, so action-level drift is invisible to CI.

### Still missing

1. **Read back what the DSP was actually configured with.** The recurring
   failure is model/DSP divergence (gaps 3, 5). `get_state` reports the *model*.
   An agent needs `get_state model=dsp backend=...` exposing the live WDSP
   config: in/dsp/out rates, block sizes, AGC mode + ceiling, filter edges.
   **This one verb would have caught gaps 3, 4 and 5 immediately.**
2. **A pitch/tone assertion primitive.** Every audio measurement this session
   was hand-rolled numpy over `capture_audio` JSON. A `capture_audio` mode
   returning dominant frequencies, peak/RMS, clipped-sample fraction and
   detected comb spacing would make audio regressions one call.
3. **Backend-vs-reference config diff.** See §9.
4. **Non-zero-sample assertion** in any data-plane health check.

---

## 9. Proposed automated bring-up sequence

Ordered by cost-to-run ascending, and deliberately front-loaded with the checks
that would have found this session's real bugs.

**Phase 0 — static, no hardware (seconds)**

1. **Reference-parameter diff.** For every vendor library we drive (WDSP
   first), diff our construction parameters against the reference clients'.
   Flag any parameter we *derive* that a reference *hardcodes* — that single
   rule catches `dsp_rate` (§2) and would have saved most of the session.
2. Assert `validateConfig()` covers every documented relationship, including
   `dsp_size`/`dsp_rate`.
3. Grep the new backend's call graph for Flex-only objects (`panStream()`,
   `connection()`, `m_flexBackend`) reachable without a null guard — catches
   gaps 1 and 9 statically.

**Phase 1 — against the simulator (a minute)**

4. Discovery → connect → assert `connected`.
5. Data-plane health: packet count, sequence continuity, **sample RMS/peak and
   non-zero fraction**, inter-arrival p50/p99/max.
6. Assert the DSP config read-back (§8.1) against expected values.
7. Drive every operator control through the bridge — mode, filter, AGC, tune —
   and after each, assert the **backend/DSP** state changed, not just the model.
   This is the dead-slider test, and it generalises to every future control.
7b. Run `tools/tune_conformance.py`: all four tuning modes, asserting
   `pan model == view == waterfall row` and that the slice lands where asked
   and stays inside the displayed span. Catches gaps 11 and 12, which are
   invisible to unit tests and nearly invisible by eye.
7c. Sweep any DSP stage whose SIGN or SCALE you are about to assume, against a
   SYNTHETIC source. `tests/hl2_shift_test.cpp` is the model: the same question
   measured against hpsdrsim was inconclusive because the simulator's DC offset
   translates with the shift and impersonates the signal. Reasoning about the
   direction got it backwards; one sweep settled it in seconds.
8. Audio assertions: inject a known tone, assert dominant frequency within
   tolerance, peak below full scale, no comb.

**Phase 2 — against hardware (minutes)**

9. Repeat 4–8 on the real radio.
10. Soak: run 10+ minutes, assert no drops, no growth in gap p99, no crash.
11. Operator sign-off on anything only ears or eyes can judge — audio quality,
    waterfall behaviour. Everything else should be machine-assertable.

**What must stay human:** whether audio *sounds* right. The pitch bug was
confirmed fixed by the operator's ears, and the AM filter bug surfaced from
"the audio sounds off". Step 8 narrows what needs listening; it does not
replace it.

---

## 10. Environment quick reference

```bash
# Build (8 cores)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j8

# Simulator
cd /Users/patj/aether/tools-external/pihpsdr && ./hpsdrsim -hermeslite2 -P1

# App with bridge, without grabbing a live radio
AETHER_AUTOMATION=1 AETHER_AUTOMATION_NO_AUTOCONNECT=1 \
AETHER_AUTOMATION_SOCKET=aethersdr-hl2 \
./build/AetherSDR.app/Contents/MacOS/AetherSDR
```

- Launch the app as the **foreground process of a backgrounded shell**;
  launching it with `&` inside a foreground command gets it killed with the
  shell's process group.
- First WDSP channel open costs **~17 s** generating FFTW wisdom; subsequent
  connects are **~110 ms**. Not a bug — don't "fix" it.
- The `prototypes/hl2/` Python spike defaults to broadcasting
  `255.255.255.255`, which fails on macOS with `OSError 65` when multiple
  interfaces are up. Use `--bcast <subnet>.255`. The in-app Qt sweep is fine.
