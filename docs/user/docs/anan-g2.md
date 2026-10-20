---
title: "ANAN / OpenHPSDR"
slug: "/anan-g2"
description: "Experimental OpenHPSDR P1/P2 reception; P2 tested with ANAN-G2 and ANAN-7000DLE."
status: "Experimental, receive-only"
applies_to: ["ANAN-G2/G2 1K", "ANAN-7000DLE/8000DLE", "ANAN-10/100", "ANAN-10E/100B", "ANAN-100D", "ANAN-200D", "ANAN-G2E", "Anvelina Pro 3", "Red Pitaya"]
---

:::info[Status]

**Status:** Experimental, receive-only · **Applies to:** ANAN-G2/G2 1K, ANAN-7000DLE/8000DLE, ANAN-10/100, ANAN-10E/100B, ANAN-100D, ANAN-200D, ANAN-G2E, Anvelina Pro 3, Red Pitaya

:::

AetherSDR can receive with an Apache Labs **ANAN-G2** over openHPSDR Protocol 2. Support is **experimental and receive-only**: one receiver, one panadapter, no transmit yet. FlexRadio remains the supported target (see [Supported Radios](./supported-radios.md)). An **EXPERIMENTAL** badge shows in the title bar while a G2 is connected.

The **ANAN-7000DLE** has an experimental Orion MkII receive profile. It uses
physical DDC2, two ADCs, and Thetis-compatible ADC0 BPF1 and antenna-chain
LPF selection. Discovery displays the Orion MkII family because board ID 5
is shared by several models. This profile has protocol and transport test
coverage; reception on an ANAN-7000DLE has been confirmed by the operator.
It provides one receiver and one panadapter, with transmit unavailable.

Additional experimental P2 RX profiles cover **ANAN-10/100 (Hermes)**,
**ANAN-10E/100B (Hermes II)**, **ANAN-100D (Angelia)**,
**ANAN-200D (Orion)** and **ANAN-G2E (HermesC10)**. Packet and transport
tests cover these profiles; reception on their hardware is not yet confirmed.
The radio must run Protocol 2 firmware.

On Hermes, Hermes II and HermesC10 select **ADC0 (ANT1/2/3)**: these boards
have one ADC, and ADC1 is refused. Legacy boards use their ALEX HPF table;
HermesC10 uses MkII BPF selection. All use frequency-selected antenna-chain
LPFs. The application still provides one receiver and one panadapter.
Hermes/Hermes-II radio-speaker channels follow Thetis' stereo swap.
Supply voltage for these additional profiles is not reported until qualified.

## Where to go next

- [Panadapter Controls](./panadapter-controls.md): FFT AVG, Black Level and NB Blank on the G2's panadapter.
- [RX Controls](./rx-controls.md): tuning, AF gain, mute and balance.
- [DSP Noise Mitigation](./dsp-noise-mitigation.md): client-side noise reduction.
- [Memory Channels](./memory-channels.md): stored on this computer.

## Setup

The G2 is found automatically on your LAN: open **File → Connect to Radio...** → **On This Network**, select it and click **Connect Selected Radio**.

Discovery also sends a directed broadcast to each attached IPv4 subnet. This can find a G2 on a second NIC with its own subnet. Two NICs sharing a subnet still use the preferred route; VPNs and routers may block broadcast discovery. If the G2 does not appear, use **Connect by IP** with **Radio type:** set to **ANAN / OpenHPSDR (P1/P2)** (see [Manual Connection](./manual-connection.md)).

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
therefore select incorrect filters or ADC/DDC routing. Transmit remains
unavailable.

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

Configure the receive front end under **Settings → Radio Setup... → Radio → ANAN Front End**. The page is available while disconnected, and while connected to a G2. Its options apply on the next connection, whether you use **Available radios** or **Connect by IP**:

| Option | Default | What it does |
|--------|---------|--------------|
| **Sample rate:** | 48 ksps | Receiver sample rate (G2 DDC0, 7000DLE DDC2): 48, 96, 192, 384, 768 or 1536 ksps. It is also the starting width of the panadapter. Higher rates use more of the radio's Ethernet link. |
| **Feed the receiver from:** | ADC0 (ANT1/2/3) | Which receive chain feeds the receiver: **ADC0 (ANT1/2/3)**, behind the switched antenna relays, or **ADC1 (RX2)**, wired straight to the RX2 jack. |
| **Dither** / **Randomization** | On | The ADC's linearisation bits. Leave them on unless you are testing. |
| **Bypass ADC0's bank** | Off | MkII/Saturn BPF1 is selected automatically from the tuned frequency using the Thetis-compatible band map. Check this only to force ADC0 around that filter. |
| **Bypass ADC1's bank (RX2)** | On | Bypass RX2/ADC1's separate filter bank. Uncheck to select its receive filter automatically at the single receiver's tuned frequency. |
| **Send receive audio to the radio** | Off | Also plays receive audio through the G2's own speaker and headphone jack. Takes effect on the next connect. |

