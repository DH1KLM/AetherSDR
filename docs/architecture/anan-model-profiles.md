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
SaturnMKII ID 11 is an unverified enum entry, not an established model profile,
and is not silently treated as Saturn. The [source audit](anan-saturn-id11-verification.md)
records the checked Thetis branches and public Saturn firmware identity paths. Thetis contains a G2-class P1 slot algorithm but no verified
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
when their bypasses are off. ADC1 may have its own receive frequency table;
without one it inherits ADC0 ranges. ADC0 per-band bypass flags remain
independent of ADC1 selection. P2 Saturn retains its existing disabled
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


## General refresh: capture comparison and qualification limit

The complete `aether-alex(3).pcapng` startup capture contains one General
packet with phase-word mode `08`, watchdog `01`, PA disable `01` and ALEX
enable `03`. ALEX0 changes from `01400040` at 3.8 MHz to `01100002` at
14.225 MHz; ALEX1 is `01401000` / `01101000`, including the previously
missing upper word and this session's ADC1 bypass. Thus the capture confirms
the initial enable was sent and the corrected filter words reached the host
NIC. It does not acknowledge their application by the radio.

The working Thetis capture repeats General during RX, whereas Aether sent it
only at startup. Thetis ChannelMaster/network.c `KeepAliveLoop` at revision
`852bf0ef0b4f3886a13fc2846489aee16f361872` refreshes General every 500 ms
while running with the watchdog enabled. Aether now repeats its exact
validated General packet on that cadence, using its existing I/O-thread
keepalive timer and a monotonic deadline. High Priority remains at 100 ms.
Retries neither depend on IQ arrival nor catch up in bursts after a stall;
stop/restart clears the cached configuration. No DUC or TX stream is added.

Socket-free production-scheduler tests discard the initial configuration and
check recovery packets despite live IQ, the model-specific enable/PA bytes,
repeat spacing, late-tick coalescing, pre-discovery refusal and teardown.
This fixes the absence of General retries; UDP loss at the radio and the
cause of Anvelina's relay failure are not established by the capture. Relay
operation after the change still requires a physical device test.


## Operator acceptance and independent ADC1 limits, 2026-10-18

DH1KLM confirms actual band-change relay activity on Anvelina P2 with an
ANAN-200D override after build `c27a720e`. HPF bypass and Disable 6m LNA on RX
also appear to work in the operator's initial test. This qualifies that
reported combination, not every profile, P1, OC wiring or a new independent
ADC1 table. General retry coverage remains necessary; these observations do
not by themselves prove which initial UDP packet the radio applied.

For dual-ADC MkII profiles already exposing Alex-2, that page now edits six
independent ADC1 frequency ranges and displays ADC1's commanded band/bypass
LEDs. The technical reference is Thetis setup.cs
`udAlex21_5HPFStart_ValueChanged` through `udAlex26BPFEnd_ValueChanged`, which
maintain a separate Alex-2 range table. Existing model defaults and bit mapping
are retained. P2 encodes ADC1 in the existing ALEX1 lower word; P1 translates
that selection to its bank-local bits. ADC0, LPF, OC, RX frequency and TX gates
are unaffected. Alex-2 stays visible for legacy mappings and single-ADC models, with its
filter controls disabled and a visible / accessible explanation. The effective
profile's `hasAlex2Filters` still gates editing and inclusion of `receiveAdc1`
in the apply request; a second ADC alone does not enable Alex-2.

HardwareFilters schema 4 adds optional `receiveAdc1` (six integer-Hz ranges,
ordered, non-overlapping, within 0–61.44 MHz). Missing tables continue to
inherit the saved ADC0 ranges, including on old documents and on legacy models
whose page has no Alex-2 editor. Apply on an Alex-2 page saves its explicit
ADC1 table atomically with the other settings, scoped by MAC/model/board.
Future schemas remain protected against overwrite. Restore model defaults
removes the separate table and restores inherited model ranges and bypasses.
Independent-table physical qualification on an appropriate MkII model remains
pending; an ANAN-200D mapping does not expose this editor.


## Main integration, 2026-10-18

Merge main `1f8a3fc1` (64 previously unmerged commits) into the ANAN branch
based on `6e6f530f`. Preserve physical/model identity, P1/P2 probing, overrides,
per-device filters and the operator-confirmed relay path. P2 General refresh,
P1/P2 filter encoders and independent ADC1 tables are unchanged by this merge.

