# ANAN / OpenHPSDR board-support matrix

Status: implementation planning and guardrails for `anan-openhpsdr-all-models`.
This document separates **recognition** from **safe connection**. A discovery
board ID is not, by itself, evidence that the current packet builder is valid
for that board.

## Wire IDs and implementation state

| Protocol 2 board ID | Thetis physical family / examples | Evidence-backed distinction | AetherSDR status |
|---:|---|---|---|
| 0 | Atlas / Metis | Legacy OpenHPSDR family; not an ANAN SKU | Recognized in the registry; not connectable through this ANAN profile |
| 1 | Hermes — ANAN-10 / ANAN-100 | Hermes-class P2 DDC assignment; 1-ADC family in NereusSDR's Thetis-derived profile | Experimental P2 RX profile; hardware validation pending |
| 2 | Hermes II — ANAN-10E / ANAN-100B | Separate Thetis board ID; Hermes-class P2 DDC assignment | Experimental P2 RX profile; hardware validation pending |
| 3 | Angelia — ANAN-100D | 2-ADC family; must not inherit Hermes-class DDC placement | Experimental P2 RX profile; hardware validation pending |
| 4 | Orion — ANAN-200D | 2-ADC family; P2 ID differs from its Protocol 1 board ID | Experimental P2 RX profile; hardware validation pending |
| 5 | Orion MkII family — ANAN-7000DLE / 8000DLE, Anvelina Pro 3, Red Pitaya | Thetis uses one physical ID for several logical SKUs; MkII BPF1/preselector behavior differs from legacy ALEX HPF assumptions | P2 RX confirmed on ANAN-7000DLE; other shared-ID hardware pending |
| 6 | Hermes Lite 2 | Distinct board and protocol implementation; do not assume it shares ANAN ALEX or attenuator behavior | Recognized; not connectable through this P2 profile |
| 10 | Saturn — ANAN-G2 / G2 1K | Saturn-specific P2 profile; phase-word tuning and BPF1 behavior | **Bench-qualified RX profile**; the original single-DDC path is bench-qualified; multi-receive awaits hardware checks |
| 11 | SaturnMKII enum label; hardware identity unverified | Thetis declares the name with a question-mark comment; public Saturn p2app emits 10 or compatibility ID 5 | Recognized; not connectable; [source audit](anan-saturn-id11-verification.md) |
| 20 | HermesC10 — ANAN-G2E | Single-ADC, Hermes-class four-DDC receive layout; shares the G2-family BPF/preselector routing but not the G2's dual-ADC capabilities | Experimental P2 RX profile; hardware validation pending |

The table is deliberately conservative. “Recognized” means the discovery parser
can label the reported family; it does not imply that the current
`buildDdcSpecific()`, `buildHighPriority()`, ADC selection, filter map, or
receiver-port demultiplexing is correct for it. Invalid ADC counts (anything other than 1 or 2) are rejected by
`P2Client::start()` before a session socket starts; they no longer silently
fall back to the two-ADC Saturn default.

## Protocol 1 IDs are not Protocol 2 IDs

Never reuse a board-ID switch across protocols without translating the enum.
Thetis' Protocol 1 model IDs differ from the Protocol 2 discovery IDs. In
particular, Protocol 2 ID 4 is Orion (ANAN-200D), while Protocol 2 ID 5 is the
Orion MkII family; a Protocol 1 ID 5 identifies Orion, and Protocol 1 ID 10
identifies Orion MkII. This is a wire-protocol distinction, not a naming choice.

## Protocol 1 receive implementation

| P1 wire ID | Normalized board ID | Family | Stream slots | RX1 slot | Status |
|---:|---:|---|---:|---:|---|
| 1 | 1 | Hermes | 4 | 0 | Implemented; hardware qualification pending |
| 2 | 2 | Hermes II | 2 | 0 | Implemented; hardware qualification pending |
| 4 | 3 | Angelia | 5 | 0 | Implemented; hardware qualification pending |
| 5 | 4 | Orion | 5 | 0 | Implemented; hardware qualification pending |
| 10 | 5 | Orion MkII | 5 | 0 | Implemented; hardware qualification pending |
| 20 | 20 | HermesC10 | 4 | 0 | Implemented; hardware qualification pending |