## ANAN Hardware

Open **Radio Setup → Radio → ANAN Hardware**. The page is always listed,
including before connection; device controls are disabled until an ANAN has a
validated session. It follows the Nereus hardware-page arrangement:

- **Radio Info**: **Board Identity** (physical board, P1/P2, ADC count, reported
  receiver count, firmware, MAC, IP) and **Operating Parameters**. The effective
  model and override remain distinct from the physical board. One receiver is active.
- **Antenna / ALEX**: **Antenna Control**, **Alex-1 Filters**, **Alex-2 Filters**.
  **Antenna Control** places the TX safety options above TX and RX per-band
  antenna matrices, followed by the TX bypass options. These per-band and TX
  controls are dimmed with an explanation; ANAN per-band antenna routing and
  transmit are not implemented. **Current RX Antenna** below the matrices
  contains the working **ADC0 input**, **ANT bank**, and **ADC1 RF filter bypass**
  controls. ANT1/2/3 changes apply immediately; input and bypass edits require
  **Apply**. ADC1 keeps its dedicated RX2 input. Settings remain separate for
  this MAC address, effective model and physical board.

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

  **Reset filter defaults** stages only the frequency edges for the selected
  page: ADC0 and available LPF on Alex-1, ADC1 on Alex-2. Bypass, antenna and
  OC settings are kept. Select **Apply** to save. **Restore model defaults**
  remains the separate action that restores the whole hardware-filter
  configuration, including bypass and OC defaults.
- **OC Outputs**: **HF**, **VHF**, **SWL**. HF has RX/TX pin matrices side by side
  and a commanded RX pin-state readout. The RX matrix is configurable; TX, VHF,
  SWL and user digital outputs are visible but disabled
  with their unsupported reason.
- **Calibration**: visible frequency, level, TX-display and current-calibration
  sections explain unavailable functions. **Open Droop Correction** opens the
  existing P2 droop sweep; it is disabled for P1.

An explicit model override determines the filter configuration; otherwise the
selected compatible model or physical-board profile is used. FlexRadio, Icom
and HL2 retain their own hardware settings.

**Antenna Control → ADC0 input** selects the ANT1/2/3 path or a dedicated
receive input. Select **Apply** to save and activate it for this MAC, effective
model and physical board. Names follow the selected model, including DIY
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
Control** with **Apply**, or change the ADC source on **ANAN Front End** and
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
With one receiver, both automatic ADC filter banks follow its tuned frequency.
ADC1 inherits ADC0 ranges until independent Alex-2 limits are saved. ADC0's individual **Bypass** boxes
apply to that HPF/BPF band only. **Disable 6m LNA on RX** selects ADC0 bypass
instead of the 6-m filter/LNA; **6m/ByPass on RX** forces the 6-m LPF. These RX
options take effect after **Apply** and do not bypass ADC1's separate input
filter. On models with **Alex-2 Filters**, ADC1 has its own six frequency
ranges and commanded-filter LEDs. Its master bypass still takes precedence.
**ADC1 RF filter bypass** is on **Antenna / ALEX → Antenna Control** for every
dual-ADC profile, including ANAN-100D/200D and a DIY ANAN-200D mapping whose
Alex-2 controls are disabled. Select **Apply** to save and activate it. Single-ADC
profiles display the control disabled with a reason. Alex-2 retains its bypass
indicator and independent band table on supported MkII profiles.
The single receiver frequency drives both banks; changing ADC1 limits does
not create another receiver or alter ADC0 limits. TX/PureSignal bypass and
**Disable 6m LNA on TX** remain disabled because ANAN transmit is unsupported.
The 6m LNA and TX/PureSignal bypass switches are grouped above the HPF/BPF
filter grid; **6m/ByPass on RX** is below the LPF grid.

The **Receive-only settings. Apply saves…** note is directly below the HPF/BPF and LPF tables on **Alex-1 Filters**.

**Apply** and **Restore model defaults** are above the hardware tabs, with
the pending/saved status directly below. Complete filter banks retain their
required height; use the page’s vertical scrollbar in a short window.

Select **Apply** to save for this MAC and model and update the running receiver;
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
Check the wanted RX pins per band and **Enable RX OC outputs**, then **Apply**.
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
off. LEDs update with tuning, independently of pending table edits, and expose
selected/not-selected names for screen readers. They do not confirm physical
relay movement.

