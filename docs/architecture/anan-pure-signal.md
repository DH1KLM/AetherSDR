# OpenHPSDR Pure Signal integration

## Reference basis

The operational and display references inspected for this implementation are:

- Thetis-Ramdor `852bf0ef0b4f3886a13fc2846489aee16f361872`:
  `PSForm.cs`, `AmpView.cs`, `console.cs` (`UpdateDDCs`, `GetDDC`),
  ChannelMaster `network.c`, `networkproto1.c`, and `sync.c`.
- NereusSDR `4ce7ec89efcc36adc0723eabcd3465fdce5e1e37`:
  `PureSignal`, `PsForm`, `AmpViewChart`, `Ps3DisplayAdapter`,
  `P1CodecStandard`, and the PureSignal refresh design/verification notes.
- AetherSDR's bundled WDSP `calcc.c` and `iqc.c` are the execution authority.

The Qt window and backend adaptation are implemented for AetherSDR's model,
extension, TX-coordinator, theme, and planner ownership boundaries. Nereus's
additional native correction-stop and file-generation APIs are not present
in this WDSP build and must not be assumed to exist.

## Window and ownership

Tools → Pure Signal opens one independent workspace window. Its action row
follows the operator-selected Nereus arrangement. Calibration and AmpView
are views inside that window. Disconnect retires the presentation. Window
opening, view switching, and calibration arming do not request transmission.

Commands use the existing `anan` extension through RadioModel. Native DSP
access stays on the existing planner and ANAN I/O threads. Feedback samples
are borrowed only during direct delivery on that I/O thread. No additional
thread or external dependency is introduced.

## Supported feedback shapes

The initial feedback path requires a validated two-ADC OpenHPSDR device and
an already prepared TX session. Diversity must be disabled.

- P2 reserves synchronized DDC0/1 at 192 kHz: ADC0 PA feedback and digital
  TX reference selector 2. Independent receivers use DDC2 onward. Insufficient
  physical DDC capacity refuses the request.
- Five-slot P1 requires the existing receive rate to be 192 kHz and at most
  two ordinary receiver streams. During operator TX, slots 3/4 carry PA
  feedback and TX reference; the two regular receiver slots remain reserved.
- Other shapes are visibly unavailable with their reason. No undocumented
  single-ADC routing or automatic receive-rate change is inferred.

The existing TxCoordinator operation still gates keying and feedback delivery.
Feedback data never grants TX authority.

## Correction lifetime

Changing calibration mode or pressing OFF prepares a fresh TXA channel on
the existing planning worker, preserving the current mode, protocol output
rate and passband. Installation requires the original connection epoch,
TX-session permit, filter revision and an idle transmitter. Once installed,
the former channel cannot contribute another output block. Its deferred
CALCC/IQC work belongs exclusively to the retired channel and is torn down
on the existing planner. OFF never reports success from a caption change.

An uncorrected channel is retained while calibration is armed so disarming
the TX session restores an uncorrected path without allocating on the I/O
thread. Restoring an operational mode or enabled flag from settings is not
part of this implementation.

## Display data

WDSP owns sixteen 256-sample measurement buckets and 512 correction points.
Snapshots copy these under the existing WDSP locks. Measurement input is
`ym * x`, measured output is `x`, gain is `1 / ym`, and measured phase is
`atan2(ys, yc)` relative to the published phase reference. Magnitude and phase
corrections retain their independent published X arrays. Unavailable data
does not produce illustrative calibration curves.

## Remaining functional work

Two-tone generation through TXA, automatic attenuation and measured
feedback-spectrum integration remain unavailable with accessible reasons.

Save captures actual IQC coefficients; Restore validates a bounded v2 document
before native parsing and installs into an unpublished fresh channel. Apply
Current copies retained coefficients to that same kind of fresh target. The
native BEGIN ramp is preserved without an audio-dependent wait. No file
operation is queued on the asynchronous compatibility worker. Real IQC
run/busy flags distinguish a ready correction from a completed ramp. These
host extensions are documented as WDSP patch 24.

No hardware correction or IMD improvement is established by the transport
tests or by opening the window. Live RF verification needs explicit operator
authorization and a measured feedback setup.
