---
title: "ANAN / OpenHPSDR"
slug: "/anan-g2"
description: "Experimental OpenHPSDR P1/P2 reception and opt-in voice, CW, TUNE and local-modem transmission."
status: "Experimental; P1/P2 transmit requires hardware qualification"
applies_to: ["ANAN-G2/G2 1K", "ANAN-7000DLE/8000DLE", "ANAN-10/100", "ANAN-10E/100B", "ANAN-100D", "ANAN-200D", "ANAN-G2E", "Anvelina Pro 3", "Red Pitaya"]
---

:::info[Status]

**Status:** Experimental; P1/P2 transmit requires hardware qualification · **Applies to:** ANAN-G2/G2 1K, ANAN-7000DLE/8000DLE, ANAN-10/100, ANAN-10E/100B, ANAN-100D, ANAN-200D, ANAN-G2E, Anvelina Pro 3, Red Pitaya

:::

AetherSDR provides experimental OpenHPSDR reception with multiple software slices and independent receive windows. The extended backend offers opt-in P1/P2 transmission for its supported model/protocol combinations, described below; RF hardware qualification is still pending. The original tropo1234 G2 implementation remains receive-only. FlexRadio remains the supported target (see [Supported Radios](./supported-radios.md)). An **EXPERIMENTAL** badge shows while a G2 is connected.

The **ANAN-7000DLE** has an experimental Orion MkII receive profile. It uses
physical DDC2, two ADCs, and Thetis-compatible ADC0 BPF1 and antenna-chain
LPF selection. Discovery displays the Orion MkII family because board ID 5
is shared by several models. This profile has protocol and transport test
coverage; reception on an ANAN-7000DLE has been confirmed by the operator.
It supports multiple receive slices and independent windows within the limits below; transmit remains unavailable.

Additional experimental P2 RX profiles cover **ANAN-10/100 (Hermes)**,
**ANAN-10E/100B (Hermes II)**, **ANAN-100D (Angelia)**,
**ANAN-200D (Orion)** and **ANAN-G2E (HermesC10)**. Packet and transport
tests cover these profiles; reception on their hardware is not yet confirmed.
The radio must run Protocol 2 firmware.

On Hermes, Hermes II and HermesC10 select **ADC0 (ANT1/2/3)**: these boards
have one ADC, and ADC1 is refused. Legacy boards use their ALEX HPF table;
HermesC10 uses MkII BPF selection. All use frequency-selected antenna-chain
LPFs. The slice and independent-window limits depend on the selected profile and protocol.
Hermes/Hermes-II radio-speaker channels follow Thetis' stereo swap.
Supply voltage for these additional profiles is not reported until qualified.

## Saturn implementation selection

A Saturn radio discovered on Protocol 2 has two choices:

- **Saturn (ANAN G2) — tropo1234** runs the independent original backend
  from main commit `c2a67af5`. It supports one receiver and one panadapter
  on DDC0, with ADC0 or ADC1, the original front-end options, receive
  band-pass filters, speaker output and amplifier mute.
- **Saturn (ANAN G2) — DH1KLM** runs the extended model-aware backend,
  including the multiple-slice and receive-window features described below.

Select the implementation under **Settings → Radio Setup → ANAN Hardware →
Saturn implementation** while disconnected. This sets the next Saturn
connection preference. Discovery lists both choices for the same physical
MAC/IP; selecting a row explicitly chooses that implementation. Under
**Connect by IP**, choose **ANAN-G2** for DH1KLM or
**Saturn (ANAN G2) — tropo1234** for the original in **Receive profile**, or use
**Use saved model** to retain the saved implementation. Other hardware,
including HermesC10 / ANAN-G2E, uses the extended implementation.

The Setup selector shows the active implementation and is locked during a
connection. Disconnect before switching. Both choices target one physical
radio; they cannot run simultaneously. Both remain receive-only.

Original and extended connection options, operating-state memory and droop
calibration are independent. Existing extended settings are retained without
migration. The original starts with its main defaults in a separate settings
scope. Nickname, MAC, IP and firmware describe the shared physical radio.
The saved implementation is reused by automatic connection and reconnect.
Original connection options marked **next connect** take effect on the next
connection; its band-pass-filter and amplifier-mute controls apply live.

## Microphone hardware options

Under **Settings → Radio Setup → ANAN Hardware → Radio Info → Microphone Hardware Options**,
choose **Mic** (Tip or Ring), **Bias** (Off or On), **PTT** (Off or On) and **Input**
(3.5 mm or XLR), then click **Apply microphone options**. These configure the
radio's physical microphone connector, independently of the PC microphone and ASIO.
They apply live and are saved separately for each device, model and protocol.
There is no hardware readback; the controls show the saved/requested configuration.

Tip/Ring, bias and microphone PTT permission are supported for Orion/ANAN-200D,
Orion MkII/ANAN-7000DLE/8000DLE, Anvelina Pro III and Saturn/G2 profiles under
their supported P1/P2 connections. XLR selection requires Saturn/G2 with P2.
Unsupported options stay dimmed with a reason. Hermes, Angelia/ANAN-100D,
G2E and Red Pitaya have no supported software microphone wiring controls here.
The original tropo1234 implementation is unchanged.

Defaults are Tip, bias Off, PTT Off and 3.5 mm. **PTT On** permits the microphone
switch in the radio's configuration; it does not enable a TX session.
The connector settings send no MOX or transmit IQ. The first voice TX path uses
PC microphone audio; radio microphone audio and physical radio PTT have not been
qualified. Alex/Apollo selection is not part of this panel.

## Transmit settings

Open **Settings > Radio Setup > ANAN Hardware > Transmit** in the extended
implementation. The table holds **PA gain (dB)** and **Power limit (W)** per band.
**Use Thetis defaults** restores reference gains and zero power limits. These
reference gains are not measurements of your amplifier. Keep a positive limit
only on bands your hardware supports.

Choose **TX antenna** (**ANT1**, **ANT2** or **ANT3**) and click **Save PA profile**.
The table and antenna are remembered per radio, model, board and protocol.
Changes can be saved while receiving; the controls are locked while transmitting.
A zero limit keeps the corresponding band closed for TX.

The **ANAN TX** log category records the selected model, board, protocol,
frequency, antenna and raw drive for key-on, plus the local transport result.
A successful local write does not confirm that the radio switched its relay or
emitted RF. Protocol 2 uses the PA-enable polarity from Thetis; repeated discovery
retains the currently armed PA configuration. A transport fault stops the current
transmission. After a successful local stop, readiness is prepared again without
requiring **Save PA profile**. Starting another transmission requires a new
operator action; a failed stop keeps TX unavailable or disconnects the session.

TX signal processing and readiness are managed automatically. Idle tuning within
the same permitted band retains TX readiness and the selected TX slice. Retuning
while keyed cancels the current transmission; changing band or mode rechecks the
TX prerequisites. There is no
preview calculator, manual DSP preparation or extra transmit-enable button.
The status explains when the current mode, band plan or hardware state prevents
transmission. One primary receive slice is required. Frequency changes refresh
readiness; mode changes prepare the matching DSP on the existing planning worker.
Neither operation keys the radio. A fresh connection starts at **RF Power 0%**;
retuning within the connection preserves the operator's percentage.

Use **RF Power** and **Tune Pwr** in the TX applet. The percentage refers to the
saved band limit and is converted through its PA gain. This is requested power,
not measured RF output. Only **MOX**, software **PTT**, **CW** or **TUNE** starts a
transmission. XIT is not implemented; an XIT request is reset.

Native P1 uses 48 kHz 16-bit IQ; P2 uses 192 kHz 24-bit IQ. TX uses the validated
receive model or explicit board override. This adds no protocol support to a
model/firmware combination that cannot already receive. The original tropo1234
G2 implementation remains receive-only and unchanged.

**TUNE** sends a carrier at the dial frequency using **Tune Pwr**. CWL/CWU use
the software keyer, a 5 ms carrier envelope and the configured break-in hang time.
USB/LSB use a 300–3000 Hz passband; DIGU/DIGL use 150–3000 Hz, following HL2.
AM/SAM and DSB use ±3000 Hz;
SAM transmits AM. FM uses 5 kHz deviation with an 8 kHz occupied-band guard on
each side. DRM, Spectrum and broadcast-wide FM remain receive-only.