**Commanded ADC0 / ADC1 / LPF** indicates the software's requested filter,
not physical relay feedback. Pending edits are marked **Changes pending** until **Apply** succeeds. The
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
   ANAN Front End** and set **Feed the receiver from:** to **ADC1 — the RX2 jack**.
   Disconnect and reconnect to apply the source change. Confirm that **ANAN Hardware →
   Antenna / ALEX → Antenna Control** reports **Active receiver: ADC1 / RX2**.
   Connect the receive signal to the fitted ADC1 input; the software cannot
   detect which physical connector is wired to a DIY board.
2. While receiving on ADC1, change **ADC0 input**, select **Apply**, and change
   **ANT bank**. They may
   switch ADC0 relays, but must not select a different source for the active
   ADC1 receiver.
3. If this hardware supports **Alex-2 Filters**, record its current values,
   clear **ADC1 RF filter bypass** on **Antenna Control** and select **Apply**.
   Return to **Alex-2 Filters** and tune across a saved
   ADC1 band boundary. Its band LED should change; verify the corresponding
   hardware relay or receive response separately.
4. To distinguish the two tables, temporarily move one ADC1 boundary within
   valid, ordered limits and select **Apply**. ADC1 should use that boundary
   while ADC0 keeps its own table. ADC0 band bypass and **Disable 6m LNA on RX**
   must not bypass ADC1. The ADC1 master bypass must take precedence over its
   table. Restore the recorded values and select **Apply** after the check.
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
with one receiver and panadapter. These P1 paths have packet and transport tests
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

1. Set **Receive profile** to your exact model. The list includes Hermes,
   ANAN-10/10E/100/100B/100D/200D, Orion MkII, ANAN-7000DLE/8000DLE,
   ANAN-G2/G2 1K/G2E, Anvelina Pro 3 and Red Pitaya.
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
application continues to provide one receiver and one panadapter.

## CTUN: tune inside a fixed spectrum

Open **Settings → Radio Setup... → ANAN Hardware → Radio Info** and enable
**CTUN — tune within a fixed spectrum**. The adjacent readout shows CTUN,
the hardware **DDC centre** and the received **RX** frequency independently.
The switch applies immediately. CTUN starts off for a new hardware configuration;
a saved band view can restore it on reconnect or band selection. The switch
is separate from the filter tables' **Apply** button.

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

Band selection recalls each band's last active **CTUN view, including VFO
frequency, capture centre and display zoom**, and enables CTUN again. Turning
B off temporarily keeps that active view in memory. Bands without a saved
active CTUN view recall their last normal view. Mode and receive filter are retained with
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

The panadapter is not dBm-calibrated: its levels are relative.

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

The G2 stores none of its settings, so AetherSDR keeps these per radio: frequency and mode (14.175 MHz USB on a first connect), tuning step, **RF Gain**, the radio's own output level (what the volume slider sets with PC Audio off) and the panadapter dBm range. The span always starts at the connect form's **Sample rate:**. AGC settings and FFT averaging start from their defaults.

### Not available on the G2

- **Transmit.** Keying is refused; transmit is a future phase.
- DAX, SmartLink or Multi-Flex.
- More than one receiver or panadapter.
- A headphone level control: the **Headphone:** row under **Settings → Radio Setup... → Audio → Radio Audio Outputs** is dimmed; **Line Out:** stays available.

Controls the G2 cannot honor are dimmed with a reason, or refuse with a notice; see [Supported Radios](./supported-radios.md).

## Known issues

- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### Nothing is received at all

If the signal path is wrong after changing RF filter options, first restore the Saturn BPF1 selection.

1. Disconnect from the G2.
2. Open **Settings → Radio Setup... → Radio → ANAN Front End**.
3. Uncheck **Bypass ADC0's bank** so the tuned-frequency BPF1 selection is active. ADC1/RX2 has a separate bypass option under **Radio Setup → ANAN Hardware**.

### No audio from the radio's own speaker

**Send receive audio to the radio** is off, or was turned on during the session. It takes effect on the next connect.

1. Open **Settings → Radio Setup... → Radio → ANAN Front End** and tick **Send receive audio to the radio**.
2. Disconnect and connect again.
3. Check that the receiver and the title-bar speaker button are not muted; they mute the radio's speaker too.

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
and disables RX OC outputs; select **Apply** to save. It leaves the receive
filters and antenna settings alone.

TX controls, User Dig Out and the additional **2m**, **GEN**, **WWV** and
**XVTR** routing rows remain dimmed with an explanation because these
functions are not implemented. The existing RX matrix covers 160m through
6m; its ranges continue to come from the active regional band plan.

Under **Help → Support & Diagnostics... → Application Logs**, the ANAN Protocol 1,
Protocol 2, RX DSP, Panadapter, Droop Defaults and Droop Calibration filters
occupy one column, directly below one another.