Adopt main's ANAN Front End page for next-connect sample rate, ADC choice,
dither/randomization, initial bypass and radio-speaker defaults. Keep
model/protocol/board-override choices on Connect by IP, and per-device live
filter controls under Anan Hardware. Correct main's pre-filter-support bypass
instructions to describe automatic selection and per-device precedence. Both
pages coexist in the new CanonWindow setup workspace. Extend main's directed
IPv4 broadcast sweep with both P1 and P2 requests while retaining the existing
MAC/protocol de-duplication policy. Retain both branches' settings tests and
main's capability record and valid-rate accessor.

Targeted protocol/profile/audio/telemetry, settings and main's broadcast-target
tests qualify the merged receive paths in software. A full Windows application
build and physical discovery/RX/override/relay regression check remain operator
checks; source and targeted checks do not substitute for those.


## Receive antenna ownership and operator UI acceptance, 2026-10-18

DH1KLM confirms the corrected filter-table layout and subsequent placement of
the four receive/TX-option checkboxes below the table. Droop expansion remains
deferred by operator instruction.

ANT1/2/3 live selection uses the existing P1/P2 wire encoders. Its previous
family-global persistence allowed one device's choice to affect another's
next connection. ReceiveAntenna schema 1 now stores integer choices 1–3 in an
exact MAC-scoped feature with model/physical-board children. Missing or invalid
values use the existing legacy global default; live selection no longer
updates that default. Corrupt/unavailable storage and future schemas refuse
writes. The backend saves before updating live state or dispatching a command,
and requires an active validated session and known identity. Profile load
restores the per-device antenna for both transports, including address-only
connection after the board identity arrives. No transmit path is introduced.


## Antenna acceptance and profile regression coverage, 2026-10-18

DH1KLM confirms the per-device receive-antenna change works on the operator's
setup. This does not qualify every model or a second ADC antenna path.

The socket-free model-profile test now exercises ANT1/2/3 across every
implemented physical-board/explicit-model override combination. At 3.8, 14.2
and 51 MHz, with receive bypass both enabled and disabled, it checks the
P2 ALEX antenna bits (including the profile-dependent shared upper word)
and verifies that every other packet byte remains unchanged. P1 checks its
bank-0 selector and preservation of ADC routing, tuning, OC, filters and
attenuation in every accepted control bank. Saturn G2/G2 1K remain excluded
from P1, and the dedicated HL2 backend is not part of this matrix. MOX/PTT
remain clear. These are encoder regressions, not new hardware qualification
or expanded protocol support. Droop expansion remains deferred.


## Receive tuning boundary, 2026-10-18

The device-swap audit found that ADC attenuation restoration resets missing
state and hardware-filter loading clears the previous session's optional
configuration before exact-device lookup. No additional change to those
ownership rules is made.

P2 live and connection tuning now use P1's existing finite 0–61.44 MHz
inclusive range. The backend rejects nonnumeric connection values before
starting DSP, and rejected connection/live input preserves the previous dial.
P2Client independently validates before phase-word conversion and preserves
its previous word on refusal. Valid tuning, filter selection and existing
restore-frequency semantics are unchanged; this does not qualify operation
at the sampling boundary.

The injected P2Client test covers negative, oversized, NaN and infinite
values plus valid/boundary tunes without a socket. Backend tests add live
and connect refusal checks for both protocols. The backend test's supply-rail
fixture now uses the actual AnanClient dispatcher type rather than its former
P2Client pointer. Targeted P2Client tests run locally; the full backend-linked
test still requires the complete application dependency build.


## Live receive-rate validation, 2026-10-18

P2 applies the same six-rate allow-list during live DDC changes that it
already uses during discovery validation. Unsupported rates or inactive
logical DDC indices produce no packet and leave the cached session and
sequence expectations unchanged. Packet preparation is socket-free and shared
with the actual live send path and does not mutate the cached rate or sequence
expectation. Only a fully accepted local write commits the new rate and resets
that DDC's sequence expectation when its rate changes. A failed or incomplete
write returns false and preserves the previous state. This is local transport
acceptance, not an acknowledgement from the radio. Valid repeated requests
still send full DDC-Specific packets for the existing UDP-loss retries and
retain gap detection. Sibling DDCs, physical offsets,
ADC routes and dither/randomization flags are retained. No restart or new
thread is introduced.