DIGU/DIGL use the existing TCI audio path for external programs such as WSJT-X.
WSPR and AetherModem/AX.25 use the existing host-generated audio path, following
the HL2 implementation. Source levels are preserved apart from fixed digital
headroom and the operator's PA power setting; the ANAN modulator adds no speech
gain or automatic level makeup to modem audio. Finite modem audio is flushed
before its captured PTT operation is released. The matching TX signal processing is prepared automatically after a mode change. RADE/FreeDV, D-STAR and a
DRM modem are not integrated for ANAN.

Radio microphone/PTT qualification and PA calibration remain open. Software tests do not establish actual RF power,
signal quality or protection. The radio speaker stream is silenced while this
client keys P1 or P2 TX. Receive audio is muted while keyed, with a short hold after release; the independent spectrum stays live. This prevents hearing the receiver pick up its own transmission. **MON** in the **Phone & CW** panel enables a local microphone/audio monitor;
its monitor level adjusts the PC playback volume. It monitors the audio entering
TX signal processing, not received RF, PA output or a CW sidetone. MON is off by
default. Microphone, post-ALC audio peak and ALC gain meters use the actual WDSP
TX channel; they are not measured RF power. **ALC Gain** reads 0 dB when the limiter
is not reducing the signal. **PROC** uses the shared local `ClientComp` voice processor, as on HL2.
**Compression** shows its actual gain reduction in dB; it is zero when the
processor is off or not compressing. The meter update starts when automatic TX
preparation completes. Neither reading measures RF output. The same controls apply to supported
extended ANAN models and their overrides on P1 and P2. The Original G2 path is
separate.

**Settings → Radio Setup → Transmit → Show TX in Waterfall** is a local, per-radio display setting, as on HL2. It allows FFT-derived waterfall rows during MOX/PTT, TUNE and CW; it does not measure PA output power or create a separate RF monitor.

P2 TX keeps its 192 ksample/s packet clock when a timer callback arrives late:
fitting packets are sent within the existing 20 ms limit. Longer stalls stop TX;
they do not accumulate delayed RF audio. Queue overflow is logged separately from
an audio format or source change.

The **RF Power** control requests PA drive using the saved profile. It starts at
**0%** on a fresh connection. Operator values are saved per band, radio, model
and protocol and recalled when returning to that band; a band without a saved
value uses 0%. Recalling a value never keys TX.

Forward/reflected RF power and SWR use the radio's P1/P2 detector readings and
Thetis model reference curves. These are estimates until the individual unit
is calibrated; the drive slider itself is not a watt measurement. Ambiguous
automatic board identities require an explicit model profile for conversion.
SWR is evaluated only above 0.1 W forward power. G2 1K and HL2 detector scaling
are not provided by this extended path. No new SWR protection is implied.

The PA table uses its row height; **Use Thetis defaults**, **Save PA profile**,
**TX antenna** and readiness appear directly below the last band.

Band-view and CTUN notices overlay the right side of the footer temporarily.
CPU, Network, TX, date and the station control keep their positions.

### Extended TX integration coverage

| Control/path | Current extended ANAN implementation |
| --- | --- |
| MON, microphone peak, ALC peak/gain | Local monitor and actual WDSP TXA meters |
| PROC / Compression | Shared local ClientComp and its gain-reduction meter |
| MIC gain before ALC | Backend MIC-slider gain mapping not implemented |
| Adjustable TX voice filter | Fixed mode defaults; operator filter control not implemented |
| CW pitch | Connected to the host receiver BFO; CW monitor level/pan dispatch is incomplete |
| PC microphone selection | PCM path exists; explicit local input-selection acknowledgement is incomplete |
| Graphic EQ | Shared host controls exist; saved-layout reapplication after delayed TX readiness is incomplete |
| MIC / TX-filter recall | Incomplete; RF drive now recalls separately per band/model/protocol |
| Hardware PTT observation | Not implemented; local MOX state is not hardware readback |
| Forward/reflected RF power and SWR | P1/P2 detector telemetry with Thetis model reference conversion; individual calibration pending |
| TX slice / multiple receivers | TX requires the single primary receive slice |

## Mouse-related receive diagnostics

The extended ANAN receiver waits for each WDSP output exchange, following
Thetis and Nereus. This keeps a burst of queued UDP IQ packets from overrunning
the asynchronous exchange pipeline. It applies to P1 and P2; the original
tropo1234 receiver and the system audio-buffer setting are unchanged.

For audible peaks or S-meter jumps during mouse movement, enable **Performance**
under **Help → Support & Diagnostics... → Application Logs**. The existing UI
heartbeat then logs `RxTrace` windows of approximately **100 ms**. Disable the
filter after the capture. This applies to the extended backend; the original
tropo1234 implementation is unchanged.

- `stage=anan`: raw wire IQ RMS/peak before WDSP, successful WDSP output
  RMS/peak, post-DC-blocker PCM RMS/peak, actual emitted signal-meter maximum,
  and separate counts for WDSP `Ok`, internal `Underrun`, `Busy`, invalid buffer,
  allocation violation and engine error. Packet-sequence gaps, maximum IQ callback
  gap, callback duration and WDSP exchange duration are recorded separately.
- `stage=playback`: the final mixed float PCM accepted by the output device,
  after the client chain and mixing/clamping. This is before device volume,
  ASIO callback gain and any hardware processing; its timestamp is the write,
  not when the sound leaves the speaker. Playback buffers introduce a delay
  between the upstream and playback traces.
- `startMs`, `endMs`, peak times, meter time and first/last internal underrun
  times use the same monotonic clock as the performance telemetry.
  `mouseEvents` counts Qt-delivered application mouse-move events in the window;
  it is not the physical mouse polling rate. `lastMouseMs` identifies the most
  recent such event. Events outside the application's delivered event stream
  are not counted.
- Levels are uncalibrated dBFS: complex IQ energy uses I²+Q²; real audio energy
  averages both channels. `fullScale` counts PCM samples at or above full scale;
  `invalid` counts non-finite samples. Sample counts distinguish absent data
  from silence. Invalid samples are counted, not included in RMS calculations.

Producer paths only accumulate fixed-size statistics and publish into a bounded
queue. The UI writes the log. `traceDropped` exposes lost diagnostic windows if
that queue fills; a capture with increasing drops is incomplete. Zero packet
loss and zero playback underruns do not exclude internal WDSP underruns, and
an internal underrun during initial channel filling does not by itself explain
an audible peak. Compare periods with a stationary mouse against periods of
movement before attributing a cause.

## UDP receive buffer

The extended backend requests **1,024,000 bytes** (**0xFA000**, matching Thetis)
for the P1 and P2 UDP receive sockets after binding. The connection log records
both the requested size and the value granted by the operating system, for
example `ANAN P2: UDP receive buffer requested=1024000 bytes; granted=1024000 bytes`.
An unreadable OS result is logged as `granted=unavailable`. The OS may grant a
different size. The original tropo1234 implementation is unchanged.

## Automatic AGC-T

In the DH1KLM implementation, click **A** beside the AGC controls in the
**RX applet** to let the host DSP follow the receive noise floor. Slow, Med
and Fast still select the AGC response speed; automatic threshold tracking
is a separate choice. Click **A** again to return to manual control. AGC Off
pauses automatic tracking without changing its fixed-gain setting.

Under **Settings → Radio Setup → ANAN Hardware → Radio Info → Auto AGC-T**,
select the **Receive slice** and set its **Noise-floor offset** (-60 to +60 dB,
initially +20 dB). Each slice measures its own receive stream. Automatic mode
and offsets belong to the current receive session; a new connection starts
in manual mode. The original **tropo1234** implementation remains unchanged
and does not offer this feature.

Increasing **Noise-floor offset** reduces automatic gain; decreasing it raises
the noise and weak signals. The DH1KLM receive DSP uses unity output gain before
the app's audio volume control, for both manual and automatic AGC-T.

