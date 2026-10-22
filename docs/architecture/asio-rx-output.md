# Native Windows ASIO receive output

The existing AudioEngine mixer writes stereo float PCM through a QIODevice.
`AsioOutput` supplies that device on Windows when explicitly enabled, using
the existing AudioEngine owner thread for COM/driver creation and teardown.
Its driver callback consumes a bounded SPSC FIFO, converts to the driver's
native planar PCM/float format, applies atomic volume/mute and fills underruns
with silence. No Qt calls, allocation, mutex or DSP runs in that callback.
The normal Qt output remains the default; no input/TX path is added.

The owner-thread refill timer runs every 10 ms, while ASIO consumes a complete
driver quantum at once. The playback FIFO therefore reserves at least 50 ms
plus one quantum of prefill (and at least two quanta), with twice that storage.
Startup and recovery wait for the prefill; a 5 ms ramp smooths the transition.
One starvation interval counts once, and its count is logged when the sink
closes. The prefill is separate from the driver's configured buffer frames.
It covers bounded scheduling jitter; it is not adaptive clock-rate matching.

The Audio-page widget uses the existing AudioEngine device-selection pattern
to queue the complete configuration and receive status. Its direct AsioOutput
header dependency supplies only host-device settings and driver identities;
the header is classified `ui-support`, like AudioOutputRouter. The SDK stays
private to the implementation, and no radio vendor header or wire verb is
added to the UI.

The driver is identified by CLSID from the 64-bit ASIO registry. A single
session may be open. The host validates the stereo pair, rate, buffer range
and granularity and each output channel's sample format. It supports
little-endian Int16/24/32, Float32/64 and right-aligned Int32 16/18/20/24.
Driver reset/resync/buffer/rate changes silence callbacks and stop playback
on the next existing audio timer tick; the operator reapplies the settings.
PCM source retirement stops the driver before clearing its FIFO and both
native buffers, then restarts, so audio from an expired session is removed.

`AsioOutput` owns one versioned AppSettings JSON value. Unsupported future
versions are neither applied nor overwritten. Writes verify persistence.
The ASIO device clock remains fixed when a producer changes between 24 and
48 kHz; the existing RX resampler follows the selected output clock.

Local references:

- Thetis-Ramdor `852bf0ef0b4f3886a13fc2846489aee16f361872`:
  `ChannelMaster/cmasio.c` and `cmASIO/hostsample.cpp`, stereo output callback,
  base output channel, driver sample format and host callbacks. Its microphone
  path, registry-based preferences and separate DLL are not ported.
- NereusSDR `3e5bf0b741d5e7bb311e6b24dd08b62a97efedcd`:
  `core/audio/AsioDriverWin.cpp`, `AsioSession.cpp`, `IAsioDriver.h`, driver
  limits, sample conversion and reset handling. Aether's implementation uses
  its existing owner thread rather than Nereus's capture helper and shared
  multi-role session. SDK interface provenance is in
  `third_party/asiosdk/README.md`.

Software checks cover FIFO/channel order, wrap, full/empty behavior, concurrent
producer/consumer access, sample conversion/clipping, nonfinite silence,
settings round-trip/range/schema refusal and unknown-driver failure. They do
not qualify playback or callback shutdown on a real ASIO driver.

The refill regression injects complete 64/128/256/1024-frame callback bursts
against a 10 ms producer with a 40 ms delayed refill, at all four configured
rates. Recovery checks retain short bursts until prefill and verify both ramps.

On 2026-10-22 the full local Windows application build and nine targeted
CTests passed: ASIO output, six RX rate/sink scenarios, PCM lifetime and the
radio-setup integration. The user documentation production build also passed.

The `radio-setup-audio` screenshot predates the ASIO group and needs a Windows
recapture with a configured ASIO device.
