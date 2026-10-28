# DH1KLM ANAN changes and Saturn implementation separation

Documentation inventory and requested integration contract, 2026-10-25.
The Saturn separation is implemented with independent executable code paths.
Original baseline: `c2a67af5bc4a2bae17302e36b4e77e14aedeb2b5`.

## Where the functional changes are documented

Functional changes belong in the user guide and the relevant technical
document. This page provides a central index; it does not replace those
documents or require a separate Markdown file for every small fix.

| Area | Existing documentation | Main implementation files |
|---|---|---|
| Supported boards, model selection, per-device mapping and explicit board override | [Model profiles](./anan-model-profiles.md), [board support matrix](./anan-board-support-matrix.md) | `src/core/backends/anan/AnanModel.h`, `AnanSettings.cpp`, `P2Protocol.h` |
| P1 receive codec and transport, protocol selection and qualification limits | [P1 codec](./anan-p1-codec.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/P1Protocol.cpp`, `P1Client.cpp`, `AnanClient.cpp` |
| Discovery identity, directed connection, saved profile and forgetting a mapping | [Model profiles](./anan-model-profiles.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/AnanDiscovery.cpp`, `AnanSettings.cpp`, `src/gui/ConnectionPanel.cpp` |
| ALEX filters, ADC routing, receive antennas and per-band antenna selection | [Model profiles](./anan-model-profiles.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/AnanFilterConfig.h`, `AnanReceiveFilterPlan.h`, `AnanBackend.cpp`, `src/gui/RadioSetupDialog_Anan.cpp` |
| Receive OC outputs, independent SWL matrix and manual digital outputs | [Model profiles](./anan-model-profiles.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/AnanFilterConfig.h`, `AnanSettings.cpp`, `P1Protocol.cpp`, `P2Protocol.cpp`, `src/gui/RadioSetupDialog_Anan.cpp` |
| Frequency and receive-level calibration | [Model profiles](./anan-model-profiles.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/AnanReceiveCalibration.h`, `AnanCalibrationOptions.h`, `AnanBackend.cpp` |
| CTUN, capture rate, band view and receive-window lifecycle | [Model profiles](./anan-model-profiles.md), [user guide](../user/docs/anan-g2.md) | `src/core/backends/anan/AnanModel.h`, `AnanReceiveLayout.h`, `AnanBackend_MultiRx.cpp` |
| Saturn board ID 11 qualification boundary | [Source audit](./anan-saturn-id11-verification.md), [board support matrix](./anan-board-support-matrix.md) | `src/core/backends/anan/P2Protocol.h` |

This inventory confirms documentation exists for these areas. It is not a
claim that every branch change has been audited against its documentation,
or that every supported hardware profile has passed a hardware test.

`docs/anan-working-state.md` is a local, ignored working agreement and status
record. It is not a tracked contributor or user reference and must not be
used as the sole documentation of functional changes. Code comments likewise
do not replace functional documentation.

## Requested Saturn choices

The operator requests two independently selectable implementations of the
same physical Saturn / ANAN G2 radio:

| Visible implementation name | Meaning |
|---|---|
| Saturn (ANAN G2) — tropo1234 | The original Saturn implementation from the selected `main` baseline, retaining its receive behavior and attribution. |
| Saturn (ANAN G2) — DH1KLM | The extended ANAN implementation maintained in this fork. |

These names identify implementation variants, not two physical radios, a
new firmware identity, or exclusive authorship of every file. Preserve the
original contributors' history, notices and licensing in both variants.

## Integration requirements

1. Pin the original implementation to a recorded `main` commit. The local
   `origin/main` and `upstream/main` ANAN trees compared identically during
   the initial inspection; the inspected upstream commit was
   `a2c70bb097ef54a142aeacf8cb83fe8981278c35`. Record the final chosen
   baseline again when implementing, rather than silently following a
   moving branch.
2. Retain an executable original backend, transport and protocol code path.
   Two labels selecting the same modified backend do not satisfy the
   requirement. Any namespace or integration adaptations to the original
   code must be recorded and reviewed separately from behavior changes.
3. Show both variants in discovery for a supported Saturn radio. Keep the
   physical MAC, IP, board ID and gateware version intact. Discovery row
   identity must include the implementation choice, so deduplication does
   not collapse the two rows. Update and expiry must handle both rows.
4. Offer both named variants under **Connect by IP > Receive profile**.
   The selected variant must reach backend construction, including on
   reconnect. An original Saturn selection must not inherit another
   model's override or silently select the extended implementation.
5. Identify the implementation in Setup and show its own applicable
   controls and values. Keep original Saturn options available through the
   original implementation. Do not present an extended-only control as
   functional on the original path.
6. Isolate implementation-owned configuration, operating state and
   calibration writes. Selecting or configuring one variant must not
   overwrite the other's settings. Preserve existing saved values and
   document any deliberate one-time migration. Keep the physical identity
   separate from the implementation-specific settings identity.
7. Persist the selected implementation for reconnect and automatic
   connection. A shared MAC alone cannot decide between the two variants.
8. Both variants target one physical radio: selection is sequential, not
   permission to start two competing streams to it. Preserve in-use
   rejection, bounded teardown and the RX-only transmit boundary.
9. Update the user guide with the final exact labels, behavior, persistence
   and limitations in the same change as implementation.

## Original source integration

`src/core/backends/anan_original/` retains the baseline backend, P2 transport,
protocol encoder/parser, DSP, spectrum analyzer and droop implementation.
Discovery is shared, but presents two selection identities; it never runs
an extra radio stream. Integration changes are restricted to include paths,
`anan_original` namespaces (including the originally outer-namespace droop
calibrator), unique C++ logging symbols, the requested comment prefix, and
an explicit Saturn/P2 connection guard. Original receive algorithms remain
unchanged. The original contributor history remains available at the pinned
Git commit and PR #5143; the labels identify variants rather than exclusive
authorship.

The general engine, model seam, WDSP library and station audio-output layer
remain shared. This is a separation of the ANAN implementations inside one
application, not two copies of the application. Client-wide audio-device and
ASIO settings remain client-wide; they are not radio firmware or ANAN-owned
calibration data.

`AnanImplementation` owns variant selection, backend construction and the
original option facade. Physical serials stay intact. The empty variant
selects DH1KLM and keeps existing settings. The `tropo1234` variant uses the
`AnanTropo1234` options object and `anan-tropo1234` radio-settings scope for
operating state and droop calibration. There is no automatic calibration or
operating-state migration between implementations. The per-MAC `Implementation`
feature remembers selection; its family-wide default is explicitly set from
Setup when no Saturn identity is available. Unknown variants fail closed.

## TX development branch

`codex/anan-tx-implementation` starts from the receive-qualified integration
state. The first TX building block is a socket-free P2 IQ encoder: a 32-bit
big-endian sequence followed by 240 signed 24-bit I/Q pairs, 1444 bytes in
total, for the default radio port 1029. Its conversion follows Thetis
`ChannelMaster/network.c` (`Outbound` and `WriteUDPFrame`), including rounding
half away from zero. Nereus `P2RadioConnection::composeTxIqFrame` confirms the
packet shape, component order and symmetric saturation range; its truncating
conversion is not used. Non-finite samples and incorrectly sized blocks are
rejected as a whole.

`AnanTxGate` supplies a transport-thread terminal admission building block.
It starts disabled and retains the original `TxCoordinator::Operation` for
every key and IQ write. The dispatch lease spans the synchronous terminal
callback; IQ also requires its captured media context. Enabling never keys,
and different producer fences cannot replace a live key even inside the same
coordinator operation. Cancellation and expiry permit only stop cleanup;
failed/ambiguous key writes block admission until an off write succeeds.
Session reset rejects old cleanup, and transport teardown clears the local
state with the gate disabled. No callback result is qualified radio-idle
evidence. The socket-free test exercises the real coordinator with injected
terminal writes, including deadlines, revocation and stale producer cleanup.

`AnanTxDsp` supplies the initial USB/LSB TXA path: 24 kHz client audio,
96 kHz internal DSP (Thetis `cmaster.c`), and 192 kHz P2 I/Q. Each exchange
consumes 128 audio samples and produces 1024 I/Q pairs. The signed 300–2700 Hz
passband selects the sideband; TXA output keeps HPSDR wire handedness. Exchanges
wait for readiness at the caller, not inside WDSP. Invalid input, expired media
and DSP errors retire the over. Explicit begin/end discards TXA's queued data
and filter history. A fixed 0.5 output scale reserves digital headroom;
non-finite or excessive internal output is refused rather than clipped. This
is not PA calibration or a completed microphone/ALC control surface.

`AnanP2TxStream` preserves samples across DSP block boundaries in a fixed
3840-sample queue (20 ms at 192 kHz). Injected monotonic deadlines space packets
by 1.25 ms. Each callback emits at most one packet; long stalls, overflow,
invalid samples and changed media provenance discard the queue. Underflow
emits a zero packet while preserving an incomplete sample group. Every write
passes through the TX gate with its captured operation and media context.
`P2Client` now owns the socket/timer adapter. Explicit configuration and enable
precede a separately authorized key request; neither configuration nor enable
keys the radio. A 1 ms precise timer polls authorization even before IQ arrives
and pumps sample-clocked packets once a media block starts. Unkey discards queued
IQ immediately, including release detected by the shared keepalive before a
replacement key arrives. Failed writes disable TX and attempt key-off; an unsuccessful
off tears down the transport with the hardware watchdog still enabled. A local
UDP write is never radio-idle evidence.

All live High-Priority senders share the admitted TX envelope. Thetis
`network.c` and `netInterface.c` define the DUC frequency, raw drive, antenna,
LPF, T/R relay and Alex1 status fields. TX uses its own frequency and LPF table;
RX-only LPF bypass and OC routing do not become TX settings. TX OC outputs are
explicit and default to zero. Alex1 mirrors T/R status without asserting the
Alex0 relay bit; MkII BPF2 grounding is independent of ADC count. Key-off restores
the latest complete RX state. TX-Specific sets the 192 ksps DUC rate and 31 dB
TX ADC attenuation, matching Thetis initialization.

`AnanClient` bridges prepared TXA channels and 24 kHz host-order int16 stereo
PCM. It averages the channels, preserves the original operation/media context
and origin, and exchanges one nonblocking 128-sample block per pump. The PCM
ring holds at most 2048 frames; P2 capacity applies backpressure before DSP
exchange. Rate/format errors, overflow, changed source and expired provenance
stop TX. Key-off reaches the transport before discarding DSP history. A new
over reuses plans without replaying partial PCM or a previous filter tail.
Channel construction remains the planning worker's responsibility; preparation
is not automatic during receive-only startup.

`AnanBackend` carries the canonical keying command and processed-audio seam to
that adapter on the existing I/O thread. It verifies protocol, model and prepared
mode, and limits queued PCM to 8192 bytes. Shared RAII credits return that budget
even when Qt discards a queued delivery, while connection changes replace the
budget ledger. Completion marks local queue consumption only, not radio idle.

The transport covers native board IDs 1–5, 10 and 20. Ambiguous IDs 5 and 10
require explicit model selection. ANAN-200D explicitly overridden on board ID 5
uses the Orion/legacy ALEX TX profile, matching native board ID 4. Other
cross-board receive overrides, Red Pitaya,
Anvelina Pro III and G2 1K have no TX admission. Receive support is unchanged.
Both desktop ANAN variants still advertise receive-only: operator-facing TX
preparation/admission, microphone gain, finite-stream drain, TUNE, calibrated
power control, P1 transmit and hardware qualification remain open. The original Saturn implementation is
unchanged. Tests inject the real client's terminal writes and clocks without
opening radio sockets.

The extended Setup also provides an operator PA-profile table and a calculation
preview, through `anan.txpower.*` extensions. One versioned `TransmitPower`
document per radio holds separate model/board/protocol entries. Requests carry
the current connection identity; stale identities, malformed values and active
TX operations refuse edits. No profile value grants transmit admission.

The reference gains follow local Thetis `DefaultPAGainsForBands`; every band
starts with a zero operator power limit. The preview clamps requested watts to
that limit and reproduces `SetPowerUsingTargetDBM`, `Audio.RadioVolume` and
`NetworkIO.SetOutputPower`, including the 1.02 factor and integer truncation.
A gain of 100 dB remains a gain, never a full-drive sentinel. Preview output is
not applied to the live transmitter. Actual PA calibration, modulation scaling
(including the TX DSP's fixed headroom) and measured RF power remain unqualified.
The UI includes only backend-neutral model/seam headers; it adds no wire path.
`txdsp.prepare` explicitly builds an `AnanTxDsp` on the existing
`anan-dsp-build` worker and stages it in `AnanClient` on the existing I/O thread.
Staging has no P2 configuration or enable side effect. A separate transport-
configured flag refuses keying even with a valid operation while only a staged
channel exists. The desktop still advertises receive-only.

Shared handles defer the final destruction of DSP resources to the planner,
including cancelled or dropped queued transfers. Completion carries only a
ticket and status to the backend. Cancellation is checked before and after
construction and before staging; mode and connection identities fence readiness.
Shutdown stops I/O first, drains planning producers, drains their late I/O
transfers and stops I/O again, then drains retirement before joining the planner.
No queued transfer can outlive its retirement context. Actual prepared TX paths,
failed preparation and replacement also use the same retirement ownership.

`AnanClient::setTransmitPower` converts an operator percentage into a fraction of
the selected band's configured watt limit, then applies the Thetis logarithmic
PA gain mapping. It updates P2 HP drive byte 345 only inside the existing keyed
operation. A foreign or expired operation cannot change drive; a disabled band
or invalid profile stops the owner's transmission. P2 reuses its guarded HP
writer, so the update retains the original dispatch lease and cannot key an
idle transmitter. This internal path is tested with native and overridden 200D
transport profiles; it is not yet wired to desktop transmit admission.
The original `anan_original` backend and its dependencies remain outside this work.

## Verification

Validation is recorded separately in the local working state after the
Windows application build, targeted backend/discovery/Setup tests and user
documentation build. Hardware reception requires an operator check on the
corresponding radio; unit tests do not claim hardware qualification.
