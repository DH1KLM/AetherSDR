# ANAN logical model integration

Logical model, reported board ID and wire protocol are separate values.
`AnanModel.h` contains the full Thetis logical ID set 0–16, plus automatic.
The end sentinel and Nereus-only board slots are not treated as radio models
or added to the discovery wire registry.

## Resolved receive profiles

| Logical model(s) | Native normalized board | ADCs | P1 slots | P2 first RX DDC | ADC0 RF filter |
|---|---:|---:|---:|---:|---|
| Hermes, ANAN-10/100 | 1 | 1 | 4 | 0 | Legacy HPF |
| ANAN-10E/100B | 2 | 1 | 2 | 0 | Legacy HPF |
| ANAN-100D | 3 | 2 | 5 | 2 | Legacy HPF |
| ANAN-200D | 4 | 2 | 5 | 2 | Legacy HPF |
| Orion MkII, ANAN-7000DLE/8000DLE | 5 | 2 | 5 | 2 | MkII BPF |
| Anvelina Pro 3 | 5 | 2 | 5 | 2 | MkII BPF |
| ANAN-G2 / G2 1K | 10 | 2 | Not enabled | 0 (existing bench path) | MkII BPF |
| ANAN-G2E | 20 | 1 | 4 | 0 | MkII BPF |
| Red Pitaya | 5 (also accepts 1) | 2 | 5 | 2 | Legacy HPF |

P1 wire IDs are normalized before model matching: raw 4 → Angelia 3,
raw 5 → Orion 4, raw 10 → Orion MkII 5. Thetis accepts Hermes or Hermes-II
reports for the ANAN-10/10E/100/100B logical selections; the selected model
then determines the effective receive layout. Red Pitaya accepts Hermes or
Orion MkII reports but resolves to the two-ADC Orion-class layout. Unknown
models and incompatible board/model combinations fail before receive setup
unless an explicit supported board override is enabled.

Standard P1 profiles use 48/96/192 ksps; Red Pitaya additionally supports
384 ksps. P2 retains the existing six-rate ladder. Anvelina P1 emits bank
0x26 with all extra OC outputs zero. P2's extra-OC byte 1397 remains zero.
No transmit or active accessory output is introduced.

## Per-radio selection and compatibility

The manual form supplies a model selector and a separate protocol selector.
The backend receives typed logical model data, independently of discovery's
normalized board ID. Display names never select packet behavior.

`AnanSettings::radioProfile` reads exact-MAC `HardwareProfile` documents:

- Schema 1: `redPitayaP1=true` maps to Red Pitaya + P1; false maps to automatic.
- Schema 2: `model` is a validated logical ID; `protocol` is 0/1/2.
- Schema 3: adds a required boolean `overrideDetection`. Schemas 1/2 always
  read as override OFF. ON requires an explicit supported model.
- Missing exact row means automatic. Family-wide choices are not inherited.
- Corrupt, unavailable, malformed or newer schemas are not silently applied.
- Writes require a known MAC, preserve other fields, check success, and refuse
  future schemas. An explicit model/protocol choice can repair a known-schema
  document whose values are invalid.

The initial connect, reconnect and retained connection paths read the model
for the exact radio. Discovery prefers P2 by default and P1 for a saved P1
preference, with the existing stale-preference aging policy. Manual explicit
P1/P2 selection probes only that protocol. Automatic manual probing accepts a
valid response; the resulting connection carries that actual protocol.

## Explicit board override

The operator-authorized Zeus-style override selects the complete effective
profile, including ADC/DDC layout, filter and antenna encoding, stereo swap
and board-dependent telemetry/default selection. It does not split digital
and analog board selection. Example: reported Orion MkII ID 5 plus selected
ANAN-200D resolves to two ADCs, DDC2 and legacy HPF; P1 uses five slots.
The actual normalized discovery ID remains separate and is still compared
against the selected device ID during session validation. Busy replies,
unsupported physical boards/models and HL2 P2 remain refused.

