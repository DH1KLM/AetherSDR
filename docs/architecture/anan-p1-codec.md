# ANAN Protocol 1 receive codec

`src/core/backends/anan/P1Protocol.*` implements socket-free wire primitives.
It is compiled into `aethercore` and covered by `anan_p1_protocol_test`.
It is connected through `P1Client` and the backend-private `AnanClient`
dispatcher. Both P1 and P2 transports use the existing ANAN IO thread. The
Hermes-Lite 2 backend remains separate and unchanged.

## Scope

The codec encodes discovery, IQ-only start/stop, general receive settings,
receive VFOs, ADC routing, manual receive filters, step attenuation and a CW
disable command. EP2 combines internally encoded controls with 48 kHz stereo
speaker samples. No caller-supplied control bytes, MOX parameter, transmit VFO,
drive value or transmit samples are accepted. C0 bit 0 is always clear, drive
is zero, Disable HF PA is set, and all transmit-IQ payload bytes are zero.

The configuration accepts 48/96/192/384 ksps, one to seven interleaved slots,
one or two ADCs, ANT1/2/3 and 0–31 dB step attenuation. These are wire bounds,
not claims that every board supports every combination. The transport must
validate its selected board profile before constructing a configuration.
Filter bytes are P1 bank-local bits; P2 Alex word masks cannot be substituted.
The transport supplies them from the existing verified Thetis frequency tables;
P2 relay positions are translated to P1 bank-local bits before encoding.

EP6 decoding checks the exact 1032-byte length, endpoint 6 and both frame
syncs. Each 512-byte frame has `floor(504 / (6 * receivers + 2))` complete
IQ/microphone rounds. Microphone words and trailing padding are skipped.
Views borrow the datagram; the transport must decode before releasing it.
The selected physical slot is explicit. Invalid slot/geometry leaves the
output vector unchanged. Normalization uses the existing AetherSDR full-scale
convention, 8388607.

## Evidence and implementation provenance

This is an independent implementation of observed protocol fields, not a
source-code port. The following pinned primary sources establish the wire
facts and behavioral differences:

- Thetis commit `852bf0ef0b4f3886a13fc2846489aee16f361872`,
  [ChannelMaster/networkproto1.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/networkproto1.c):
  EP6 round geometry/status decoding, receive-frequency Hz representation,
  general settings, ADC routing and control bank field positions.
- NereusSDR commit `12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb`,
  [P1CodecStandard.cpp](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/codec/P1CodecStandard.cpp):
  confirms microphone-PTT-disable polarity and Disable HF PA, and distinguishes
  Hermes four-slot, Hermes II two-slot and Orion five-slot layouts.
- The same NereusSDR revision,
  [P1CodecRedPitaya.cpp](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/codec/P1CodecRedPitaya.cpp)
  and
  [P1CodecAnvelinaPro3.cpp](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/codec/P1CodecAnvelinaPro3.cpp):
  model identity is necessary for differences beyond the shared board ID.
  Extra OC output and transmit-state attenuation behavior are outside this
  receive codec's scope.
- Existing AetherSDR `hl2/MetisProtocol.*`: common Metis packet framing,
  discovery offsets, and the application's IQ normalization convention.
  No HL2 gain, I2C, EADDR or transmit encoder is reused.

## Receive session integration

`P1Client` accepts only its configured IPv4 sender and UDP source port 1024.
It requires an idle discovery reply with a supported board profile and matching
selected identity before priming RX control banks and starting IQ. Busy radios
are rejected without sending stream or control packets. RX1 is P1 frame slot 0.
The known Thetis layouts are four slots for Hermes/HermesC10, two for Hermes II,
and five for Angelia/Orion/Orion MkII. Only RX1 is decoded into the existing DSP.

P1 wire IDs normalize into the existing board registry: 4 → Angelia 3,
5 → Orion 4, 10 → Orion MkII 5. In particular P1 ID 10 never becomes Saturn.
The normalization matches NereusSDR `RadioDiscovery.cpp` at the pinned revision.
Standard P1 sessions expose 48/96/192 ksps, matching `BoardCapabilities.cpp`
`sampleRatesFor` and Thetis `setup.cs` (847–849 at the pinned Thetis revision).
An explicit Red Pitaya P1 model override additionally exposes 384 ksps
(rate code 3), including live zoom. It uses five slots, two ADCs, unswapped
speaker channels and legacy HPF policy, even if discovery reports Hermes.
The generic P1 profiles do not advertise or send 384 ksps.