Tracking uses an unaveraged output of the existing FFT, before display droop
correction or edge fade. Measurements are normalized to a 4096-point reference,
so resizing the display does not alter the intended gain. Display smoothing
is independent. After enabling, retuning or a discontinuity, the estimator
settles for about two seconds. It updates at most every 500 ms when fresh,
valid data is available; on missing data it keeps the last applied gain.
No additional audio buffering is introduced.

## Multiple slices and receive windows

Use the main UI's add-slice control on an existing panadapter for another
independently demodulated frequency inside its sampled span. The slices share
one hardware IQ stream, with separate mode, filter, AGC, noise blanker, volume,
mute and stereo balance. Each slice also has its own pre-volume audio tap.
The speaker mix is sample-driven on the receive I/O/DSP thread. Active audio
contributors are synchronized through per-slice buffers, so temporarily uneven
block delivery does not insert silence for a late slice or depend on a GUI
mixer timer. A new slice joins the mix on its first audio block; closing it
explicitly removes its contribution. Mute and balance preserve this audio timing.
Adding an independent panadapter claims a separate hardware receive stream;
it is refused when none is free, rather than creating a coupled duplicate.

When a slice leaves a shared capture's span, AetherSDR can move it to a free
stream or an existing compatible stream. The other slices' capture stays in
place. A lone receiver can retune its own hardware centre. If no compatible
stream is available, the frequency change is refused and the observed VFO
remains at the previous value. Closing the final slice of a stream releases
its window; the final slice of the whole connection cannot be removed.
Moving a shared panadapter preserves each slice's tuned frequency; a center
that would exclude a hosted slice is refused. Slice letters reuse the lowest
free letter when a receiver is closed and replaced. Internal receiver identities
remain distinct so delayed work cannot affect its replacement.

| Effective profile | Maximum slices | Maximum independent P1 windows | Maximum independent P2 windows |
|---|---:|---:|---:|
| Hermes / ANAN-10, ANAN-100 | 4 | 4 | 4 |
| Hermes II / ANAN-10E, ANAN-100B | 2 | 2 | 2 |
| Angelia / ANAN-100D; Orion / ANAN-200D | 5 | 4 | 5 |
| Orion MkII / ANAN-7000DLE, ANAN-8000DLE | 5 | 4 | 5 |
| HermesC10 / ANAN-G2E | 5 | 4 | 4 |
| Saturn / ANAN-G2 | 5 | Not implemented | 5 |

These are ceilings, not guaranteed gateware resources. The physical board,
effective override and P2 discovery receiver count can lower the available
window count. Red Pitaya and Anvelina mappings use those same checks; an
override does not manufacture additional physical DDCs. Multi-receive has
software DSP/packet tests and still requires a Windows and hardware test.

**P2 sample rates are per receive window. P1 has one shared sample rate:**
changing the span on a P1 window rebuilds all active receiver DSPs at that rate.
A narrower span is refused if it would exclude another slice sharing the
window. Planning runs on the existing background build thread while reception
continues; changing rates briefly mutes the affected DSPs while they settle.

Independent digital windows do not create independent analogue inputs.
All windows currently use the connection's selected ADC. Antenna, attenuation
and ALEX filtering are shared. Automatic HPF/BPF selection considers every live
slice on that ADC: one required filter stays selected; different required rows
use receive bypass. The hardware page reports **BYPASS (multi-band: …)** while
the manual bypass checkbox retains your setting. Closing or retuning a slice
recomputes the filter and restores normal filtering when the conflict disappears.
The shared receive LPF follows the highest DDC centre on ADC0 (including with
CTUN); **6m/ByPass on RX** remains a separate manual override. RX OC outputs
continue to follow the first active hardware stream.

An explicitly selected native **Anvelina Pro 3** profile combines required BPF
rows instead of using its non-passing bypass path. A shared 6m BPF/LNA excludes
the incompatible 12/10m row; the hardware page reports this restriction. A DIY
Anvelina or Red Pitaya mapped to **ANAN-200D** uses the mapped filter hardware's
normal bypass policy. Combining filters can alter signal levels and requires
hardware qualification. Changes to shared gain or antenna controls apply to the
common RF path.

Additional slices/windows are session-only and are not restored on reconnect.
Band buttons address a slice in their own panadapter, preferring the active slice
in that pan. Selecting a band in another panadapter leaves the globally active
receiver in its original band. Saved CTUN views are not restored while
multi-receive is active.
The existing **B** band-view toggle/recall and droop sweep require the original
receiver to be the only slice. The original receiver retains its CTUN control. TX, diversity,
PureSignal and simultaneous independent ADC selection are not added by this
receive extension.

## Where to go next

- [Panadapter Controls](./panadapter-controls.md): FFT AVG, Black Level and NB Blank on the G2's panadapter.
- [RX Controls](./rx-controls.md): tuning, AF gain, mute and balance.
- [DSP Noise Mitigation](./dsp-noise-mitigation.md): client-side noise reduction.
- [Memory Channels](./memory-channels.md): stored on this computer.

## Setup

The G2 is found automatically on your LAN: open **File → Connect to Radio...** → **On This Network**, select it and click **Connect Selected Radio**.

Discovery also sends a directed broadcast to each attached IPv4 subnet. This can find a G2 on a second NIC with its own subnet. Two NICs sharing a subnet still use the preferred route; VPNs and routers may block broadcast discovery. If the G2 does not appear, use **Connect by IP** with **Radio type:** set to **ANAN / OpenHPSDR (P1/P2)** (see [Manual Connection](./manual-connection.md)).

If opening the discovery UDP socket fails, automatic discovery retries on each
scan (normally every five seconds). Bind failures, missing IPv4 subnet broadcast
addresses and probe-send failures appear under the discovery log category.
Repeated failures are reported once until that condition recovers; send failures
are tracked separately for P1 and P2 at each destination. After three scans that
sent a probe but received no accepted reply, an informational message reports
that no supported OpenHPSDR radio answered. This alone does not identify a
hardware fault. With no successful sends, the send/bind diagnostics explain the
failure instead. Stop/start clears the diagnostic history.

Click **Scan** on **On This Network** to request discovery immediately, even
when radios are already listed. This also sends the HL2 and ANAN P1/P2 probes.
The status shows **Scanning…**, then the end time of the two-second response
window and the total devices currently listed. The count includes existing
entries; it is not a count of new replies. Automatic discovery continues and
can still receive late replies. **Retry Discovery** uses the same scan action.

To reach it across a VPN or another subnet, use **Connect by IP** with **Radio type:** set to **ANAN / OpenHPSDR (P1/P2)** (see [Manual Connection](./manual-connection.md)). For the ANAN-7000DLE, use the same **ANAN / OpenHPSDR (P1/P2)** radio-type entry;
Discovery selects the Orion MkII receive layout automatically. The form's
connection options apply on both paths.

**Connect by IP** accepts validated P2 discovery replies from UDP source port
0 as well as 1024. An ANAN-7000DLE capture showed port 0 for both automatic
and directed discovery; the request is still sent to UDP port 1024, and the
reply must come from the requested IP address.

### DIY board override

On **Connect by IP**, choose the required **Receive profile**, then set
**Board override → Override detection (ON)** before connecting. This permits
an intentional mismatch, such as an Orion MkII digital board with an
ANAN-200D LPF/PA board. The selected model determines the complete effective
hardware profile: ADC/DDC layout, filters, antenna control, speaker-channel
mapping and supported board-dependent telemetry/defaults. This is a full
model override, not a separate filter-board selector. A wrong selection can
therefore select incorrect filters or ADC/DDC routing. An extended TX session
uses the same effective model and still requires explicit preparation and enable.

The switch has three states: **ON** forces the selected explicit model;
**OFF** restores normal model/board compatibility checks; **use saved
setting** (partially checked) reads this device's setting after discovery
supplies its MAC. **Use saved model** can reuse a previously selected explicit
model; **Automatic physical-board profile** cannot be used with override ON.
The model and override are saved together per MAC when connecting, and apply
to later automatic-discovery connections too. Existing settings default to
override OFF. To change the override, disconnect and reconnect through this
page. The firmware's actual P1/P2 protocol and the discovered device identity
are retained; busy, unknown and unsupported devices are still refused.

