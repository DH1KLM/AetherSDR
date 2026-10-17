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

Connection options:

| Option | Default | What it does |
|--------|---------|--------------|
| **Sample rate:** | 48 ksps | Receiver sample rate (G2 DDC0, 7000DLE DDC2): 48, 96, 192, 384, 768 or 1536 ksps. It is also the starting width of the panadapter. Higher rates use more of the radio's Ethernet link. |
| **Select ADC:** | ADC0 (ANT1/2/3) | Which receive chain feeds the receiver: **ADC0 (ANT1/2/3)**, behind the switched antenna relays, or **ADC1 (RX2)**, wired straight to the RX2 jack. |
| **Dither** / **Random** | On | The ADC's linearisation bits. Leave them on unless you are testing. |
| **ADC0 RF filter bypass** | Off | MkII/Saturn BPF1 is selected automatically from the tuned frequency using the Thetis-compatible band map. Check this only to force ADC0 around that filter. |
| **ADC1 RF filter bypass** | On | RX2/ADC1's separate filter bank still uses bypass; automatic ADC1 band selection is not implemented yet. |
| **Send RX audio to the radio's speaker** | Off | Also plays receive audio through the G2's own speaker and headphone jack. Takes effect on the next connect. |

## Protocol 1 reception

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

## Receiving with the G2

- **Receive** with live tuning and zoom. Changing the zoom changes the DDC rate live, without rebuilding the session.
- **Panadapter** computed by WDSP's display analyser: a ≥16384-point windowed FFT at one point per screen pixel. **FFT AVG** and **Weighted** averaging drive it, and noise-floor auto-adjust works.
- **S-meter.**
- **NB** (noise blanker): WDSP's impulse blanker, run on this computer before the demodulator.
- **RF Gain** from −31 to 0 dB drives the G2's ADC step attenuator, and is restored per radio.
- **AF gain, mute and balance** apply. With **Send RX audio to the radio's speaker** on, the receiver's mute and volume apply to both the computer and the radio's speaker, and the title-bar speaker button mutes the radio's speaker too.
- Manual **Black Level** and waterfall **NB Blank**. See [Panadapter Controls](./panadapter-controls.md).
- **Tools → Radio Health...**

The panadapter is not dBm-calibrated: its levels are relative.

## Droop Correction

The G2's DDC loses a little amplitude near the edges of the displayed span. AetherSDR applies a correction derived from Saturn gateware automatically on the G2. These shipped tables are not applied to the ANAN-7000DLE; it can use a sweep measured and saved for that radio.

To measure your own radio instead, open **Settings → Radio Setup... → Droop Correction** while connected:

1. Disconnect the antenna or terminate it in a dummy load. The sweep uses the receiver's own noise floor as a flat reference, so a live signal biases the result.
2. Click **Start Sweep**. It steps through every DDC0 sample rate and takes several minutes. **Stop** ends it early.
3. Review the measured correction per rate, then click **Apply** to use and save it for this radio, or **Discard**.

> **Upgrading from an older version:** G2 panadapter tones now read about **7.4 dB lower** than they did before the WDSP panadapter (a test tone that read −6.0 dB now reads −13.4 dB). Re-trim a saved reference level or waterfall black level once.

## Reference

### Not available on the G2

- **Transmit.** Keying is refused; transmit is a future phase.
- DAX, SmartLink or Multi-Flex.
- More than one receiver or panadapter.

Controls the G2 cannot honour are dimmed with a reason, or refuse with a notice; see [Supported Radios](./supported-radios.md).

## Known issues

- Weak signals sound quieter than in Thetis on the same radio unless the **AGC-T** slider is near the top ([#5988](https://github.com/aethersdr/AetherSDR/issues/5988)).
- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### Nothing is received at all

If the signal path is wrong after changing RF filter options, first restore the Saturn BPF1 selection.

1. Disconnect from the G2.
2. Open **Connect by IP** with **Radio type:** set to **ANAN / OpenHPSDR (P1/P2)**.
3. Uncheck **ADC0 RF filter bypass** so the tuned-frequency BPF1 selection is active. Keep **ADC1 RF filter bypass** checked when using RX2 until its separate filter selector is implemented.

### No audio from the radio's own speaker

**Send RX audio to the radio's speaker** is off, or was turned on during the session. It takes effect on the next connect.

1. Tick **Send RX audio to the radio's speaker** in the ANAN-G2 options.
2. Disconnect and connect again.
3. Check that the receiver and the title-bar speaker button are not muted; they mute the radio's speaker too.

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/architecture/anan-p2-backend-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/anan-p2-backend-design.md): background for developers