Thetis `clsHardwareSpecific.cs` at the pinned revision (185–190) selects two
ADCs, legacy filter policy and no speaker swap for Red Pitaya.
`NetworkIO.cs` accepts Hermes or Orion MkII identity for that model.
Thetis `setup.cs` (847–849) explicitly includes the extra 384 ksps P1 rate
for Red Pitaya; NereusSDR `BoardCapabilities.cpp:sampleRatesFor` confirms it.

The manual receive-profile selector writes an exact-radio `HardwareProfile`
version-2 document through `AnanSettings` after discovery supplies the MAC.
Version-1 Red Pitaya P1 documents remain readable.
It never writes an unknown identity/family default, never inherits a family
model override, preserves other fields, and refuses corrupt/unavailable/newer
schemas. The next manual attempt defaults back to saved profile use.
Initial connect and automatic reconnect read the override for that exact MAC;
The logical model also selects the P2 receive profile; protocol preference is
stored separately.

EP2 control/audio continues at 48 kHz even with speaker audio disabled: speaker
slots contain silence and transmit-IQ slots always contain zero. A 5 ms precise
timer accumulates fractional 126-frame packet credit, bounded to four packets
per tick after a stall. Speaker playout primes 4096 samples and consumes only
whole packets so DSP block tails are retained rather than padded every block.
The audio queue retains at most 8192 samples. A refused
periodic send terminates the session and does not inflate transmitted-byte counters.
Live rate changes build a candidate general-bank packet and commit the new
control rate only after the local transport accepts it. A refused rate write
returns failure, retains the previous rate in periodic controls and remains
retryable. Repeated requests for the same rate still send a packet. Transport
acceptance is not firmware acknowledgement; the existing backend rate retries
remain in use.

The dispatcher forwards only the selected transport's signals. P2 remains the
default when discovery protocol is unspecified. Discovery emits one row per MAC
and prefers a responding P2 interface unless this MAC has the explicit
saved P1 preference. A missing preferred protocol ages out after two sweeps
rather than retaining stale identity forever. The preference policy has
socket-free tests for both directions. Explicitly selecting Protocol 1 in the manual form sends only a P1 probe;
Protocol 2 sends only a P2 probe.
Initial connect, automatic reconnect and the retained wake path carry protocol
and normalized board ID through the existing connection request.

P1 does not apply saved P2 droop tables or Saturn defaults, and it does not
advertise the P2 calibration sweep or P2 voltage telemetry. P1 rate limits also
bound the panadapter's live zoom. The protocol comes from the reply, not the
model display string.

## Remaining qualification

Hardware qualification is required before calling these P1 profiles verified.
Shared physical board IDs cannot distinguish Red Pitaya or Anvelina from
standard ANAN hardware. Red Pitaya P1 therefore requires the explicit choice
above. The logical-model selection and Red Pitaya P2 path are documented in
[model profiles](anan-model-profiles.md).
Atlas/Metis, Saturn MkII and Hermes-Lite P2 are not activated by
this step; HL2 P2 is out of scope. No radio firmware is changed automatically.

## Validation

`anan_p1_protocol_test` covers exact bytes, RX-only control boundaries, channel
swap, signed IQ, malformed packets, and truncation at every EP6 length.
`anan_p1_transport_test` injects datagrams and captures sends without a socket
or simulated radio. It covers each normalized board/layout, sender validation,
busy/profile rejection before any control, slot-0 RX1, bounded pacing/queues,
failed writes, live rate refusal and retry across every implemented P1 logical
model (including Red Pitaya's 384 ksps), duplicate discovery, sequence wrap/gaps and
inactive-transport suppression. Existing P2 packet/profile/audio/telemetry tests
remain regression checks.

Logical-model selection now uses the [shared model catalogue](anan-model-profiles.md);
its schema-3 document stores a separate protocol preference and an explicit
board-override flag. Version-1 Red Pitaya P1 and version-2 model/protocol
documents remain readable, with board override OFF.