Configure the receive front end under **Settings → Radio Setup... → Radio → ANAN Hardware → Connection Defaults**. The page is available while connected to an ANAN. These family-wide defaults are not a display of the active session. Its options apply on the next connection, whether you use **Available radios** or **Connect by IP**:

| Option | Default | What it does |
|--------|---------|--------------|
| **Sample rate:** | 48 ksps | Receiver sample rate (G2 DDC0, 7000DLE DDC2): 48, 96, 192, 384, 768 or 1536 ksps. It is also the starting width of the panadapter. Higher rates use more of the radio's Ethernet link. |
| **Feed the receiver from:** | ADC0 (ANT1/2/3) | Which receive chain feeds the receiver: **ADC0 (ANT1/2/3)**, behind the switched antenna relays, or **ADC1 (RX2)**, wired straight to the RX2 jack. |
| **Dither** / **Randomization** | On | The ADC's linearisation bits. Leave them on unless you are testing. |
| **Send receive audio to the radio** | Off | Also plays receive audio through the G2's own speaker and headphone jack. Takes effect on the next connect. |

Filter bypass is controlled only under **ANAN Hardware → Antenna / ALEX** and applies immediately for the connected device and effective model. Change the live sample rate under **ANAN Hardware → Radio Info → Operating Parameters**, or use the panadapter **− / +** buttons.

On **ANAN Hardware → Radio Info → Operating Parameters**, **Mute the radio's speaker amplifier** disables the radio's hardware amplifier immediately. It defaults to off and is remembered for later connections. It requires Protocol 2 and an ANAN-7000DLE, ANAN-8000DLE, ANAN-G2, ANAN-G2 1K, Anvelina Pro 3 or Red Pitaya model profile. Automatic detection enables it for Saturn/G2 (board 10); board 5 requires an explicit supported model profile because its amplifier cannot be identified from discovery alone. The control is unavailable for ANAN-100D/200D, generic Orion MkII, G2E and Protocol 1. PC audio is unchanged. Sending silence to the radio leaves its amplifier running; this control also suppresses amplifier hiss. A clean disconnect releases the mute.

## ANAN Hardware

Open **Radio Setup → Radio → ANAN Hardware** while connected to an ANAN.
This device-specific page is hidden for HL2, FlexRadio and other backends,
and while disconnected. Searching the setup pages does not reveal it for
an unsupported backend. Within the ANAN page, unsupported controls such as
Alex-2 on a legacy profile remain visible but disabled with a reason.
The page follows the Nereus hardware-page arrangement:

- **Connection Defaults**: sample rate, ADC source, Dither, Randomization and radio speaker audio for the next connection. Existing saved values are retained. Filter bypass remains under **Antenna / ALEX**.
- **Radio Info**: **Board Identity** (physical board, P1/P2, ADC count, reported
  receiver count, firmware, MAC, IP) and **Operating Parameters**. The effective
  model and override remain distinct from the physical board. Active receive windows share the selected ADC and its analogue front end.
  **Receive window** selects a live capture; **Sample rate (Hz)** changes its
  rate immediately through the existing DSP rebuild. P2 offers 48000, 96000,
  192000, 384000, 768000 and 1536000 Hz. P1 offers 48000, 96000 and 192000 Hz;
  an explicit Red Pitaya P1 profile also offers 384000 Hz. P2 changes the
  selected window and all slices sharing it; P1 changes every receive window.
  A narrower rate is refused if it would exclude a co-hosted slice. Selection
  is disabled during a receive rebuild or droop sweep. The displayed rate is
  session state, not an acknowledgement from the radio. Connection Defaults
  remains the independent starting-rate setting for the next connection;
  existing exact-profile band-view recall keeps its normal precedence.
- **Antenna / ALEX**: **Antenna Control**, **Alex-1 Filters**, **Alex-2 Filters**.
  **Antenna Control** places the TX safety options above TX and RX per-band
  antenna matrices, followed by the TX bypass options. TX controls remain
  dimmed with an explanation. **Enable RX antenna per band** activates the RX
  table; it starts off so existing manual choices remain effective. Each band
  keeps an independent ANT1/2/3 selection and RX-only selection. The two external
  input checkboxes exclude each other and can both be cleared to receive through
  the ANT bank. Names follow the selected model. Edits are saved immediately;
  the current band's row is applied when band routing is enabled.

  RX1 controls the shared ADC0 antenna path using its receive frequency, including
  when CTUN holds the capture centre elsewhere. Other slices follow this physical
  path; selecting or tuning one cannot take antenna control from RX1. If RX1 is
  closed, the lowest remaining receiver ID takes control. All receiver labels
  report the commanded shared input. ADC1 retains its dedicated RX2 input;
  the table does not provide independent antennas for slices sharing one ADC.

  Amateur-band selection follows the active regional band plan. Frequencies
  outside those bands use **GEN**. **WWV** covers ±5 kHz around 2.5, 5, 10, 15,
  20 and 25 MHz. GEN/WWV are explicit Aether slots rather than Thetis's
  nearest-amateur-band fallback. **2m**, **XVTR** operating mode and the table's
  conditional XVTR checkboxes remain disabled with explanations: transverter
  translation is not implemented and receive tuning ends at 61.44 MHz.

  **Current RX Antenna** below the matrices displays the effective **ADC0 input**
  and **ANT bank**. These manual selectors are disabled while band routing is on;
  edit the band row instead. Turning band routing off restores the separately
  saved manual path. **ADC1 RF filter bypass** remains independent. Settings
  remain separate for this MAC address, effective model and physical board.

  **Alex-1 Filters** places **Reset filter defaults** above two filter banks.
  The receive-bank master bypass and RX/TX options sit above the filter rows;
  the LPF bank has its RX bypass option below the rows. Every row shows its
  name, commanded-state LED, optional band bypass, and lower/upper frequency
  fields with six decimal places and an **MHz** suffix. LEDs align vertically;
  complete banks stack when the window is narrow. Older profiles show
  **ALEX HPF bands**; MkII profiles show **ALEX BPF bands**.

  **Alex-2 Filters** uses the same two-bank arrangement and remains visible
  for every ANAN profile. Its ADC1 receive controls are enabled for supported
  dual-ADC MkII profiles (ANAN-7000/8000, Orion MkII, Anvelina and G2).
  Legacy models, single-ADC profiles and an ANAN-200D override show disabled
  controls with an explanation. A second ADC alone does not imply Alex-2.
  The master bypass mirrors **ADC1 RF filter bypass** in **Antenna Control**.
  Individual ADC1 band bypasses and an independent Alex-2 LPF bank are
  displayed disabled because they are not implemented. The current receive
  LPF remains on Alex-1.

  **Reset filter defaults** saves and applies only the frequency edges for the selected
  page: ADC0 and available LPF on Alex-1, ADC1 on Alex-2. Bypass, antenna and
  OC settings are kept. The change is applied automatically. **Restore model defaults**
  remains the separate action that restores the whole hardware-filter
  configuration, including bypass and OC defaults, while retaining the antenna
  band table and its enable state.
- **OC Outputs**: **HF**, **VHF**, **SWL**. HF has RX/TX pin matrices side by side
  and a commanded RX pin-state readout. HF and SWL RX matrices are configurable;
  Four manual **User Dig Out** pins are independent of the band matrices.
  TX and VHF remain disabled with their unsupported reason.
- **Calibration**: frequency correction and receive level calibration are available
  for P1/P2. TX-display, current and LNA-step calibration remain unavailable.
  **Open Droop Correction** opens the
  existing P2 droop sweep; it is disabled for P1.

An explicit model override determines the filter configuration; otherwise the
selected compatible model or physical-board profile is used. FlexRadio, Icom
and HL2 retain their own hardware settings.

With **Enable RX antenna per band** off, **Antenna Control → ADC0 input** selects
the ANT1/2/3 path or a dedicated receive input. Changes are saved and activated immediately for this MAC,
effective model and physical board. Names follow the selected model, including DIY
overrides:

| Effective model | Additional receive inputs |
|---|---|
| Hermes, ANAN-10/10E | RX1, RX2, XVTR RX |
| ANAN-100/100B/100D/200D, generic Orion MkII | EXT2, EXT1, XVTR RX |
| ANAN-7000/8000, G2/G2 1K/G2E, Anvelina Pro 3, Red Pitaya | BYPASS, EXT1, XVTR RX |

**ANT bank** still selects ANT1/2/3 immediately. The active input readout
changes after the input selection is applied. These settings affect ADC0;
ADC1 reception continues through its dedicated RX2 input. **BYPASS** here is
an input route, separate from HPF/BPF filter bypass. **XVTR RX** selects the
physical receive input only; it does not configure frequency translation or
transmit. Connector availability depends on the fitted RF hardware. Returning
to **ANT1/2/3**, or restoring model filter defaults, clears the dedicated-input
selection while retaining the saved ANT-bank choice. Older settings default
to the existing ANT1/2/3 path.

The main panadapter **ANT → RX ANT** selector also offers **ANT1**, **ANT2**
and **ANT3** when ADC0 uses the antenna bank. It uses the same per-device,
effective-model and board setting as **ANAN Hardware → Antenna / ALEX →
Antenna Control → ANT bank**. Changes from either surface update the main
display. If the choice cannot be saved, the previous selection is restored.

When a dedicated ADC0 input is selected, **RX ANT** shows that input's
model-specific name; when ADC1 feeds the receiver, it shows **RX2**. These
fixed active routes do not offer an ANT-bank switch that would leave the
active receiver unchanged. Choose the dedicated ADC0 input on **Antenna
Control**, or change the ADC source on **ANAN Hardware → Connection Defaults** and
reconnect. The hardware page's ANT-bank control remains available for ADC0.

**ALEX HPF bands** is used by the legacy Hermes, Angelia and Orion profiles
(including ANAN-100/100D/200D). **ALEX BPF bands** is used by Orion MkII
(including ANAN-7000DLE/8000DLE), Saturn/G2 and G2E profiles. Red Pitaya uses
the existing legacy HPF receive profile. Board ID 11 (**Saturn MkII**) is
recognized but cannot connect: the checked Thetis and public Saturn sources
do not establish a separate receive profile for this ID. A newer PCB revision
alone does not imply board ID 11. An automatic board profile identifies
a board family, not a guaranteed commercial model or external DIY filter board.

Edit the lower and upper **MHz** fields with six decimal places (1 Hz resolution).
The band-bypass checkbox is compact; filter names and six-decimal fields
retain readable widths. LEDs occupy the same fixed position in each row.
On narrow windows, HPF/BPF and LPF tables stack vertically; enlarge the
resizable setup window to place them side by side. Each filter name is drawn
once, with the LED in a fixed position after the name.
Rows must be ordered, non-overlapping, and within 0–61.44 MHz. Frequencies in
receive-table gaps select bypass. LPF gaps select the 6 m LPF fallback.
**HPF Bypass (master)** / **BPF Bypass (master)** on Alex-1 overrides ADC0.
**ADC1 RF filter bypass** in Antenna Control overrides ADC1; the Alex-2
**ByPass / 55 MHz BPF (master)** checkbox mirrors it on supported profiles. Single-ADC models disable ADC1 controls. LPF limits are disabled
when the active profile does not drive LPF selection (the P2 Saturn profile).
Both automatic ADC filter banks follow the first active hardware stream.
ADC1 inherits ADC0 ranges until independent Alex-2 limits are saved. ADC0's individual **Bypass** boxes
apply to that HPF/BPF band only. **Disable 6m LNA on RX** selects ADC0 bypass
instead of the 6-m filter/LNA; **6m/ByPass on RX** forces the 6-m LPF. These RX
options take effect immediately and do not bypass ADC1's separate input
filter. On models with **Alex-2 Filters**, ADC1 has its own six frequency
ranges and commanded-filter LEDs. Its master bypass still takes precedence.
**ADC1 RF filter bypass** is on **Antenna / ALEX → Antenna Control** for every
dual-ADC profile, including ANAN-100D/200D and a DIY ANAN-200D mapping whose
Alex-2 controls are disabled. Changes are saved and activated immediately. Single-ADC
profiles display the control disabled with a reason. Alex-2 retains its bypass
indicator and independent band table on supported MkII profiles.
The shared RF reference frequency drives both banks; changing ADC1 limits does
not create another receiver or alter ADC0 limits. TX/PureSignal bypass and
**Disable 6m LNA on TX** remain disabled because these transmit front-end controls are not implemented.
The 6m LNA and TX/PureSignal bypass switches are grouped above the HPF/BPF
filter grid; **6m/ByPass on RX** is below the LPF grid.

The **Receive-only settings. Switches apply immediately…** note is directly below the HPF/BPF and LPF tables on **Alex-1 Filters**.

**Restore model defaults** is above the hardware tabs, with the save / error
status directly below. There is no Apply button: switches and selections
are saved and applied immediately. A frequency edge is committed with
**Enter** or on leaving its field, after range validation. Invalid or
overlapping limits and refused storage writes restore the last confirmed
settings and display an error. Unconfirmed fields are not included in an
unrelated checkbox change. Complete filter banks retain their
required height; use the page’s vertical scrollbar in a short window.

Changes are saved for this MAC and model and update the running receiver;
no reconnect is needed. These saved hardware settings take precedence over the
connection page's initial bypass defaults. **Restore model defaults** saves and
applies the Thetis-compatible frequency limits and default bypass states
(ADC0 automatic, ADC1 bypass). Changing the mapped model loads that model's own
settings, without transferring custom limits from a different model.

P2 profiles with LPF selection send the antenna/LPF relay word in both ALEX0
and ALEX1, matching Thetis' P2 4.3+ layout. While connected, the validated P2 General configuration, including
the ALEX enable mask, is refreshed every 500 ms so a lost initial UDP packet
can be recovered. High Priority packets retain their 100 ms keepalive.

This is separate from OC outputs:
external filters wired to open collectors use **OC Outputs → HF** instead.
Check the wanted RX pins per band and **Enable RX OC outputs**. Both changes
apply immediately; selecting a pin while outputs are enabled changes the
commanded output at once.
Both the enable and every pin default off, including after upgrading older
filter settings. The seven ordinary OC pins are supported by P1 and P2;
Anvelina's extra pins remain off. Band limits come from the active band plan
when first configured (hover the band name to inspect them), and are saved
with this device/model. Discrete receive channels are treated as one band
range; this is receive routing, not a transmit-permission check. Outside the
configured ranges and on a normal stop, all RX OC outputs are cleared.
An absent band is disabled with a reason. **Restore model defaults** also
clears OC assignments and disables outputs.

On **Alex-1 Filters**, a green LED beside the HPF/BPF and LPF band identifies
the currently commanded filter. Other rows stay unlit. Receive bypass has its
own indicator. Alex-2 has separate ADC1 band and bypass indicators. A profile that does not drive LPF selection leaves all LPF LEDs
off. LEDs update with tuning, independently of unfinished frequency edits, and expose
selected/not-selected names for screen readers. They do not confirm physical
relay movement.

**Commanded ADC0 / ADC1 / LPF** indicates the software's requested filter,
not physical relay feedback. The status reports automatic save completion or a refusal. The
page controls receive filters, antennas and the RX OC matrix; it does not add
transmit or PureSignal. Unsupported Nereus-style sections remain visible and
disabled rather than presenting working controls.

### Check ADC1 and Alex-2

Keep the receive profile that matches your fitted hardware, including a
required DIY override. **Alex-2 Filters** is available only for dual-ADC MkII
filter profiles. An **ANAN-200D** override has two ADCs; its **Alex-2 Filters**
tab remains visible with disabled controls and an explanation. Do not change
to an incompatible model to enable those controls.

