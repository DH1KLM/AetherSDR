# ANAN Protocol 1 receive codec

`src/core/backends/anan/P1Protocol.*` implements socket-free wire primitives.
It is compiled into `aethercore` and covered by `anan_p1_protocol_test`.
It is not yet connected to discovery or `AnanBackend`; the confirmed Protocol
2 receive path and the Hermes-Lite 2 backend do not call it.

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
They remain explicit inputs until logical-model filter policy is integrated.

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

## Remaining integration

P1 sessions need board/model-specific slot selection, filter selection,
discovery identity/protocol propagation, sender validation, start/stop and
timeout handling, EP2 pacing, and transport integration on the existing ANAN
IO thread. P1 has four rate codes; it must not expose P2's 768/1536 ksps rates.
Shared board IDs cannot choose a logical SKU automatically. Socket-free
transport tests must precede making P1 devices connectable.

Tests cover exact control bytes, unsupported/TX address rejection, all accepted
addresses keeping MOX clear, speaker swap including odd short input, zero TX
payload, one-to-seven-slot EP6 framing, signed IQ extremes, every truncated
EP6 length, both frame syncs, and malformed configuration/geometry.
