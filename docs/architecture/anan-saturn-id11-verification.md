# Saturn board ID 11: source verification

Checked 2026-10-18 for the `anan-openhpsdr-all-models` branch.

## Decision

Keep P2 board ID 11 recognizable but not connectable, including through a
board override. The checked sources establish an enum name, not an
independent receive contract. Do not describe this as a confirmed new
commercial ANAN model or a verified Saturn PCB revision. The available
evidence is consistent with a reserved entry; that interpretation is not
proof that no ID-11 hardware or private firmware exists.

## Pinned sources and findings

| Source | Finding | Limit of the evidence |
|---|---|---|
| Thetis master `852bf0ef0b4f3886a13fc2846489aee16f361872`, `Console/enums.cs:399` and `ChannelMaster/network.h:424` | Both enums assign `SaturnMKII = 11`, with the comment `MKII board?`. | An enum does not specify ADCs, DDCs or RF control. |
| Same Thetis tree, source search for `SaturnMKII` | Only those two declarations occur. | No dedicated symbolic ID-11 setup path was found; generic P2 behavior alone is not a device specification. |
| Thetis `Console/clsHardwareSpecific.cs:164–176` | Both G2 and G2 1K select `HPSDRHW.Saturn`, two ADCs and MkII BPF policy. | This maps to ID 10, not evidence for ID 11. |
| Thetis `Console/HPSDR/clsRadioDiscovery.cs:1181` | P2 byte 11 is cast to the board enum. | Parsing an ID is recognition, not profile qualification. |
| Thetis `Console/HPSDR/NetworkIO.cs:162–181` | The normal model/board check compares discovery identity with the selected hardware; mismatches can be acknowledged by the operator. | Allowing a mismatch does not establish a separate MkII layout. |
| Thetis dev `0916a66b366d98e9b090ba3cb20a79e3b0f3e4e9` | Retains the same two enum declarations and question-mark comment. | The checked development branch supplies no separate symbolic ID-11 path. |
| Saturn main `4b0b76f345961cfeeb447abc6d8b0373f5743245`, `sw_projects/P2_app/p2app.c:418–429` | The default discovery reply uses byte 11 = 10, four DDCs and phase-word tuning. | This is the public Saturn ID-10 contract. |
| Same file, `p2app.c:587–603` | The `-i saturn` option selects 10; `-i orionmk2` selects 5; other strings are refused. | There is no ID-11 identity option in this code. |
| Saturn `sw_projects/common/version.c:233–248` | PCB revision is read separately from the FPGA product-version register. | A PCB revision number must not be treated as discovery board ID 11. |

The checked Saturn repository exposes only `main`; Thetis exposes `master`
and `dev`. These findings cover those snapshots, not every downstream fork,
closed firmware image or unreleased board. No radio traffic or physical
ID-11 device was available for this investigation.

## Nereus, Longpath and Zeus comparison

The operator requested this comparison during the audit. Acceptance by a
client and independent verification of a firmware contract remain separate.

| Checked client snapshot | ID-11 handling | Evidence status |
|---|---|---|
| NereusSDR `66bc1e43b65c0ffbe5d2c62812355e97f2f8f06a` | `HardwareProfile.cpp:289–295` maps SaturnMKII to ANAN_G2. `BoardCapabilities.cpp:1198–1208` explicitly derives its P2 capabilities from Saturn; the row enables two ADCs and Saturn-family features. | The row's TODO asks for hardware verification of differences. `phase3i-verification.md` lists SaturnMKII as unverified. This is an implemented compatibility policy, not a separate Thetis-derived ID-11 contract. |
| Longpath `aaa080a76de4b3f9f00da2dd1dccbda2b07d74a6` | `HardwareProfile.cpp:244–249` maps ID 11 to ANAN_G2, and lines 295–301 also permit G2/G2 1K selection. `BoardCapabilities.cpp:1091–1123` derives the capability row from Saturn. | The same source has an explicit TODO to verify differences when hardware is available. It supplies no independent ID-11 firmware evidence. |
| Zeus `86657f7228c96eba9cf63b19a7923de4f2ed20bf` | `docs/references/protocol-1/thetis-board-matrix.md` marks 11 as reserved. `Zeus.Contracts/HpsdrBoardKind.cs` has no distinct ID-11 enum member. | No independent ID-11 profile was found in the checked board contract. Do not infer P2 behavior from the mixed-protocol enum's naming. |