These standard profiles expose 48/96/192 ksps. An explicitly selected Red
Pitaya P1 profile also supports 384 ksps, with five slots and two ADCs; its
choice is stored per MAC. A shared discovery ID alone does not enable it.
HL2 P1 remains on its existing
backend. Atlas/Metis and unverified P1 identities are not advertised through
the ANAN path. See [P1 codec and transport](anan-p1-codec.md) for pinned
sources, receive-only boundaries and remaining logical-model overrides.

## Implemented receive profiles

Discovery still reports physical families rather than selecting a logical SKU.
The current target for board ID 5 is the operator-confirmed ANAN-7000DLE.
Other logical models sharing that ID are not independently hardware-qualified.

| Property | Saturn (10) | Orion MkII (5), ANAN-7000DLE target |
|---|---|---|
| Physical ADCs | 2 | 2 |
| Normal single RX physical DDC | DDC0 (retained bench path) | DDC2 (Thetis `UpdateDDCs` / `GetDDC`) |
| RX enable byte 7 | `0x01` | `0x04` |
| RX row | bytes 17–22 | bytes 29–34 |
| Tuning word | bytes 9–12 | bytes 17–20 |
| IQ source port | 1035 | 1037 |
| General ALEX enable byte 59 | 0 (retained bench path) | 3 (both words enabled) |
| General PA-disable byte 58 | 0 (retained bench path) | 1 |
| ADC0 filtering | Thetis MkII/Saturn BPF1 | Thetis MkII BPF1 plus frequency-selected RX LPF |
| Shipped Saturn droop correction | Enabled | Disabled; per-radio measured tables remain usable |

The transport sends only Discovery until the reply identifies an implemented
receive profile and matches any board ID selected by the picker. Profile
selection never uses the reported receiver count. Before session packets are
sent, the physical DDC offset and ADC count are checked against the resolved
configuration. Unsupported boards and mismatched identities fail closed.
The stop path sends no board-specific packet before profile validation.

The P2 client's stream indices remain logical: physical DDC2 is delivered as
logical RX0 to the existing DSP and link-up path. The physical offset applies
to initial RX configuration, all tuning/keepalive/stop packets, live rate
changes and IQ source-port demultiplexing. DDC0/1 remain disabled for normal
Orion MkII reception. The backend additionally supports shared-window software slices and independent
receive streams (up to five logical streams, with physical/profile/discovery
limits). No diversity, PureSignal or TX is enabled.

The Orion MkII LPF ranges come from Thetis `setup.designer.cs` defaults:
0–2.5, 2.500001–5, 5.000001–8, 8.000001–16.5, 16.500001–24,
24.000001–35.6 and 35.600001–61.44 MHz. Relay positions come from
`ChannelMaster/network.h`; frequency words use the 122.88 MHz clock in
`Console/HPSDR/NetworkIO.cs:Freq2PhaseWord`; the out-of-range fallback is the 6 m LPF,
as in `console.cs:setAlexLPF`. BPF1 uses the already sourced MkII/Saturn
map. ADC1 filtering still uses the existing explicit bypass option.

Protocol-byte tests and socket-free injected-transport tests cover the new
profile. They are not evidence of successful reception on real hardware.
The operator confirmed the native Windows build, ANAN-7000DLE reception
and corrected model label on 2026-10-14. The additional families remain
hardware-unqualified.

Thetis source verification for the physical P2 board-ID enum was performed
against commit `852bf0ef0b4f3886a13fc2846489aee16f361872`:
`Project Files/Source/ChannelMaster/network.h`, enum `HPSDRHW`, lines 414-425.
That enum explicitly assigns Atlas 0, Hermes 1, HermesII 2, Angelia 3, Orion 4,
OrionMKII 5, HermesLite 6, Saturn 10, SaturnMKII 11 and HermesC10 20. The
shared ID 5 comment lists several logical SKUs, so discovery alone cannot
distinguish them.

## Porting order and required evidence

1. **Hermes / Hermes II / HermesC10:** port the Hermes-class DDC assignment
   from Thetis' `Console/console.cs` (`GetDDC` / `UpdateDDCs`) and the
   corresponding NereusSDR `P2CodecHermes` behavior. Keep the single-ADC
   family separate from the 2-ADC layout.
2. **Angelia / Orion / Orion MkII:** implement the verified 2-ADC DDC and
   packet layout from Thetis' `ChannelMaster/network.c` and
   `Console/console.cs`; use the exact family-specific ALEX/preselector path.
   Do not infer a SKU from shared discovery ID 5.