1. On a dual-ADC profile, open **Settings → Radio Setup... → Radio →
   ANAN Hardware → Connection Defaults** and set **Feed the receiver from:** to **ADC1 — the RX2 jack**.
   Disconnect and reconnect to apply the source change. Confirm that **ANAN Hardware →
   Antenna / ALEX → Antenna Control** reports **Active receiver: ADC1 / RX2**.
   Connect the receive signal to the fitted ADC1 input; the software cannot
   detect which physical connector is wired to a DIY board.
2. While receiving on ADC1, change **ADC0 input** and change
   **ANT bank**. They may
   switch ADC0 relays, but must not select a different source for the active
   ADC1 receiver.
3. If this hardware supports **Alex-2 Filters**, record its current values,
   clear **ADC1 RF filter bypass** on **Antenna Control**.
   Return to **Alex-2 Filters** and tune across a saved
   ADC1 band boundary. Its band LED should change; verify the corresponding
   hardware relay or receive response separately.
4. To distinguish the two tables, temporarily move one ADC1 boundary within
   valid, ordered limits and confirm each field with **Enter**. ADC1 should use that boundary
   while ADC0 keeps its own table. ADC0 band bypass and **Disable 6m LNA on RX**
   must not bypass ADC1. The ADC1 master bypass must take precedence over its
   table. Restore the recorded values and confirm each field after the check.
5. Reconnect to confirm the saved filter settings. Select ADC0 on the next
   connection to return the active receiver to ADC0. Single-ADC profiles must
   refuse ADC1 rather than connect with an invented second input.

## Protocol 1 reception

P2 discovery accepts the raw **ProtocolSupported** value used by Thetis,
including Patroclus' value `2`; it is not restricted to decimal version
numbers. If you switch the emulator from P1 to P2, allow discovery to refresh
before selecting the device. Patroclus uses the same MAC for all its profiles,
so saved model/override/protocol choices also refer to that same identity.
For a directed connection, explicitly choose **Protocol 2** if a P1 preference
was previously saved. This does not change the emulator's firmware/profile.

A P1 timeout now distinguishes **no P1 Discovery reply** from **no P1 IQ after
discovery**. Neither message means a P2 stream has been decoded as P1.

Discovery and **Connect by IP** also probe Protocol 1. The actual reply selects
P1 or P2; the model name does not select the protocol. The gateware label shows
**P1 Gateware** or **P2 Gateware**. One discovery row is retained per MAC address.

Experimental P1 receive profiles are implemented for Hermes, Hermes II,
Angelia, Orion, Orion MkII and HermesC10. They use the existing receiver DSP,
with independent per-slot tuning and shared-window slices. These P1 paths have packet and transport tests
but still require hardware qualification. The existing Hermes-Lite 2 connection
continues to use its own backend.

Standard ANAN P1 profiles use **48, 96 or 192 ksps**. **Red Pitaya also
supports 384 ksps under P1**, including live zoom. The operator has confirmed
ANAN-7000DLE P2 and Red Pitaya P1/384 ksps reception on hardware; the additional
logical-model choices require their own hardware checks.

During P1 live zoom, a locally refused rate-control packet leaves the previous
rate in the periodic controls so a later retry can apply the requested rate.
P2 likewise retains its cached DDC rate and packet-gap tracking when the local
transport refuses a rate-control packet; a later accepted retry applies it.
This does not detect packets lost on the network or confirm the radio's rate.

### Choose the exact model and protocol

In **Connect by IP**, choose **Radio type → ANAN / OpenHPSDR (P1/P2)**, then:

1. Set **Receive profile** to your exact model. The transceiver choices are
   ordered as HERMES, ANAN-10, ANAN-10E, ANAN-100, ANAN-100B,
   ANAN-100D, ANAN-200D, ANAN-7000DLE, ANAN-8000DLE, ANAN-G2E, ANAN-G2,
   ANVELINA-Pro III and RED-PITAYA. The original Saturn implementation follows
   as a separate choice. Orion MkII, ANAN-G2 1K, HERMES-LITE 2 and
   OpenHPSDR / Atlas / Metis are omitted from this picker; existing saved
   profiles and physical-board detection remain valid. For Hermes-Lite 2,
   use its dedicated **Radio type**.
2. Set **Protocol** to **Protocol 1**, **Protocol 2**, or **Automatic** to
   accept a responding protocol. This selects discovery; it does not change
   radio firmware.
3. Choose **Sample rate** and connect. For Red Pitaya P1, choose **384 ksps**.

The choice is saved per MAC address and used on reconnect. **Use saved model**
and **Use saved preference** retain the existing choice. To clear a model
selection, choose **Automatic physical-board profile**, set override **OFF**, and reconnect.
Discovery retains the reported board family. The selected **Profile** and
**Override ON/OFF** appear separately alongside IP and ID/MAC, so two devices
with the same effective profile remain distinguishable. A board-family ID
cannot identify an exact SKU; use **Set Nickname…** for a name such as
"DIY Red Pitaya". A mismatched model is
refused unless **Override detection (ON)** explicitly selects the alternate
hardware profile described above.

### Inspecting and forgetting a mapping

With override ON, the main operating interface (radio tabs, model label and
default station name) shows the selected model, for example **ANAN-200D**.
The connected radio tab uses the running session's profile even if its
discovery-list entry still contains an older model. ANAN discovery changes
also refresh the radio tabs and switcher.
A custom nickname still takes precedence over the default station name.
The connection dialog continues to show the reported physical board family
and the separate mapping details. FlexRadio, Icom and Hermes-Lite 2 retain
their existing model and nickname behavior.

After connection, reopen **File → Connect to Radio...** to inspect the
**Connected device** details above the footer: reported board family, nickname,
IP, ID/MAC, profile, override state and actual P1/P2 protocol. These details
describe the configuration used to establish the current session.

To remove a saved model/override association, disconnect, select that device
under **On This Network → Available radios**, and click **Forget**. Only the
selected device's model and override are reset. Protocol preference, nickname,
DSP, calibration and other connection settings remain. The device stays in
the list and continues to appear through discovery, now using its physical
board profile with override OFF. **Forget** is disabled when there is no
mapping to remove or while connected; its tooltip explains the reason.
Discovery-list rows always resolve the current saved mapping, including when
an older discovery snapshot arrives after **Forget**.

Existing Red-Pitaya-P1 settings keep their model, P1 preference and 384 ksps
ceiling. Red Pitaya P2 is an explicit model choice too: it uses two ADCs,
Orion-class RX DDC2 and the sourced legacy HPF policy, including when its
firmware reports a Hermes identity. Its P2 hardware qualification is pending.
Anvelina P1 includes its additional control bank with extra OC outputs zero;
P2 also leaves those outputs zero. Transmit and accessory-output control are
not enabled.

Hermes-Lite 2 remains on the dedicated **Hermes-Lite 2** P1 connection type;
there is no HL2 P2 integration. Atlas/Metis initialization and the planned
Saturn-MkII board ID are not enabled. ANAN-G2/G2 1K use the existing P2 path;
a distinct G2 P1 discovery identity still needs verification.

Wider saved P2 rates are limited to the selected P1 profile's maximum. RF gain
and ANT1/2/3 use receive controls only. P1 does not load P2 droop tables or
advertise the P2 calibration sweep or P2 supply-voltage telemetry. The
application supports multiple slices and independent receive windows within the connected hardware limits.
Zoom requests snap to a supported sample rate. The displayed span confirms
the actual rate even when the request leaves that rate unchanged or arrives
while a receiver is being configured.

## CTUN: tune inside a fixed spectrum

Open **Settings → Radio Setup... → ANAN Hardware → Radio Info** and enable
**CTUN — tune within a fixed spectrum**. The adjacent readout shows CTUN,
the hardware **DDC centre** and the received **RX** frequency independently.
The switch applies immediately. CTUN starts off for a new hardware configuration;
a saved band view can restore it on reconnect or band selection. Filter and hardware
switches also apply immediately.

With CTUN on, tuning within the captured band moves the receive marker and
the DSP tuning offset while the hardware centre and spectrum stay fixed.
The audio follows the RX marker independently of the hardware centre.
The usable window is 92% of the sampled bandwidth, with room for the receive
filter and CW offset. Tuning beyond this window recentres the capture at the
new receive frequency. Narrowing the sample rate also recentres if the
receiver would otherwise fall outside the usable window. An explicit spectrum
drag moves the capture centre while retaining the received frequency when it
still fits. Turning CTUN off recentres at the received frequency and restores
normal tuning, where the spectrum follows the dial.