The injected test covers all implemented physical P2 board profiles, every
supported rate, rejected rates/indices, refused/incomplete writes for both
logical DDCs, accepted-byte counters, sibling preservation, retry equality and
sequence reset behavior. Its private sender injection drives the public live
setter without opening sockets. The backend also rejects nonfinite/nonpositive
zoom requests before conversion and clamps oversized finite spans before
narrowing to integer ksps. Backend regression cases retain P1's 192 ksps
ceiling, Red Pitaya's 384 ksps ceiling and P2's 1536 ksps ceiling. These tests
do not qualify a live radio's response to a rate change.

The targeted P2 receive-profile tests pass under undefined-behavior and
float-cast-overflow sanitizers. Removing only the rate allow-list check in an
external mutation causes the invalid-rate and state-preservation checks to
fail. Backend and backend-test syntax checks pass; the full backend-linked
test and Windows build remain unrun locally.


## Active input and antenna-save feedback, 2026-10-18

The Antenna Control page labels the ANT1/2/3 bank as ADC0 rather than RX1.
Its input readout follows the existing hardware status ADC index, antenna
number and effective profile ADC count; single-ADC profiles no longer imply
a fitted RX2 input. ADC1 reception leaves ADC0 relay control available and
explicitly states that this does not change the active RX2 route. This is
commanded session state, not physical relay feedback.

Antenna changes now use nonzero request IDs through the page's existing
backend result/error guard. The partial antenna reply updates only the
antenna field of the current hardware snapshot without marking filter edits
clean. Refused saves restore the displayed last-confirmed antenna under a
signal blocker and surface the backend error. Antenna replies/errors are
ignored if MAC/model/board changed before delivery. The active route label
announces actual text changes through the accessibility channel and resets
on disconnect. Protocol, persistence and hardware defaults are unchanged.

The modified GUI translation unit passes syntax checking with an external
forward declaration for Qt MotionPreference (local Qt 6.4 vs pinned 6.12).
Accessibility and engine-boundary checks pass. A full GUI build and interactive
Windows verification remain operator checks.


## Dedicated ADC0 receive inputs

Antenna Control adds an Apply-based ADC0 input selector alongside the existing
immediate ANT1/2/3 relay-bank selector. HardwareFilters schema 5 adds optional
integer `rxOnlyInput` (0 ANT bank, 1/2 external, 3 XVTR). Older documents omit
the field and retain source 0. Settings remain exact MAC/model/physical-board
children, use the existing save-before-dispatch path, reject malformed values
and protect future schemas. Restore model defaults selects source 0 without
changing the separately owned ANT1/2/3 choice.

Pinned Thetis `852bf0ef0b4f3886a13fc2846489aee16f361872` provides:

- `Console/HPSDR/Alex.cs:350–378`: RX-only source selection and RX_OUT=1 for
  a nonzero input, independently of ANT-bank selection. Aether exposes the
  physical RX connector without transverter mode, band automatic selection,
  TX override, external ATU or RX_OUT override.
- `ChannelMaster/netInterface.c:459–481`: source 1 selects ALEX bit 10, source
  2 bit 9, source 3 bit 8. RX_OUT uses bit 11 on classic ALEX; MkII source 1
  also uses bit 11, while MkII sources 2/3 use RX master selection bit 14.
- `ChannelMaster/network.h:279–285`: those register bit positions.
- `ChannelMaster/networkproto1.c:452–462`: P1 general-bank C3 bits 5–6
  encode the selector, bit 7 RX_OUT, bit 0 MkII RX master selection.
- `Console/setup.cs:19886–20490` and `clsHardwareSpecific.cs:143–190`: model
  connector labels and independent MkII/legacy RF-control policy. Red Pitaya
  retains its explicit legacy policy even though its input label is BYPASS.

The generic ORIONMKII logical model has no explicit Thetis setup-label case.
It uses the EXT2/EXT1 wire names from SetAntBits, consistent with the named
Nereus `SkuUiProfile.cpp` grouping at `66bc1e43b65c0ffbe5d2c62812355e97f2f8f06a`;
this does not identify the connectors fitted to an unknown DIY enclosure.
Automatic profiles use native board-family names; explicit mappings use the
selected model. Labels are returned by the backend in the existing hardware
status map, so no protocol/model branching is added above the seam.