3. **Hermes Lite 2, Atlas/Metis and Saturn MkII:** verify the protocol and
   exact hardware path independently before making any of these IDs
   connectable in this backend.
4. For each family, add packet-byte regression tests for discovery, DDC
   assignment, ADC count, frequency representation, ALEX/filter registers and
   stream-port routing before changing the discovery gate.
5. Build and run the ANAN protocol/settings tests, then test on hardware for
   each family. A passing test suite for Saturn does not certify another board.

## Source provenance

- Thetis physical board enums: [ChannelMaster/network.h](https://github.com/ramdor/Thetis/blob/master/Project%20Files/Source/ChannelMaster/network.h) and [Console/enums.cs](https://github.com/ramdor/Thetis/blob/master/Project%20Files/Source/Console/enums.cs).
- Thetis model-specific hardware behavior: [Console/clsHardwareSpecific.cs](https://github.com/ramdor/Thetis/blob/master/Project%20Files/Source/Console/clsHardwareSpecific.cs), [Console/console.cs](https://github.com/ramdor/Thetis/blob/master/Project%20Files/Source/Console/console.cs), and [ChannelMaster/network.c](https://github.com/ramdor/Thetis/blob/master/Project%20Files/Source/ChannelMaster/network.c).
- NereusSDR's Thetis-derived P2 family split: [P2CodecHermes.h](https://github.com/boydsoftprez/NereusSDR/blob/main/src/core/codec/P2CodecHermes.h), [P2CodecOrionMkII.h](https://github.com/boydsoftprez/NereusSDR/blob/main/src/core/codec/P2CodecOrionMkII.h), [P2CodecSaturn.cpp](https://github.com/boydsoftprez/NereusSDR/blob/main/src/core/codec/P2CodecSaturn.cpp), and [BoardCapabilities.cpp](https://github.com/boydsoftprez/NereusSDR/blob/main/src/core/BoardCapabilities.cpp).

These references are evidence for porting and test design, not permission to
copy code without checking each source file's licence and AetherSDR's
clean-room/provenance rules.

## Additional P2 profiles

| P2 ID | Family | ADCs | RX1 physical DDC | DDC limit | ADC0 RF filter | Speaker channels |
|---:|---|---:|---:|---:|---|---|
| 1 | Hermes (ANAN-10/100) | 1 | 0 | 4 | Legacy ALEX HPF + LPF | Swapped |
| 2 | Hermes II (ANAN-10E/100B) | 1 | 0 | 2 | Legacy ALEX HPF + LPF | Swapped |
| 3 | Angelia (ANAN-100D) | 2 | 2 | 4 | Legacy ALEX HPF + LPF | Normal |
| 4 | Orion (ANAN-200D) | 2 | 2 | 4 | Legacy ALEX HPF + LPF | Normal |
| 20 | HermesC10 (ANAN-G2E) | 1 | 0 | 4 | MkII BPF1 + LPF | Normal |

Source: Thetis `clsHardwareSpecific.cs`, `console.cs:GetDDC/UpdateDDCs`,
`setup.designer.cs` and `network.c`. The pinned Thetis commit remains the
one cited above. NereusSDR `P2CodecHermes.h` independently confirms the
Hermes/Hermes-II DDC split (blob `7cc9e6807ca4c5de35f216bbbb135bfdfeb53d0c`).
The speaker swap is Thetis' `lr_audio_swap` in the RX audio output path.

Legacy HPF ranges are 1.8–6.499999, 6.5–9.499999, 9.5–12.999999,
13–19.999999, 20–49.999999 and 50–61.44 MHz; outside them the bank
is bypassed. They must not inherit MkII BPF boundaries. Single-ADC
profiles leave ADC1 filter and attenuation bytes zero. Address-only
sessions derive their ADC count from the actual physical board profile;
picker-selected IDs must still match. Hermes II rejects DDC2/3.

Supply-rail telemetry remains restricted to the existing Orion MkII/Saturn
path: the 5 V slow-ADC scale is not generalized to 3.3 V families.
Saturn droop defaults are never applied to another family. Per-radio
measured tables remain usable.

P1 discovery and receive sessions now use a separate
[RX codec and transport](anan-p1-codec.md) behind the existing backend.
[Logical model selection](anan-model-profiles.md) now covers the full Thetis
model list, including ANAN-8000DLE, G2 1K, Anvelina Pro 3 and Red Pitaya P2.
The operator confirmed ANAN-7000DLE P2 and Red Pitaya P1/384 ksps with commit
`2ae00c58`; the additional model/protocol combinations still need hardware
qualification. Atlas/Metis, G2 P1 identity and planned Saturn MkII remain
unqualified. Hermes-Lite 2 remains on its existing P1 backend; HL2 P2 is
excluded at the operator's instruction.


## Operator receive-filter check, 2026-10-18

Anvelina P2 (reported Orion MkII) explicitly mapped as ANAN-200D: DH1KLM
confirms band changes now produce physical relay switching with `c27a720e`.
The initial HPF-bypass and Disable 6m LNA on RX checks also appear successful.
This is evidence for that DIY configuration only; native Anvelina filtering,
other models, P1, independently configured ADC1 ranges and OC wiring are not
qualified by this report.

### Receive attenuation and historical preamp presets

The analogue model override selects the ATT policy independently of the
physical digital board. Classic ALEX ADC0 profiles (Hermes and
ANAN-100/100B/100D/200D) combine a 0–31 dB ADC register with the ALEX
10/20 dB relays. Continuous attenuation above 31 dB uses both relays (30 dB)
and the remaining ADC attenuation. ADC1/RX2 has only its independent 0–31 dB
ADC register. ANAN-10/10E and MkII-family profiles have no classic ALEX ATT
extension. Automatic implemented legacy board profiles assume the existing
classic ALEX receive path.

The main UI's historical **Preamp** presets follow Thetis' attenuation modes:
classic 0/10/20/30 dB uses ALEX alone; 40/50 dB uses 20 dB at the ADC plus
20/30 dB ALEX. Other implemented profiles offer ADC-only 0/10/20/30 dB.
These positions do not advertise positive gain. The existing OperatingState
`rfGain` object retains the two ADC values plus `alexAttenuationDb`; omitted
ALEX defaults to zero. Unsupported restored ALEX stages are removed and the
combined value bounded to the supported ADC range before use.

Source authority, Thetis `852bf0ef0b4f3886a13fc2846489aee16f361872`:

- [console.cs](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/console.cs):
  `RX1PreampMode`, `RX1AttenuatorData`, `maxAtt`, `SetATT`.
- [netInterface.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/netInterface.c):
  `SetAlexAtten` refuses MkII; `SetADC1StepAttenData` owns the ADC register.
- [network.h](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/network.h):
  ALEX 20 dB bit 13 and 10 dB bit 14; MkII repurposes bit 14 for RX MASTER.
- [network.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/network.c):
  `CmdHighPriority`, ALEX bytes 1432–1435 and ADC bytes 1442/1443.
- [networkproto1.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/networkproto1.c):
  bank-zero C3 bits 0/1 for classic ATT; bank 0x14 C4 low five bits for ADC0.

Socket-free tests verify encoding and client state. These checks do not
qualify the physical attenuation or analogue gain calibration of every model.

### CTUN receive tuning

The ANAN extension `ctun.set` accepts a boolean for a validated connected
receiver; `hw.get` reports `ctun`, `ctunAvailable`, `ddcCenterHz` and
`sliceFrequencyHz`. CTUN defaults off for a new configuration; saved band views
can restore it after validation. It uses
the existing DSP shift and hardware frequency setters without adding a new
shared backend interface. Other radio families retain their tuning paths.

The DDC centre is separate from the slice dial. In-window tuning sends no
new hardware frequency intent. WDSP shift is `slice - centre - CW BFO`,
preserving the previous `-CW BFO` behavior with CTUN off. The IQ polarity in
Aether's raw-wire receive path requires this sign (HERMES.md §16.2); a
Thetis RXOsc expression cannot be transplanted independently of its IQ path. The usable window
is 92% of the sample rate, including passband and BFO margins. Leaving it
recentres on the dial. Range/zoom centre suggestions are observed without
moving the CTUN anchor; operator Drag requests may move it if the receiver
still fits. Limits use the old rate during rebuild and the new rate after
settling. Physical ALEX/LPF/OC selection follows the DDC centre. Droop sweeps
require CTUN off, and cannot be used to enable CTUN mid-measurement.

Reference: Thetis `852bf0ef0b4f3886a13fc2846489aee16f361872`,
[console.cs](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/console.cs),
`chkFWCATU_CheckedChanged`, RX1 tuning around lines 31339–31440 (RXOsc,
92% sample-area limits and filter margins), and lines 31824–31844 (fixed
RX1DDSFreq under CTUN). `UpdateRX1DDSFreq`, `UpdateAlexRXFilter` and
`UpdateAlexTXFilter` select RX filters from the DDS frequency during receive.
Aether's initial surface is Radio Info; Thetis'
optional zoom-to-band, linked RX2 CTUN and detailed edge-scroll options are
not introduced by this single-receiver implementation.

### Regional spectrum band view

`BandPlanManager::amateurBandEnvelopes()` groups the active plan's segments
using the broad band catalogue solely as an identifier. Returned edges come
from that plan, without a US allocation fallback or licence-class truncation.
The spectrum paints cosmetic one-pixel danger-token lines in both software
and GPU frequency overlays; plan changes invalidate the cached overlay.
Channelized bands expose outer envelopes for display only.

The ANAN `band.center` extension accepts `lowHz` and `highHz`. It validates
the live session and band containment, refuses an active droop sweep, and
uses `receiveBandView()` to select a supported ceiling rate with 90% nominal
span for the band. The existing display crops to 92%. At the session maximum,
the whole-band receipt reflects actual 92% coverage. The centre is clamped
to retain the RX passband and CW offset without changing the dial. P1 uses
the effective profile's existing 192/384 ksps maximum; P2 tops out at 1536.

Band view enables session CTUN. During a live rate rebuild the requested
centre is held until the final requested rate settles, then applied through
the normal tuning path. Later tuning, dragging, span requests, CTUN edits,
failures and disconnect cancel the held centre. The extension result is an
acceptance receipt; ordinary pan state publishes the applied centre/span.
Flex's B/S flags are unchanged and no shared capability is misadvertised.

Deferred operator requests remain deferred: ASIO audio support, installer
packaging and expansion of droop correction to all models.

Band-view display correction: hardware capture rate and displayed span are
separate. A full-band view reports `(band width × 1.02) / 0.92` nominal
pan span, bounded to the actual capture, accounting for the shared widget's
4% crop per edge. A clamped centre uses the farther band edge for the span.
Backend spectra are centrally resampled to that same nominal span before
the neutral pan feed, so trace and waterfall retain their frequency mapping.
Bin count and absolute dB values are preserved; this interpolation adds no
spectral resolution. Droop receives the full spectrum before display cropping.
The displayed centre/span land together after a rate change. Partial-band
views retain the maximum full capture; manual span changes, CTUN off and
tuning outside the displayed window release the display crop.

### CTUN band memory and B toggle

ANAN B now toggles the fitted view and its saved pre-B span. Disabling through
B forces CTUN off, preserves the current VFO and recentres the normal view at
that receiver. The saved capture rate is requested through the existing queued
rate-change path, so a second press supersedes a pending fit.

`band.select` is an explicit band-button recall intent. It snapshots the source
band and restores the target's VFO, CTUN, DDC centre, rate, display span, mode
and passband. First visits retain the ordinary band defaults. GEN/WWV use
their normal frequency/mode defaults after snapshotting the source band, and
other backends keep their existing UI path. Typed frequency changes honour the entered
frequency and do not substitute a remembered dial.

OperatingState's domain-gated `ext.tuning` owns the entire version-1 view-memory
object. RadioStateMemory remains the sole disk reader/writer and uses its normal
debounce and shutdown flush. The object carries a physical radio serial and
profile keys `(logical model, physical board, ADC, override flag, protocol)`.
Unknown schemas are preserved without editing; malformed snapshots and unsafe
rates/frequencies are refused. Each band's record contains its live view and
pre-B snapshot. Capture reads the current settled view, including a final VFO
edit at disconnect. Empty/same-family restores reset the memory owner. A validated
connection restores the last exact-profile view unless given an explicit RX
frequency; an unseen mapping starts independently of another profile's RF state.

Band-button recall and reconnect both restore the latest live view, including
CTUN off. Older active CTUN snapshots remain compatible with the stored schema
but never override the latest state. This follows Thetis' current-state handling
in `OnCTUNChanged` and `SetBand`: a saved false value remains false on recall.