P1 and P2 use the same CTUN policy. ALEX/LPF/OC frequency selection continues
to follow the physical DDC centre; the hardware page reports that centre's
commanded filters. CW retains its pitch offset in addition to the CTUN shift.
CTUN is unavailable during a droop sweep; turn it off before starting a sweep.

## Regional band boundaries and band view

Two thin red vertical lines mark each amateur band's lower and upper edge
in the spectrum. The edges follow the selected **Band Plan** region and
remain visible when the coloured band-plan strip is off. Only edges within
the displayed span appear. A channelized band's outer envelope does not
make its internal gaps available for transmission; consult the band-plan
strip and your licence conditions.

On an ANAN, click **B** beside the spectrum zoom buttons to centre the
current RX band and select a suitable supported sample rate. This enables
**CTUN** and preserves the RX frequency, mode and receive filter. The
sample rate may change, so allow the DSP rebuild to finish; the spectrum
updates when the new capture is ready. The status bar acknowledges the
request rather than claiming that hardware has already completed it.

The **B** button is highlighted while the backend reports an active band view,
including a restored view after reconnect. A refused request does not activate it.
Click **B** again to turn CTUN off and restore the span/sample rate used before
the first click. The current VFO frequency stays unchanged; with CTUN off the
normal view recentres on that received frequency. A second click also replaces
a band-fit request whose DSP rebuild is still in progress.

The displayed spectrum and waterfall then zoom to the band edges, leaving
about 1% of the band width on each side so both red lines remain visible.
This display zoom is independent of the hardware sample rate. For example,
20 m displays about 357 kHz around 14.175 MHz while P2 captures at 768 ksps.
The receive audio and IQ rate are unaffected by the extra display crop.
Manual span changes or turning CTUN off return to the full hardware view;
tuning outside the displayed window also releases the band display zoom.

Aether selects the smallest supported rate with room for the band and its
edge markers, up to the connected profile's limit. If the entire band
cannot fit, it uses the maximum supported span and keeps the received
signal and passband within that span. The status message identifies partial
coverage. Manual tuning, spectrum dragging or another span request cancels
an outstanding centring action. If no regional amateur band contains RX,
**B** explains why it cannot centre a band.

Band selection recalls each band's **last CTUN state (on or off), VFO
frequency, capture centre and display zoom**. Turning B off and then leaving
a band saves CTUN off; returning to that band keeps it off, even if an older
CTUN-on view exists. Mode and receive filter are retained with
that view so the recalled receiver remains consistent. A first visit uses the
normal band-button defaults. Direct frequency entry continues to honour the
frequency you enter rather than substituting an old band's VFO frequency.

Views are stored per physical radio and hardware configuration: logical model,
physical board, override selection, ADC and P1/P2 protocol. Changing a mapping
therefore does not borrow another configuration's view. Saved views survive
restarts; the last band's view is recalled after a validated connection unless
an explicit connection frequency was supplied. A recalled B view retains its
pre-B span, so **B** can also switch that restored view off.

**S** remains unavailable on ANAN. FlexRadio retains its existing radio-owned
band and segment zoom controls; other radio families retain their controls.
The red regional boundary lines apply to the shared spectrum display.

## Receiving with the G2

In the main panadapter menu, **ANT → RF Gain** controls receive attenuation.
Its negative value means attenuation: −10 dB applies 10 dB of attenuation;
0 dB removes it. The range follows the selected hardware profile, including
an explicit model override:

| Receive path | RF Gain range |
| --- | --- |
| ADC0, classic ALEX profiles: Hermes, ANAN-100/100B/100D/200D | −61 to 0 dB |
| ADC0, ANAN-10/10E, Orion MkII, ANAN-7000/8000, G2/G2E, Anvelina and native Red Pitaya profiles | −31 to 0 dB |
| ADC1/RX2 | −31 to 0 dB |

Automatic profiles for the implemented legacy boards use the classic ALEX
path. An ANAN-200D override on Anvelina or Red Pitaya selects the classic
ALEX policy. Above 31 dB, the continuous control adds 30 dB of ALEX attenuation
and sets the ADC attenuator to the remainder. MkII input relay bits are never
used as classic attenuator bits.

**ANT → Preamp** provides Thetis-style attenuation presets: 0/−10/−20/−30 dB,
plus −40/−50 dB on the classic ADC0 path. This historical name does not imply
positive gain or a separate amplifier switch. Classic presets use the ALEX
10/20 dB stages and, for −40/−50 dB, an additional 20 dB ADC attenuation.
Other profiles use the ADC attenuator. A non-preset slider value appears as
**custom** in the preset readout; cycling the button returns to a preset.

The ADCs retain separate settings; the classic ALEX stage belongs to ADC0.
The settings are restored per radio. Changing the receiver source and
reconnecting uses that ADC's remembered value. Single-ADC profiles refuse
ADC1 changes. Both protocols keep each ADC register within 0–31 dB.

- **Receive** with live tuning and zoom. Changing the zoom changes the DDC rate live, without rebuilding the session. Nonfinite or nonpositive zoom requests are ignored; finite requests wider than the available span select the protocol/model maximum. Both P1 and P2 accept finite receive frequencies within 0–61.44 MHz. Invalid live tuning requests leave the last valid frequency unchanged; an invalid connection frequency is refused before reception starts.
- **Panadapter** computed by WDSP's display analyzer: a ≥16384-point windowed FFT at one point per screen pixel. **FFT AVG** and **Wt Avg** drive it, and noise-floor auto-adjust works.
- **S-meter.**
- **NB** cycles Off → **NB** → **NB2** → Off: WDSP's two impulse blankers, run on this computer before the demodulator. **NB** silences each impulse; **NB2** reconstructs the blanked window from the samples either side, and shows an **NB2** fill selector (**Zero**, **Sample hold**, **Mean hold**, **Hold sample** or **Interpolate**) while it runs. Neither is offered in FM.
- **AGC-T** sets the AGC's maximum gain from −20 to 120 dB, 1.4 dB per slider step, starting at 71 (79.4 dB). With AGC off it sets the fixed gain instead, starting at 29 (20.6 dB). AGC-T and the AGC-off level are not remembered across launches; each launch starts at 71 and 29.
- **RF Gain** from −31 to 0 dB drives the G2's ADC step attenuator, and is restored per radio.
- **AF gain, mute and balance** apply. With **Send receive audio to the radio** on, the receiver's mute and volume apply to both the computer and the radio's speaker, and the title-bar speaker button mutes the radio's speaker too.
- Manual **Black Level** and waterfall **NB Blank**. See [Panadapter Controls](./panadapter-controls.md).
- **Status bar:** the **Network:** indicator follows the G2's link (received data and packet loss; round-trip time and jitter are not measured on this radio). The G2's supply voltage is shown in the status bar and the Meter applet. The G2 reports no PA temperature, so that reading is not shown.
- **Tools → Radio Health...**