Nereus's `codec/AlexFilterMap.cpp:213–235` explicitly identifies inclusion
of SaturnMKII in BPF1 routing as a deliberate divergence from Thetis. Its
comment acknowledges that Thetis has only the two enum declarations. The
additional routing is a client decision based on treating ID 11 as G2-family,
not a newly discovered Thetis implementation.

The newer public Saturn fork `kd4yal2024/Saturn` was also checked at
`9bc0140a25e78454d1e7235a9abcaa4604e4bdbd`. Its `p2app.c` default reply
(line 1219) and identity options (lines 1412/1417) likewise use 10 or 5. This
additional firmware source does not supply an ID-11 discovery mode.

Adopting the Nereus/Longpath fallback could make Aether accept ID 11
experimentally. It would not establish hardware verification. This audit
does not enable that fallback.

## What would enable a profile

| Required evidence | Current status |
|---|---|
| Firmware source or vendor specification deliberately identifying the device as P2 ID 11 | Missing |
| ADC count, input routes, physical DDC assignment and limits | Missing for ID 11 |
| Supported sample rates, IQ format, clock/phase conversion and UDP ports | Missing for ID 11 |
| General enable/PA-disable requirements and ALEX/antenna/attenuator mapping | Missing for ID 11 |
| Exact firmware/hardware revision associated with those rules | Missing for ID 11 |
| Successful Thetis RX capture from that revision, followed by Aether device tests | Missing |

Once the wire contract is available, implement a distinct receive profile and
its socket-free packet/transport tests. Physical qualification then checks
discovery, RX, tuning, rate changes, ADC routing, antennas and RF filters on
that exact revision. An emulator changing its discovery byte to 11 is not
hardware evidence. Do not change the existing Saturn ID-10 profile or enable
TX, PureSignal or new droop defaults as part of this work.

## Source links

- [Thetis board enum](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/enums.cs#L388-L401)
- [Thetis C board enum](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/ChannelMaster/network.h#L415-L426)
- [Thetis model setup](https://github.com/ramdor/Thetis/blob/852bf0ef0b4f3886a13fc2846489aee16f361872/Project%20Files/Source/Console/clsHardwareSpecific.cs#L164-L176)
- [Thetis development enum](https://github.com/ramdor/Thetis/blob/0916a66b366d98e9b090ba3cb20a79e3b0f3e4e9/Project%20Files/Source/Console/enums.cs#L388-L401)
- [Saturn discovery and identity options](https://github.com/laurencebarker/Saturn/blob/4b0b76f345961cfeeb447abc6d8b0373f5743245/sw_projects/P2_app/p2app.c#L418-L429)
- [Saturn PCB revision access](https://github.com/laurencebarker/Saturn/blob/4b0b76f345961cfeeb447abc6d8b0373f5743245/sw_projects/common/version.c#L233-L248)

- [Nereus capability derivation and hardware-verification TODO](https://github.com/boydsoftprez/NereusSDR/blob/66bc1e43b65c0ffbe5d2c62812355e97f2f8f06a/src/core/BoardCapabilities.cpp#L1198-L1230)
- [Nereus G2 fallback](https://github.com/boydsoftprez/NereusSDR/blob/66bc1e43b65c0ffbe5d2c62812355e97f2f8f06a/src/core/HardwareProfile.cpp#L289-L295)
- [Nereus deliberate Thetis divergence](https://github.com/boydsoftprez/NereusSDR/blob/66bc1e43b65c0ffbe5d2c62812355e97f2f8f06a/src/core/codec/AlexFilterMap.cpp#L213-L235)
- [Longpath capability derivation](https://github.com/oe5sos/Longpath/blob/aaa080a76de4b3f9f00da2dd1dccbda2b07d74a6/src/core/BoardCapabilities.cpp#L1091-L1123)
- [Longpath G2 fallback](https://github.com/oe5sos/Longpath/blob/aaa080a76de4b3f9f00da2dd1dccbda2b07d74a6/src/core/HardwareProfile.cpp#L244-L249)
- [Zeus reserved-ID reference](https://github.com/abhishekprakash22/zeus/blob/86657f7228c96eba9cf63b19a7923de4f2ed20bf/docs/references/protocol-1/thetis-board-matrix.md)
- [Zeus current board contract](https://github.com/abhishekprakash22/zeus/blob/86657f7228c96eba9cf63b19a7923de4f2ed20bf/Zeus.Contracts/HpsdrBoardKind.cs)
- [Newer Saturn fork discovery identity](https://github.com/kd4yal2024/Saturn/blob/9bc0140a25e78454d1e7235a9abcaa4604e4bdbd/sw_projects/P2_app/p2app.c#L1214-L1225)
