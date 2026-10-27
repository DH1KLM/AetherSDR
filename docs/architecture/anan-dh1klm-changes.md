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

The encoder and gate have no live transport caller. Both ANAN variants remain receive-only
in the application. TX DSP, packet pacing, model-specific transmit filters,
drive control, operation-fenced PTT/TUNE, stop/timeout handling and hardware
qualification remain to be implemented before enabling transmission.

## Verification

Validation is recorded separately in the local working state after the
Windows application build, targeted backend/discovery/Setup tests and user
documentation build. Hardware reception requires an operator check on the
corresponding radio; unit tests do not claim hardware qualification.
