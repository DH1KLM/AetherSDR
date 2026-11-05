# OpenHPSDR Pure Signal integration

## Reference basis

The operational and display references inspected for this implementation are:

- Thetis-Ramdor `852bf0ef0b4f3886a13fc2846489aee16f361872`:
  `PSForm.cs`, `AmpView.cs`, `setup.cs` / `setup.Designer.cs` (two-tone settings),
  `console.cs` (`UpdateDDCs`, `GetDDC`),
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

## Reference amplitude domain

TXA output is multiplied by `AnanTxDsp::kWireAmplitudeGain` (0.5) before
serialization, retaining the existing 6 dB digital headroom. The digital
hardware monitor therefore also contains that gain. Before passing its TX
reference to CALCC, `feedPureSignalFeedback` divides it by the same constant;
the measured PA feedback is not scaled. This restores the full-TXA reference
domain expected by the hardware-specific SetPk and by IQC's input envelope.
It changes neither emitted TX samples nor operator drive, attenuation or SetPk.

CALCC divides the TX envelope by SetPk and needs all sixteen amplitude buckets,
including 0.9375–1.0. An uncompensated half-scale reference cannot fill the upper
buckets even while valid paired packets arrive. The native regression compares
both paths using identical sample fixtures: the raw path remains collecting;
the compensated path completes calculation and installs a real correction.
Raw spectra and sample-peak diagnostics retain the received digital levels.

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

Two-tone generation uses the existing TUNE admission and original request
media context. The existing I/O audio pump supplies bounded 700/1900 Hz PCM
blocks through TXA, at 0.49999 amplitude per tone, and retains the captured
operation/context through the terminal transport write. It does not use the
raw transport carrier. Two-tone support is declared only for prepared USB/LSB
sessions whose TX passband contains both tones. Auto-attenuation is opt-in. Each new valid RX-builder/calibration result can
request a 0–31 dB step towards the Thetis target level 152.293 (adjust at or below 128, or above 181). The host requests reset, waits for CALCC reset consumption and the
IQC END worker/ramp to drain, updates the TX-only ADC0 attenuator, waits 100 ms,
and restores the selected calibration mode. Key-off cancels the pending write;
a later over drains and resumes the paused calibration without applying it.
P1 carries the changed value on its existing IQ/control cycle; P2 sends its
mic-control byte 59 under the captured operation fence. Neither route keys TX.
Quick-response remains unavailable.

## Paired spectra and windowed distortion estimates

The optional analyser uses an 8192-point Hann-windowed complex FFT at the
paired stream's 192 kHz rate. I/Q is swapped like WDSP Spectrum0 for RF-oriented
frequency axes. Plans are created and destroyed on the existing DSP planner
under the double-precision FFTW planner lock; bounded execution runs on the
owning I/O thread at at most 10 frames per second. No new thread or dependency
is introduced. A failed optional FFT initialization does not block calibration.
A viewer request leases collection for one second; display data expires after
500 ms. Key-off, transport sequence gaps, invalid samples, and input gaps over
100 ms clear the window. An entirely new window must fill before publication.

IMD estimates require the locally admitted generated two-tone context, both
700/1900 Hz reference and feedback carriers, compatible sideband orientation,
and no possible sample clipping. Power is integrated across five bins per
carrier/product; the stronger lower/upper product is compared with the mean
power of one feedback carrier for IMD3/5/7/9. These are IMD-plus-noise estimates,
not a claim of RF power calibration or established PA correction performance.
Tests inject known -40/-50/-60/-70 dBc products on both RF sidebands and check
normalization, freshness, clipping rejection, and the original source context.

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