Socket-free tests pin all four RX selections across the implemented
board/model override matrix, including P1/P2 byte differences and return to
ANT-bank input. Other filters, ADC1, OC, attenuation, frequency and PTT/MOX
remain unchanged. Injected P1/P2 setters and per-device persistence tests cover
propagation, refusal and defaults. Physical RX-connector qualification is
pending; no TX, per-band antenna matrix, frequency-offset transverter support,
new ID-11 profile or expanded droop support is introduced.

Validation: model-profile, P1/P2 protocol, injected P1/P2 transport and settings
tests pass. External mutations suppressing P2 input selection or replacing
MkII P1 master selection with classic RX_OUT each fail the exact-bit tests.
The user guide builds, and boundary/accessibility/settings/registration
checks pass. GUI/backend syntax checks use external Qt-6.4 compatibility
declarations; a full Windows application build was not run locally. An
isolated Qt form probe checks ten width/input combinations for wrapping and
overlap; it is not a full-application UI test.

## ADC1 routing and independent filter qualification

The model-profile test covers ADC1's separate six-row table across every
implemented physical-board/logical-model override combination. Each row is
checked at its integer-Hz lower edge, midpoint and upper edge, with ADC0
bypassed, all ADC0 band bypasses enabled and each dedicated ADC0 input chosen.
ADC1's lower ALEX word must select the independent row or its master bypass;
single-ADC profiles must emit neither ADC1 filter bits nor ADC1 attenuation.
ADC source selection retains the effective model's physical RX DDC. Alex-2
availability requires both MkII filtering and two ADCs, including under a DIY
override; two ADCs alone do not make an ANAN-200D an Alex-2 profile.

The injected P1 transport test covers every implemented P1 logical model and
each fitted ADC. It verifies the emitted startup routing bank 0x1c, keeps IQ
decoding on wire slot zero when ADC1 is selected, and exercises the independent
ADC1 table's edges and master bypass through the live filter setter and
retuning path. Relay masks follow the pinned Thetis netInterface.c/network.h
and P1 bank-local translation in networkproto1.c described above.

These checks verify client encoding and routing only. The user guide's
[ADC1 and Alex-2 check](../user/docs/anan-g2.md#check-adc1-and-alex-2) separates
source selection, ADC0 relay isolation, independent boundaries and reconnect
recall on the actual fitted hardware. No new receiver, hardware capability,
profile, TX function or physical relay readback is introduced by this audit.


### OC page layout

The OC page follows the arrangement in
[NereusSDR's OC Outputs page](https://github.com/boydsoftprez/NereusSDR/blob/main/src/gui/setup/hardware/OcOutputsHfTab.cpp):
User Dig Out above the category tabs, RX/TX band matrices below the enable
and reset row, then TX pin actions beside live commanded pin state. The
matrix uses a widget grid rather than a table viewport, so P1–P7 cannot be
clipped by table-header minimum section sizes. Matrix and lower groups
reflow vertically in narrow windows. Unsupported rows and actions are
visible but disabled; they do not extend the backend's RX-only contract.


### Antenna and filter page layout

Antenna Control and the Alex pages follow the public NereusSDR
[`AntennaAlexAntennaControlTab`](https://github.com/boydsoftprez/NereusSDR/blob/main/src/gui/setup/hardware/AntennaAlexAntennaControlTab.cpp),
[`AntennaAlexAlex1Tab`](https://github.com/boydsoftprez/NereusSDR/blob/main/src/gui/setup/hardware/AntennaAlexAlex1Tab.cpp)
and
[`AntennaAlexAlex2Tab`](https://github.com/boydsoftprez/NereusSDR/blob/main/src/gui/setup/hardware/AntennaAlexAlex2Tab.cpp)
arrangement. Per-band RX/TX antenna matrices and TX options are unavailable
previews; current RX routing retains its existing controls and persistence.
Filter rows are widget grids with aligned LEDs and MHz-suffixed edge fields.
Whole banks reflow rather than clipping the final row or frequency field.

`hw.get` includes a read-only `defaultFilterEdges` object with `receive` and
`lowPass` arrays derived from the effective profile's existing model defaults.
The page reset stages these edges in its editor and uses the existing
`hw.set` / Apply path. It neither invokes `hw.defaults` nor changes bypass,
OC or antenna state. Alex-2 edits and reset remain gated by `hasAlex2Filters`;
its master bypass is a second view of the existing ADC1 bypass field.
Independent ADC1 per-band bypass and Alex-2 LPF are disabled with reasons.
No new wire controls or capability flags are added.