The manual page's three-state checkbox selects ON, OFF, or the exact-MAC saved
setting (partial state). The complete model/protocol/override object is saved
on connect; the form then returns to saved selection. Reconnect and automatic
discovery consume the same object, with no global override inheritance.

Behavioral reference: Zeus commit `1aa1d8632df7fe4b63cf49f1bac937793f6f309a`,
[RadioService.ConnectedBoardKind](https://github.com/abhishekprakash22/zeus/blob/1aa1d8632df7fe4b63cf49f1bac937793f6f309a/Zeus.Server.Hosting/RadioService.cs#L4737-L4775)
and [RadioSelector](https://github.com/abhishekprakash22/zeus/blob/1aa1d8632df7fe4b63cf49f1bac937793f6f309a/zeus-web/src/components/RadioSelector.tsx#L235-L267).
Thetis remains the authority for each selected model's wire behavior.

## Identity, presentation and forgetting

`RadioInfo::model` retains the reported board family. The display-only
`connectionProfile` describes the effective model and override separately;
it never selects a codec. Both discovery paths use the same ANAN presentation
helper. Discovery's change detection includes this description so a mapping
edit refreshes the row even if the reported model does not change. IP and
ID/MAC are always shown on these rows, with an independent custom nickname
when present. A shared board byte cannot prove a specific product identity.

`RadioModel::lastRadioInfo()` captures the profile at the connect boundary.
The connection dialog displays this snapshot while connected, independently
of list selection and subsequent settings changes.

For an explicit valid override, the ANAN display helper additionally sets
`effectiveModel`. Generic `RadioInfo::displayModel()` / `displayNickname()`
provide the operating UI's selected model and replace only a missing or
model-fallback nickname. RadioModel's initial identity and radio-tab labels
use these accessors. `model`/`boardId` and connection-dialog details keep the
reported identity; packet behavior remains selected by typed model/protocol
parameters. With no effective override, the accessors return the original
model/nickname, including for FlexRadio, Icom and HL2.

The disconnected selected-row **Forget** action resets only the model and
override fields in the exact-radio HardwareProfile document. It preserves
protocol preference, unknown sibling fields and every other feature. Schema-1
Red Pitaya's implicit P1 preference is retained during normalization. Future
schemas and unavailable/corrupt storage fail closed. This is neither device
deletion nor a discovery blacklist; the row is immediately refreshed and
normal discovery continues.

## Qualification boundaries

Discovery byte 12 is retained as an opaque Thetis `ProtocolSupported` value,
not a bounded decimal version. Thetis `clsRadioDiscovery.cs` at
`852bf0ef0b4f3886a13fc2846489aee16f361872` reads it directly. Patroclus
`RadioProfile.CreateDiscoveryResponse` at
`8c2752230d8afe702169ae70e99cbaad287c70f5` emits value 2 for P2 profiles.
The packet signature selects P1/P2, not byte 12. Existing MAC/length/signature
validation remains; P2 session discovery is limited to source port 0 or 1024
to keep status/IQ endpoints from authorizing the session. Socket-free fixtures
cover Patroclus' shared-MAC P1→P2 transition and the normal P2 receive path.
This is packet/transport compatibility coverage, not qualification of the
emulator's complete stream implementation or every physical radio.

The operator confirmed ANAN-7000DLE P2 and Red Pitaya P1/384 ksps using commit
`2ae00c58`. The added logical-model choices and Red Pitaya P2 have source and
packet/transport tests; this is not hardware qualification of every listed SKU.
The retained Saturn DDC0 path is intentionally unchanged.

HL2 stays with its dedicated P1 backend; HL2 P2 is explicitly out of scope at
the operator's instruction. Atlas/Metis initialization remains unqualified.
Saturn MkII ID 11 is a planned variant in Thetis/Nereus and is not silently
treated as Saturn. Thetis contains a G2-class P1 slot algorithm but no verified
distinct G2 P1 discovery identity here; the G2 models remain P2-only in this
implementation until that identity is established.

## Pinned primary sources

Thetis commit `852bf0ef0b4f3886a13fc2846489aee16f361872`:

- [clsHardwareSpecific.cs](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/clsHardwareSpecific.cs): logical model → hardware, ADC count, BPF policy and stereo swap.
- [NetworkIO.cs](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/HPSDR/NetworkIO.cs): cross-board model compatibility; P1 Hz and P2 phase-word tuning.
- [networkproto1.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/networkproto1.c): Anvelina bank 17 / C0 0x26.
- [network.c](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/network.c): Anvelina extra-OC byte 1397.

NereusSDR commit `12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb`:

- [HpsdrModel.h](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/HpsdrModel.h): exact logical IDs and distinction from physical board IDs.
- [HardwareProfile.cpp](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/HardwareProfile.cpp): effective hardware and cross-board logical-model matching.
- [P2RadioConnection.cpp](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/core/P2RadioConnection.cpp): codec dispatch uses the effective model hardware.

This is independent implementation of the documented wire facts, not a source
port. The model registry, P1/P2 production handlers, settings migration and
Anvelina bank are covered by socket-free tests. No simulated radio peer runs.

## Receive filter setup

`Anan Hardware` uses the session's resolved `receiveProfile(reportedBoard,
model, overrideDetection)`, not the discovery display name. Legacy HPF and
MkII BPF defaults remain byte-equivalent to the existing P2 profiles with ADC1
bypassed. The single receiver's tuned frequency selects both ADC filter banks
when their bypasses are off. The receive frequency table is shared between
ADC0 and ADC1 in this implementation. Independently editable RX2 limits are
not exposed; ADC0 per-band bypass flags are independent of ADC1 selection. P2 Saturn retains its existing disabled
LPF control; P1 retains its existing LPF bank selection.

The source reference is Thetis commit
`852bf0ef0b4f3886a13fc2846489aee16f361872`:
`console.cs` (`setBPF1ForOrionIISaturn`, `setAlexHPF`, `setAlex2HPF`,
`setAlexLPF`), `setup.designer.cs` (frequency defaults), and ChannelMaster
`netInterface.c` / `network.h` (`SetAlexHPFBits`, `SetAlex2HPFBits`, relay bit
fields). Alex0 receive bits are 6,5,4,1,2,3 in ascending table order; Alex1 uses
those same bit positions in its separate register. Bypass is bit 12.

The version-3 `HardwareFilters` document belongs to the exact ANAN MAC scope.
Children are keyed by logical model and physical board; changing an override
cannot transfer edited limits to a different model. Reads use defaults for
missing or invalid data. Writes validate all ranges, reject future/corrupt
schemas and check persistence before dispatching the live update. `hw.set`
also checks the page's identity/model/board against the current session.
`hw.defaults` is an explicit persisted reset; polling `hw.get` never writes.

The new GUI translation unit includes only the existing RadioModel extension
surface and ThemeManager. Their generated touchpoint counts each increase by
one; no concrete ANAN or wire header crosses into the new page. Relay displays
are commanded state, without hardware readback. Hardware acceptance and a full
Windows GUI build remain operator checks.


### ALEX packet comparison, 2026-10-17

Operator captures of Anvelina with the ANAN-200D mapping showed Aether changing
Alex0 correctly at 3.8/14.225 MHz (`01400040` / `01100002`) but leaving the
upper Alex1 relay word zero. The working Thetis capture at 3.65/14.175 MHz
also carries `0140` / `0110` at bytes 1428–1429. Its ADC0 HPF was bypassed;
that is an operator setting, not a reason to copy bypass into Aether.

For profiles that already select receive LPFs, the encoder now also writes
ANT1/2/3 and LPF bits into Alex1's upper word, independently of the low ADC1
filter word. This follows Thetis `SetAntBits` and `SetAlexLPFBits` in
ChannelMaster/netInterface.c at the source revision above. No T/R, transmit
status or PTT bit is copied. The LPF-disabled Saturn profile is unchanged.

The same capture shows ordinary RX OC output masks `0x0b` (80 m) and `0x05`
(20 m), encoded as byte 1401 `0x16` / `0x0a`. Aether's byte is zero. These are
site-specific settings, not universal ANAN defaults; other band masks and the
physical wiring cannot be inferred from two captured bands. A hardware relay
fix claim therefore still requires operator acceptance and clarification of
whether the filter board is wired to ALEX SPI or these OC outputs.


## Nereus-style hardware configuration surface

Layout reference: NereusSDR commit `12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb`,
[`src/gui/setup/HardwarePage.cpp`](https://github.com/boydsoftprez/NereusSDR/blob/12f2ea5f3ad5b4e6b91903d3a1f8612f6e671ecb/src/gui/setup/HardwarePage.cpp)
and its `hardware/{RadioInfoTab,AntennaAlexTab,OcOutputsTab,CalibrationTab}.cpp`
children. Aether uses its own widgets and theme tokens with that category and
sub-tab arrangement. This is a layout correspondence, not parity with every
Nereus backend function. Unavailable TX, independent ADC1 limits, VHF/SWL OC,
USB BCD, user digital output and calibration functions are explicitly labelled.
The existing P2 droop page remains the sole sweep owner.

HF receive OC is operator-enabled and defaults off with zero pin masks.
Ranges are seeded from the current BandPlanManager plan, persisted with the
MAC/model filter document and never guessed from a physical board ID. Version
1 documents read with OC disabled; version 2 prevents older filter writers
from silently discarding OC configuration. Up to eleven ordinary HF receive
band ranges drive seven pins. Configured nonzero-mask ranges must be ordered,
non-overlapping integer Hz within the supported 0–61.44 MHz receive range.
P2 writes mask << 1 at byte 1401; P1 writes it at bank 0 C2. Both match Thetis
network.c/networkproto1.c. A normal stop clears active outputs; no TX, T/R,
extra Anvelina OC or PA-sequencing bit is generated by this feature.

The operator subsequently confirmed that the current Anvelina + ANAN-200D
filter relays are not wired to OC. This feature serves other explicitly
configured RX accessories; it is not claimed as the fix for that ALEX issue.


## Model-gated Alex-2 and RX filter indicators

Alex-2 Filters is shown only when the resolved profile has MkII filtering and
two ADCs. Legacy ANAN-100/100D/200D and Red Pitaya profiles do not acquire an
Alex-2 board merely by having two ADCs. In particular an Orion MkII explicitly
mapped as ANAN-200D hides the tab. The UI consumes this backend fact, not a
model-name comparison. The single-ADC G2E profile also hides it.

HPF/BPF and LPF rows have theme-token green LEDs, with selected/not-selected
accessible names. They follow the same configured-band selection as the
encoder, independently of pending edits. Bypass has a separate indicator and
LPF-disabled profiles leave LPF LEDs off. These are commanded states, not
physical relay readbacks. The operator reported the relay issue still present
after the first Alex1 compatibility correction; a current Aether capture is
needed to verify the post-correction wire state before changing more bits.

Schema 3 adds six ADC0 band-bypass flags, Disable 6m LNA on RX and forced 6m
receive LPF. Older documents default these options off and keep their OC
configuration. Thetis console.cs setAlexHPF/setBPF1ForOrionIISaturn selects
receive bypass for a bypassed band or disabled 6m LNA, and setAlexLPF chooses
6m when LPF bypass is requested. Those rules are applied only to ADC0; ADC1
uses its independent bits at the shared tuned frequency. P1 translates both
words independently rather than deriving ADC1 from ADC0's overridden bits.
No TX, PureSignal or TX-LNA control has been enabled by these RX options.
