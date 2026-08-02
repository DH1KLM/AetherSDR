# Icom CI-V Backend — Design Note

Bring-up plan for `IcomCivBackend`, an `IRadioBackend` implementor for Icom
networked radios. First target: **IC-705 over WiFi**.

The protocol reference is the oracle at `~/oracles/icom/icom-oracle.md`, with
primary sources under `~/oracles/icom/sources/`. This note does not restate the
wire format; it covers what AetherSDR has to build and in what order.

Companion to `aetherd-hl2-backend-design.md`, and deliberately shaped like it.

---

## 1. Why Icom is a different kind of backend

The two backends we have bracket the design space, and Icom sits between them in
a way neither anticipated:

| | Flex | HL2 | **Icom** |
|---|---|---|---|
| Demodulation | radio | host | **radio** |
| FFT / spectrum | radio | host | **radio** |
| Raw IQ available | yes (DAX-IQ) | yes (it's all we get) | **no** |
| Command plane | text over TCP | registers in the IQ stream | **CI-V over UDP** |
| State push | full status subscription | telemetry in-band | **CI-V Transceive, partial** |
| Meters | VITA-49 stream | in-band telemetry | **polled, one at a time** |

Flex is *radio-authoritative and rich*: it tells you everything, unprompted.
HL2 is *host-authoritative and raw*: it tells you almost nothing and hands you
samples. Icom is **radio-authoritative and poor** — the radio owns the DSP, the
demodulation and the FFT, but its only way to tell you anything is a 1980s
request/response bus that also carries every command you send.

That combination produces the two structural facts this whole design turns on:

**(a) There is no IQ.** Not over WiFi, not over USB, not at all — confirmed
against Icom's own CI-V Reference Guide, wfview's type system, and the transport
itself (oracle §8.1). The panadapter is a cooked 475-bin, 0–160 display array at
one of eight fixed spans. Everything downstream of raw IQ is unavailable:
host-side FFT sizing, arbitrary zoom, DAX-IQ consumers, CW Skimmer.

The goal as written asks for "control/voice/data/IQ/panadapter". **Four of those
five are achievable and IQ is not.** The panadapter is real and good; the IQ line
item should be struck rather than shipped as a disabled control.

**(b) Meters are polled and share a stream with tuning.** There is no meter
plane. Every S-meter reading is a round trip on the same UDP stream carrying
frequency changes, and on WiFi that is 5–30 ms. This is the first backend where
*metering policy* is a real engineering constraint rather than a subscription.

---

## 2. What `IcomCivBackend` owns

```
src/core/backends/icom/
  IcomCivBackend.{h,cpp}      IRadioBackend implementor; owns the others
  IcomSession.{h,cpp}         RS-BA1 session: handshake, login, token renewal
  IcomStream.{h,cpp}          one UDP stream: seq, ARQ, keepalives (×3 instances)
  IcomSeqBuf.{h,cpp}          reorder + replay buffers (the ARQ layer)
  CivCodec.{h,cpp}            CI-V framing, BCD codecs, frame reassembly
  CivCommands.h               the command table (§4.3 of the oracle)
  IcomScope.{h,cpp}           0x27 waveform decode → spectrum frames
  IcomAudio.{h,cpp}           codec 4 LPCM, the 1364+556 split, resampling
  IcomMeters.{h,cpp}          calibration curves + the poll scheduler
  IcomModels.h                per-model capability table (IC-705 first)
```

`IcomStream` existing three times — control, serial, audio — with independent
sequence spaces and ARQ state is the shape the protocol dictates, and it is worth
resisting the urge to collapse it. kappanhang's `streamCommon` is exactly this
and it is the cleanest part of that codebase.

---

## 3. Seam mapping

### Intents DOWN

| `IRadioBackend` | Icom mechanism |
|---|---|
| `setSliceFrequency` | CI-V `05` (set operating frequency), 5-byte LE BCD |
| `setSliceMode` | CI-V `06` (mode + filter) |
| `setSliceFilter` | CI-V `1A 03` (IF filter width) — **discrete FIL1/2/3, not continuous** |
| `setSliceAgc` | CI-V `16 12` — FAST/MID/SLOW only; **no threshold**, ignore `thresholdDb` |
| `setPanCenter` | CI-V `05` in Center mode; `27 1E` edges in Fixed mode |
| `setPanBandwidth` | CI-V `27 15` — **snaps to one of eight spans**; report what was taken |
| `setPanRfGain` | CI-V `16 02` preamp — **three positions**, see §5 |
| `setPanFrameRate` | CI-V `27 1A` sweep speed 0/1/2 — mapping to Hz is unmeasured |
| `setKeying` | CI-V `1C 00` (00=RX, 01=TX) |
| `setTune` | **no direct command** — see below |
| `setTxPower` | CI-V `14 0A`, 0000–0255 BCD |
| `setTxFilter` | CI-V `1A 05 0020/0021/0022` — **discrete WIDE/MID/NAR**, not Hz |
| `submitTxAudio` | audio stream, codec 4 — **requires DATA MOD = WLAN** |
| `setSliceAudioGain` | CI-V `14 02` (AF level) |
| `createPanadapter` | `false` — one receiver, one scope |

Three of these do not fit the seam cleanly, and all three fit the same pattern:
**the verb is continuous and the radio is discrete.** `setSliceFilter`,
`setPanBandwidth` and `setTxFilter` all take Hz and can only snap. The seam
already anticipates this — `setPanBandwidth`'s own comment says "hz is a REQUEST"
and the result comes back via `panCenterBandwidthChanged`. Follow that contract
exactly: take the request, snap, report what happened. Do not clamp silently.

`setSliceAgc`'s `thresholdDb` has nowhere to go. Ignoring a parameter is the
right call over inventing a mapping, but it should be a documented no-op with a
comment, not a silent drop.

**`setTune` has no Icom command, and the obvious candidate is a different
feature.** `1C 01` is the *antenna tuner* status (`00`=OFF, `01`=ON, `02`=Tune) —
it starts an ATU matching cycle, not a tune carrier. AetherSDR's `setTune(on,
tunePowerPercent)` means "raise a steady carrier at the operator's tune power",
which on an Icom is composed rather than commanded: save the mode, set RTTY or
CW, apply the tune power via `14 0A`, key with `1C 00`, and restore on release.

Those are two genuinely different operations and they must not be conflated —
`1C 01 02` belongs on the tuner extension path (`TunerModel`'s autotune intent),
not on `setTune`. A backend that wires the ATU cycle to the TUNE button gives the
operator a button that does nothing on a radio with no ATU attached, and does
something unexpected on one with an AH-705.

### State UP

| Signal | Source |
|---|---|
| `sliceChanged` | CI-V Transceive pushes + polled `03`/`04` |
| `panCenterBandwidthChanged` | the `27 00` waveform header carries centre+span or edges |
| `spectrumFrameReady` | `27 00` waveform data, 475 bins → float (see §6) |
| `waterfallRowReady` | derived from the same frame |
| `audioFrameReady` | audio stream, decoded to the engine's PCM format |
| `sliceAudioFrameReady` | same buffer — one slice, so they are the same stream |
| `meterUpdate` | polled `15 xx` through the calibration curves |
| `transmitChanged` | `1C 00` echo + TX meters |
| `radioChanged` | `19 00` model, firmware, connection state |
| `linkStatsUpdated` | per-stream counters + `0x07` ping RTT |
| `healthSnapshot` | OVF (`15 07`), Vd, Id, retransmit and loss counters |

`sliceAudioFrameReady` and `audioFrameReady` carrying the same buffer is correct
here and worth a comment in the code — with one receiver there is nothing to
un-mix, and a future reader will wonder if it is a bug.

---

## 4. Capabilities `IcomCivBackend` advertises (IC-705)

```cpp
caps.family                 = "icom";
caps.model                  = "IC-705";        // from CI-V 19 00, never hardcoded
caps.maxSlices              = 1;
caps.maxPanadapters         = 1;
caps.tuningMinHz            = 30e3;
caps.tuningMaxHz            = 470e6;           // with gaps; see note
caps.canTransmit            = true;
caps.txPowerMaxWatts        = 10.0;
caps.hostModulates          = false;           // the radio modulates
caps.hasRadioSideDsp        = true;            // NR/NB/notch are 16 xx, in firmware
caps.hasTuner               = false;           // no INTERNAL ATU; see note
caps.hasSupplyVoltageTelemetry = true;         // 15 15 Vd
caps.hasDaxStreams          = false;           // NO IQ — see oracle §8.1
caps.hasGpsLocation         = false;           // GPS exists, protocol won't carry it
caps.hasProfiles            = false;
caps.hasWaveforms           = false;
caps.hasMultiClientSessions = false;
caps.hasRadioSideWaterfallAutoBlack = false;
caps.persistsMemories       = false;           // radio has 99; not phase 1 — see §8
caps.canReboot              = false;           // see note
caps.clientSettingsDomains  = {};              // radio remembers its own state
```

**`hasTuner = false` is a judgement call, not a fact.** The IC-705 has no
internal ATU, but `1C 01` controls an *external* AH-705 — and there is no command
to detect whether one is attached. So the capability is unanswerable from the
radio. False is the safer default (no tuner UI on a radio that probably has
none); an operator with an AH-705 is better served by an explicit setting than by
a control that appears unconditionally and silently fails.

**`canReboot = false` despite `18 00` / `18 01` existing.** Those turn the
transceiver off and on — but over WiFi, powering off drops the WLAN interface,
so the `18 01` that would bring it back has no path to reach the radio. The pair
is usable on a wired CI-V bus and is a one-way trip over the network. Advertising
a reboot the operator cannot recover from is worse than not offering it.

**`clientSettingsDomains` empty is the load-bearing line.** Unlike the HL2 — where
the radio reports no VFO and the app must be authoritative — an Icom remembers
its own frequency, mode and filter across power cycles and reports them on
request. Constitution II/III then says the client must not re-assert them. This
backend reads state at connect; it does not push a restored state.

`tuningMaxHz` is a simplification: the IC-705 covers 0.03–470 MHz with gaps
(no 148–430 receive on some regional variants). The seam has no gap
representation, so the honest thing is the outer envelope plus a rejected-tune
path that reports what the radio actually did.

---

## 5. The structural gaps Icom forces

### Gap A — RF gain has no continuous range

`setPanRfGain(panId, gainDb)` assumes a dB register. The IC-705 has a
three-position preamp (`16 02`: OFF / P.AMP1 / P.AMP2, and only OFF/ON on
144/430) and no continuous gain.

The seam already has the escape hatch: `panRfGainInfoChanged(panId, low, high,
step)`. Emit `(0, 2, 1)` and let the existing slider snap to three detents. That
is precisely what that signal was added for — the HL2 case in its comment is the
same shape (a Flex-derived range that did not match the hardware), and reusing it
avoids a per-family special case in the UI.

**Do not advertise a fabricated dB range.** A slider that moves smoothly over a
control with three positions is the "the control moves, the audio is unchanged"
failure the capability comments keep warning about.

### Gap B — metering is a scheduler, not a subscription

This is new. Flex streams meters; the HL2 embeds them. Icom needs a **poll
scheduler** that:

- polls only meters currently visible in the UI;
- runs S-meter at ~10 Hz and everything else at ~5 Hz;
- stops TX meters entirely while receiving, and RX meters while transmitting;
- yields to user-initiated commands, so tuning never queues behind metering;
- filters its own request/response traffic out of anything re-exported (CAT
  pass-through, TCI) — kappanhang does exactly this and it matters.

wfview's per-rig `Periodic\N\Command` list with priorities is the proven shape.
This should be a named component (`IcomMeters`) with its own test, not a timer
sprinkled through the backend.

### Gap C — the scope is not calibrated

`spectrumFrameReady` carries float dBm on the HL2 path. The Icom scope is 0–160
display units relative to the `27 19` reference level, and Icom publishes no
calibration.

Follow the `Hl2DbReference` precedent: a named per-model offset, documented as an
estimate, anchored to the reference level read back from the radio, and
cross-checked against the **S-meter**, which *is* calibrated (0 = S0, 120 = S9,
241 = S9+60 dB). Put a known signal in the passband and compare.

Until that cross-check exists, the UI should not present the scope's Y axis as
absolute dBm. An honest relative scale beats a number that looks like a
measurement and is not.

---

## 6. Phasing

Each phase is independently shippable and independently provable.

**Phase 0 — the socket.** `IcomStream` + `IcomSession`: handshake, login, token
renewal, keepalives, ARQ. No CI-V, no audio. Proof: connects to an IC-705, stays
connected for an hour, `linkStats` shows RTT and zero loss. This is the phase
where the protocol is either right or wrong, and it is testable against a
recorded packet trace with no radio attached.

**Phase 1 — control.** `CivCodec` + the command table: frequency, mode, filter,
PTT, power. Proof: the automation bridge tunes the radio and reads it back; the
front panel follows. This is the phase that makes the backend *useful*.

**Phase 2 — panadapter.** `IcomScope`: enable `27 10` **and** `27 11`, decode the
single-packet WLAN waveform, emit spectrum and waterfall. Proof: a screenshot
with a real signal at a known frequency landing in the right bin.

**Phase 3 — audio.** `IcomAudio`: codec 4 LPCM 48 k mono, RX first. Then TX,
which needs DATA MOD = WLAN on the radio and **verification outside the system** —
a second receiver or a WebSDR, per `feedback-verify-outside-the-system`. A TX
path that looks perfect from inside AetherSDR and is silent on the air is the
exact failure mode this project has already been bitten by.

**Phase 4 — meters and health.** `IcomMeters` with the poll scheduler and the
calibration curves. Proof: S-meter tracks a signal generator; Po meter tracks a
wattmeter into a dummy load.

**Phase 5 — breadth.** Model discovery via `19 00`, the per-model capability
table, and CI-V over a **local serial port** with the network layer bypassed.
That last one is a small increment and it brings in every non-networked Icom
(IC-7300 and up), which is the strongest argument for the `IcomCIV` name.

---

## 7. Clean-room provenance

**wfview is GPL-3.0 and AetherSDR cannot take code from it.** It is the best
reference available and it must be treated as a *specification*: read it, cite
it, do not paste it. This is the same rule the project already applies to
`pihpsdr` for the HL2 test fixture.

**kappanhang is MIT** and is the reference to port *from*, with attribution.

**Hamlib is LGPL-2.1.** Its meter calibration tables are data, but most of the
same curves are in Icom's own published guide — derive them from tier 1 and the
question does not arise.

Allowed inputs, in order: Icom's CI-V Reference Guides (facts, not text);
kappanhang (MIT, portable); wfview and Hamlib (read-only reference); packet
captures from our own radio.

---

## 8. Explicitly out of scope for phase 1

- **IQ.** It does not exist on this radio. Not deferred — absent.
- **Memory channels.** The radio stores 99 in 100 groups (`1A 00`) and the decode
  is large and fiddly. Ship `persistsMemories = false` (client-side bank) and
  revisit.
- **D-STAR / DV.** A large command surface (`22 xx`, `23 xx`) and a separate
  feature.
- **Bluetooth transport.** Unknown whether it carries all three streams.
- **Opus and ADPCM codecs.** LPCM first. They matter for WAN use, so this is a
  deferral rather than a dismissal.
- **USB transport.** Needs the 11-chunk scope reassembly the WLAN path avoids.
- **Multi-model support.** Build the seams for it (`19 00` discovery, a model
  table) and populate only the IC-705.

---

## 9. Open questions needing a radio on the bench

Carried from oracle §12, because they gate specific phases:

1. **Scope frame rate over WLAN** — gates whether `setPanFrameRate` does anything.
2. **Does the scope update during TX?** — gates the phase-2/3 interaction.
3. **The real dBm offset**, per band and preamp setting — gates Gap C.
4. **Are `27 15` span changes echoed** when set on the front panel, or must they
   be polled? — gates whether the pan follows the operator's own zoom.
5. **Second-client behaviour.** The protocol has `busy` and `computer` fields;
   the IC-705's single-session response to contention is untested.

Answering 1–4 needs perhaps an hour with the radio and a packet capture, and
would remove most of the guesswork from phases 2 and 3.