Without a reference level calibration for the selected ADC, panadapter levels
are relative. See [Receive calibration](#receive-calibration).

## Receive calibration

Open **Settings → Radio Setup... → ANAN Hardware → Calibration**. Both
calibrations use an external reference carrier and never transmit. Values are
saved per radio MAC, selected model, reported board and P1/P2 protocol. ADC0 and
ADC1 have separate receive meter and spectrum offsets. An uncalibrated profile
starts with frequency factor 1 and zero offsets.

### Frequency

1. Apply a stable carrier with a known frequency. Tune RX1 exactly to that
   frequency, turn **CTUN** off and close additional receivers.
2. Choose a narrow sample rate, preferably **48 ksps**, and allow the spectrum
   to settle. A larger FFT and lower rate improve measurement resolution.
3. Enter the carrier frequency in **Freq Cal → Frequency (Hz):**, then click
   **Start frequency calibration**.
4. Wait for the completion message. The procedure averages ten fresh spectra
   after one second of settling and searches within ±2.5 kHz of the reference.

The reference must be unambiguous and at least 20 dB above the local noise
floor. Weak or competing carriers are refused. Changing receive settings,
disconnecting, or clicking **Cancel calibration** discards the measurement.
The procedure times out after 30 seconds without sufficient spectra.

**HPSDR Freq Cal Diagnostic → Correction factor:** also accepts a known factor
directly, from **0.999** to **1.001** (±1000 ppm); confirm with Enter or by
leaving the field. **Reset frequency correction** restores 1. The factor changes
P1 integer-Hz and P2 phase-word tuning, including independent receive streams;
displayed frequencies and band/filter routing retain their nominal values.
External 10 MHz reference switching is unavailable.

### Receive level

1. Feed a carrier of known input power into the selected ADC's receive path.
   Tune RX1 to the carrier, turn CTUN off and close additional receivers.
2. Select **AM** or **SAM** with a receive passband around the carrier, then
   allow the receive meter and spectrum to settle.
3. Enter the frequency and input power under **Level Cal → Frequency (Hz):**
   and **Level (dBm):**, then click **Start level calibration**.
4. Wait for completion. The page shows the ADC's independently measured meter
   and spectrum offsets, including compensation for commanded attenuation.

This is a single-reference level calibration, not an LNA/attenuator-step sweep
or a frequency-response measurement. The reference level must be between
−140 and −10 dBm; the same carrier-quality checks apply. **Reset level
calibration** clears the selected ADC's meter and spectrum offsets and retains
the oscillator factor and the other ADC's calibration. Filter resets do not
erase calibration. Saved values are retained when a measurement fails.

> **Upgrading from an older version:** **AGC-T** covers −20 to 120 dB, so a given slider position gives more gain than before (at 100, 120 dB instead of 60 dB). Band-stack bookmarks saved earlier replay their slider position on this scale, so they come back louder. Lower **AGC-T** on each one and save it again.

## Droop Correction

The G2's DDC loses a little amplitude near the edges of the displayed span. AetherSDR applies a correction derived from Saturn gateware automatically on the G2. These shipped tables are not applied to the ANAN-7000DLE; it can use a sweep measured and saved for that radio.

To measure your own radio instead, open **Settings → Radio Setup... → Droop Correction** while connected:

1. Disconnect the antenna or terminate it in a dummy load. The sweep uses the receiver's own noise floor as a flat reference, so a live signal biases the result.
2. Click **Start Sweep**. It steps through every DDC0 sample rate and takes several minutes. **Stop** ends it early.
3. Review the measured correction per rate, then click **Apply** to use and save it for this radio, or **Discard**.

> **Upgrading from an older version:** G2 panadapter tones now read about **7.4 dB lower** than they did before the WDSP panadapter (a test tone that read −6.0 dB now reads −13.4 dB). Re-trim a saved reference level or waterfall black level once.

## Reference

### What is remembered

The G2 stores none of its settings, so AetherSDR keeps these per radio: frequency and mode (14.175 MHz USB on a first connect), tuning step, **RF Gain**, the radio's own output level (what the volume slider sets with PC Audio off) and the panadapter dBm range. The span always starts at the connect form's **Sample rate:**. AGC settings start from their defaults.

For all supported ANAN profiles, **FFT FPS**, **FFT AVG**, **Wt Avg** and waterfall **Rate** are saved separately for each radio and panadapter. Reconnecting or restarting AetherSDR restores these choices and applies them to the receive display processing. Averaging **0** and **Wt Avg** off are remembered too. Temporary adaptive frame-rate limits do not replace your saved **FFT FPS** choice.

### Not available on the G2

- **Transmit.** Keying is refused; transmit is a future phase.
- DAX, SmartLink or Multi-Flex.
- A headphone level control: the **Headphone:** row under **Settings → Radio Setup... → Audio → Radio Audio Outputs** is dimmed; **Line Out:** stays available.

Controls the G2 cannot honor are dimmed with a reason, or refuse with a notice; see [Supported Radios](./supported-radios.md).

## Known issues

- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### Nothing is received at all

If the signal path is wrong after changing RF filter options, first restore the Saturn BPF1 selection.

1. Connect to the G2.
2. Open **Settings → Radio Setup... → Radio → ANAN Hardware → Antenna / ALEX**.
3. Turn off the ADC0 master bypass under **Alex-1 Filters** to enable automatic filtering. The change applies immediately. ADC1/RX2 bypass is configured separately under **Antenna Control**.

### No audio from the radio's own speaker

**Send receive audio to the radio** is off, or was turned on during the session. It takes effect on the next connect.

1. Open **Settings → Radio Setup... → Radio → ANAN Hardware → Connection Defaults** and tick **Send receive audio to the radio**.
2. Disconnect and connect again.
3. Check that the receiver and the title-bar speaker button are not muted; they mute the radio's speaker too.
4. Check that **Mute the radio's speaker amplifier** is unticked.

If AetherSDR closed without disconnecting, or the network cable was pulled, while the amplifier was muted, the G2 keeps it muted. Connect again with **Mute the radio's speaker amplifier** unticked.

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/architecture/anan-p2-backend-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/anan-p2-backend-design.md): background for developers

### OC table and log layout

**ANAN Hardware → OC Outputs** places **User Dig Out** above the **HF**,
**VHF** and **SWL** tabs. In **HF**, **Enable RX OC outputs** and
**Reset OC defaults** sit above the RX/TX pin matrices. All seven pin columns
(**P1–P7**) and all supported band rows (**160m–6m**) are visible without
scrolling inside a matrix. The matrices sit side by side when space permits
and stack in a narrow setup window.

**TX Pin Action mapping** and **Live OC pin state** sit below the matrices.
The seven LEDs and hexadecimal mask show the commanded RX output byte,
not physical pin feedback. **Reset OC defaults** clears the RX assignments
and disables RX OC outputs immediately. It leaves the receive
filters and antenna settings alone.

TX controls and the additional **2m**, **GEN**, **WWV** and
**XVTR** routing rows remain dimmed with an explanation because these
functions are not implemented. The existing RX matrix covers 160m through
6m; its ranges continue to come from the active regional band plan.

**User Dig Out → Pin 1–4** independently selects the four manual DB9 user
outputs on Protocol 1 and Protocol 2. Multiple pins can be selected together;
these outputs do not follow the HF/SWL band matrices or their master switch.
New configurations start with all four pins off. Selections are saved per radio
MAC, selected model and physical board, and restored on reconnect. The command
bits are cleared on a normal receive stop; the interface shows commanded state,
without physical pin readback. The receive-filter reset clears the saved pins.
Physical availability follows the connected device's firmware and wiring.

**SWL** provides separate seven-pin RX assignments for **LMF**, **120m**, **90m**,
**61m**, **49m**, **41m**, **31m**, **25m**, **22m**, **19m**, **16m**, **14m**,
**13m** and **11m**. The enable switches in HF and SWL operate the same master
switch, initially off. Assignments are saved per radio MAC, selected model and
physical board and applied automatically on Protocol 1 and Protocol 2.

The initial SWL ranges follow Thetis's non-US, non-extended defaults. Hover the
band names to inspect exact limits. At shared SWL edges the higher-frequency
row owns the first integer Hz. HF ranges from the active regional plan are
seeded when first assigning SWL pins; saved HF ranges and masks are retained.
HF takes priority even when its mask is zero; masks are never ORed together.
Routing follows the existing OC receive capture-center frequency. Independent
WWV, GEN and transverter routing remains unavailable; this is not a complete
copy of Thetis's regional band-stack classifier.

**Reset SWL pin assignments** clears only SWL masks and preserves HF and the
master switch. **Reset OC defaults** in HF clears HF masks and disables the
shared master switch, leaving SWL assignments saved. The general receive-filter
reset clears both matrices. Outside all configured ranges, and on stream stop,
the commanded OC pins are off. TX remains unavailable.

Under **Help → Support & Diagnostics... → Application Logs**, the ANAN Protocol 1,
Protocol 2, RX DSP, Panadapter, Droop Defaults and Droop Calibration filters
occupy one column, directly below one another.
