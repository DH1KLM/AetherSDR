# ANAN upstream conformance audit

Audit and targeted cleanup started 2026-11-04. This document records findings
and validation, not architecture approvals.

## Comparison scope

- Work branch: `codex/anan-upstream-conformance`.
- Preserved source branch: `codex/anan-tx-implementation`.
- Initial work head: `c71419fd8a90c152e7ddeac571f0fdb9ca762d70`.
- Fetched upstream head: `231895b0bc2d2d2962c1e2b2ed27352a3152ee02`.
- Merge base: `d3cbbf6412515ce09ae8cc0a6183ff26565da27f`.
- Divergence: 23 upstream-only and 204 branch-only commits, including merges.

Use the merge-base diff to identify additions; do not mistake upstream-only
changes for local deletions. Upstream has been fetched, not merged.

## Initial findings

| Item | Classification | Evidence and next action |
| --- | --- | --- |
| New ANAN family branches in RadioModel | First cleanup implemented; broader audit remains open | Initial connection, automatic reconnect and wake reconnect now delegate board/protocol mapping to the existing backend-local AnanImplementation helper. This removes three added family tests and namespaced parameter writes from RadioModel, without adding a generic seam. Existing family parameter seeding and other above-seam ANAN coupling remain audit targets. |
| ASIO widget includes AudioEngine and AsioOutput | Local model extraction implemented; upstream design review remains necessary | AsioOutputSettingsWidget now consumes AsioSettingsModel. The model loads configuration, enumerates drivers and queues existing engine application, forwarding status. AudioEngine keeps persistence, validation, driver ownership and TX guards. Other existing GUI audio-engine consumers remain audit targets. |
| ASIO SDK and shared audio pipeline changes | RFC status unresolved | GOVERNANCE.md requires prior approval for new dependencies and audio pipeline architecture changes. RFC #6106 excludes new dependencies; its comments contain no explicit approval. Issue #3242 and its five comments prioritize a common sink factory; no approval of this fork's implementation was found there. This is not an exhaustive approval search. |
| DSP planning thread | Earlier new-thread claim withdrawn | Both m_ioThread and m_dspBuildThread already exist in upstream AnanBackend.cpp. Audit the changed TX planning and retirement behavior, not the existence of the thread. |
| Capability boolean freeze | Initial check passes | 70 booleans at frozen count 70. This does not establish semantic capability correctness. |
| Raw Flex command freeze | Initial check passes | 125 raw commands across 16 files, matching baseline. |
| Engine boundary strict check | Passes with unchanged legacy warnings | 101 tracked findings, zero blocking findings. Merge-base/head annotations match after ignoring line shifts. |
| Touchpoint manifest | Initial freshness check passes | New headers and increased includer counts still require review and PR justification under the conformance checklist. |
| Generator edits | Inspected changes are encoding fixes | gen_touchpoint_manifest.py and gen_seam_probe_table.py explicitly use UTF-8; these changes do not relax the inspected checks. |

All four checks were subsequently run on the merge base and initial work head
with Python UTF-8 mode. Both trees pass: 101 tracked engine-boundary findings,
70 capability booleans, 125 raw commands across 16 files, and fresh manifests.
Engine annotations compared per file/rule/message, ignoring shifted line
numbers, have no added or removed findings. The command-plane scanner's
per-file maps also match exactly.

After the first cleanup, the same four checks pass and the manifest needs no
regeneration. Full logs are local build artifacts under build/conformance-audit.

The initial branch adds touchpoints for AsioOutput, AnanImplementation and
FrontPanelModel; includer counts also grow for AudioEngine, ThemeManager,
IRadioBackend, AnanSettings, BandPlanManager, PanadapterModel and RadioModel.
These remain review targets; the first cleanup adds no further touchpoints.

## First implementation candidate

Audit RadioModel connection, reconnect and selected-radio request preparation
as one unit. Identify the narrowest existing backend-owned path for ANAN board
and protocol parameters. Avoid introducing another generic seam or routing
architecture merely to remove a family comparison.

Implementation is tracked by [DH1KLM/AetherSDR #1](https://github.com/DH1KLM/AetherSDR/issues/1),
created with DH1KLM assigned before editing code. No upstream issue was claimed
and no PR has been opened.

The new request-preparation cases cover original-option reset ordering,
physical board identity, P1 board ID zero, retained extended profile options,
case-insensitive family matching, the existing address-only P2 default, and
non-ANAN request isolation. These are socket-free mapping checks in the existing
implementation test; they do not certify live reconnect or RF behavior.

The aethercore library and anan_implementation_test target rebuilt successfully
with MSVC x64. CTest passed anan_implementation_test after the change, with the
Qt bin directory on PATH. The compiler reported C4804 on an unchanged RadioModel
line. Linux/macOS and live-radio validation remain outstanding.

## ASIO model cleanup

[DH1KLM/AetherSDR #2](https://github.com/DH1KLM/AetherSDR/issues/2) was created
and assigned before this cleanup. The new model is built into libaethercore;
the widget consumes it without direct AudioEngine or AsioOutput includes.
Configuration and driver ownership, schema, explicit Apply behavior and engine
guards retain their existing implementation. This facade does not establish
upstream approval of the underlying ASIO integration.

The generated manifest removes the AsioOutput GUI row, reduces AudioEngine
includers from 50 to 49, and adds AsioSettingsModel with two GUI includers.
Total header count stays 283. This is model mediation of client audio settings,
not a conversion to the daemon protocol; converted count remains zero.

The new socket-free asio_settings_model_test checks queued value snapshots,
engine refusal propagation, unchanged persistence on refusal, and cancellation
when the engine is destroyed. No ASIO driver is opened. Static boundary,
capability, command-plane, manifest, test-registration and frozen CI gate checks
pass after the cleanup; the QSettings ratchet also passes. The model test built
successfully and CTest passed asio_settings_model_test, asio_output_test and
anan_implementation_test (3/3). No driver or radio was used for these checks.
The complete Windows AetherSDR desktop target also built and linked successfully
after both cleanups. The application was not launched as part of validation.
The accessibility checker reports no findings in the ASIO widget and nine in
RadioSetupDialog; comparing the before/after findings with line shifts ignored
confirms all nine predate this cleanup.

## Connection panel audit and next bounded cleanup

The first two cleanups are saved as signed commits `1ef0077e` (ANAN request
mapping) and `1f7c18b9` (ASIO model and audit). Both signatures were verified
locally. They have not been pushed.

Inspection of the remaining GUI touchpoints identifies three separate scopes:

| Surface | Current ownership | Next action |
| --- | --- | --- |
| ConnectionPanel manual profile picker | Backend facade supplies the operating-profile choice table; GUI retains saved/original wrapper entries | Choice-table extraction implemented under fork #4, preserving captions, order, IDs and omissions. |
| ConnectionPanel::probeAnan | GUI retains discovery transport and presentation; backend helpers normalize replies and resolve selection | Selection and identity extractions are implemented under fork #3/#5. Any transport migration remains a separate design scope. |
| RadioSetupDialog_Saturn | SaturnSettingsModel mediates implementation preferences and original-option settings | Extracted under fork #6, retaining disconnected selection, scope fallback and existing live-session guards. |

RadioSetupDialog_Anan already uses RadioModel and the extension route rather
than including AnanSettings or AnanImplementation. It is not evidence of the
same direct-header defect merely because it is an ANAN-specific page.

At the connector audit baseline, broadcast discovery constructed physical
RadioInfo inside AnanDiscovery while the manual path repeated its mapping.
The shared normalizer described below removes that duplication and leaves
routed-address and source-bind metadata with the caller. The P2Protocol include in ConnectionPanel
predates this fork; do not classify its mere presence as a newly introduced
vendor include. The added P1 behavior and GUI hardware policy need their own
review regardless of the unchanged include baseline.

The next implementation should cover one selection path at a time, without
starting the deferred cross-family connector registry or adding a generic
backend interface. Before editing it, create and claim a separate fork issue.
Required socket-free cases: saved/explicit/automatic choices; original Saturn
versus another board or P1; incompatible protocol ignored without saving;
invalid/future saved profile; override off/on; failed persistence; and preserved
physical identity. Existing picker labels and button behavior must remain the
same. A full directed-probe transport replacement is a separate design scope.

## Manual profile resolution cleanup

[DH1KLM/AetherSDR #3](https://github.com/DH1KLM/AetherSDR/issues/3) was created
and assigned before this change. ConnectionPanel now passes the selected
profile ID, protocol and optional override value to the existing
AnanImplementation facade. It receives distinct accepted, ignored-protocol,
invalid-profile/override, incompatible-original and persistence-failure results.
The panel retains its current input controls, messages and transport loop.

The helper owns saved-profile resolution, physical-board compatibility,
original Saturn/P2 eligibility and the existing preference writes. A successful
hardware-profile write consumes the controls even if the subsequent, separate
implementation-preference write fails, matching the previous panel behavior.
Future-schema refusal and per-radio persistence still use the existing settings
methods. No generic connector or backend interface was added.

The expanded socket-free implementation test passes. It covers saved,
automatic, explicit and original selections; physical identity; protocol
mismatch without saving; override validation; invalid/future saved profiles;
refused profile/implementation writes and the partial-success reset case.
Engine-boundary, capability, command-plane, manifest and QSettings checks pass.
The manifest requires no regeneration; accessibility findings in ConnectionPanel
match the preceding commit when shifted line numbers are ignored.
The complete Windows desktop target and the expanded implementation-test target
built successfully; CTest passed anan_implementation_test. No live directed
probe, audio driver or radio session was started for this validation.

The Saturn setup extraction is recorded below. These cleanups do not
complete the entire connector migration or establish firmware qualification.

## Manual profile choice cleanup

[DH1KLM/AetherSDR #4](https://github.com/DH1KLM/AetherSDR/issues/4) was created
and assigned before this change. AnanImplementation now supplies the existing
14 ordered operating-profile choices as caption/opaque-ID descriptors. The
saved-model and original-Saturn wrapper entries keep their existing positions.
ConnectionPanel no longer references AnanModel enumerators. The unreachable
disabled-item branch for the absent Atlas/Metis and HL2 entries and its unused
QStandardItemModel include were removed; internal board recognition is unchanged.

The existing connection_panel_size_test already checks captions/order, stable
G2/original routing IDs and retained internal board identities. Its pre-change
baseline passes; no duplicate table test was added. Engine-boundary, capability,
command-plane and manifest checks pass after extraction, with no manifest
changes or additional accessibility findings.
The complete Windows desktop and both affected test targets built successfully.
CTest passed connection_panel_size_test and anan_implementation_test (2/2)
after extraction. The existing GUI test confirms unchanged captions/order and
G2/original routing IDs. No live radio or audio-driver validation was performed.

## Shared discovery identity normalization

[DH1KLM/AetherSDR #5](https://github.com/DH1KLM/AetherSDR/issues/5) was created
and assigned before this change. AnanDiscovery::fromDiscoveryReply accepts a
datagram, address and optional directed source port, reuses the existing P1/P2
parsers, and returns physical RadioInfo without loading settings. Its public
signature exposes no vendor wire payload type.

Both broadcast and manual discovery use this normalization for family, board,
protocol, address, radio port, physical name, MAC, integer firmware version,
gateware label and in-use status. Profile admission remains with each caller:
automatic listing still requires a qualified receive profile, while manual
connection can report a recognized but unqualified board. Source-IP matching,
directed source-port policy, protocol preference, busy-radio refusal and bind/
routed metadata retain their existing paths. Discovery never authorizes TX.

Socket-free fixtures cover corresponding P1/P2 Orion MkII identities, canonical
MAC rendering, unmodified shared-board names, protocol-specific firmware labels,
available/busy status, recognized-but-unqualified identities, unknown boards,
truncation and directed port acceptance/refusal (including P2 port zero).
The four conformance checks pass with unchanged accessibility findings and
no additional GUI headers or touchpoint-manifest changes.
CTest passed the expanded anan_discovery_test. The Log Analyzer maintenance
tool refreshed 15 moved source-line citations, including earlier RadioModel
cleanup offsets; only line numbers changed. All 767 checks across 49 log rules
pass after that generated update.
The Windows desktop and both affected test targets built successfully, and
connection_panel_size_test also passed after the build. No live discovery
traffic or radio/audio-driver session was used for validation.

## Saturn settings model cleanup

[DH1KLM/AetherSDR #6](https://github.com/DH1KLM/AetherSDR/issues/6) was created
and assigned before this change. RadioSetupDialog_Saturn consumes neutral
implementation-choice descriptors and a pull-based snapshot from
SaturnSettingsModel. The model owns preference selection, original-option writes
and the existing RadioModel frontend-extension dispatch. The page retains its
existing controls, labels, signal blockers and reseed lifecycle.

Connected sessions refuse implementation changes at the model boundary. Original
band-filter and speaker-mute edits retain their live route only for the current
original implementation; next-connect fields and non-original/disconnected
sessions do not dispatch those live edits. Persistence still delegates to the
existing backend settings facade with its per-radio/family preference scope.
QPointer handles retirement of the owning RadioModel without a stale dereference.
There is no new worker, signal-routing architecture or wire/daemon interface.

The generated manifest adds one ui-support model header (one GUI consumer),
replaces the Saturn page's AnanImplementation include (3 to 2 GUI consumers),
and removes its direct RadioModel include (63 to 62). Total headers are now
284, with 229 core and 55 model headers. This model boundary is the reason for
the new manifest row; it is not a conversion to a daemon protocol. Frozen
baselines and vendor classifications were not relaxed.

The implementation test now exercises family/per-radio scope isolation,
connected selection refusal, both allowed live options, next-connect-only
options, disconnected/non-original refusal and RadioModel retirement through an
injected backend without a peer. Existing setup tests cover the actual widget
selection and original/extended configuration separation. The four conformance
checks and QSettings ratchet pass; Saturn accessibility findings are unchanged.
The full Windows desktop and both affected test targets built successfully.
CTest passed the expanded anan_implementation_test and the existing
radio_setup_anan_sample_rate_test. All 767 Log Analyzer checks pass without
another citation update. No live radio, audio driver or network peer was used.

## Consolidated gate review and missing seam probe

The wider review at `efc230d3` ran 15 platform-independent checks from the static
workflow: engine boundary, touchpoint manifest, seam probe table, generated user
references, test registration, WDSP links, capability records, command plane,
frozen CI test gate, network timeouts, QSettings, theme seed, shader dialects,
Qt pin and documented build options. Fourteen passed immediately. The seam
table check failed because panZoomModesChanged had no generated probe. The
signal and missing probe already coexist at the original fork head `c71419fd`;
this was not introduced by the cleanup commits.

[DH1KLM/AetherSDR #7](https://github.com/DH1KLM/AetherSDR/issues/7) tracks the fix.
The canonical generator adds that one probe. The freshness check now passes for
all 58 declared seam signals. Its 23 parser tests also pass after explicitly
reading UTF-8 in the live-header comparison: with the Windows CP1252 default,
the test previously reported a false stale-table error even after regeneration.
Running it in UTF-8 mode isolated that encoding defect before the two reads
were corrected. No equality check, parser guard or baseline was weakened.

backend_seam_affinity_test rebuilt and passed. It checks simulator emissions
and cold construction/getters for other families; this does not establish live
ANAN signal affinity or hardware behavior. The 15 named checks are now green.
Local logs are in build/conformance-audit/final-static, including the original
failing seam-table check. This is a broader local gate pass, not a complete
GitHub CI run or a Linux/macOS application build.

The available shell has no Docker executable on PATH; WSL listing returned the
installation/help interface rather than a usable distribution. No Linux build
environment or macOS runner was established. Installing one, pushing for CI,
opening a PR and changing repository settings were not part of this review.

The remaining work is qualification and review, rather than a reason to keep
adding speculative interfaces: confirm maintainer approval for the expanded
ANAN/P1/multi-RX/TX and ASIO scope; run the required Linux/macOS CI checks in
their proper environments; and collect the model/firmware-specific RX/TX and
ASIO evidence required by the existing design documents. The existing family
connection seeding and manual discovery transport remain staged migration
touchpoints, not automatic evidence that every retained include is a new
violation. None of the local green checks grants architecture or TX approval.

## Fork build qualification at f1e80b89

The signed branch was pushed to DH1KLM/AetherSDR. Manual workflows ran against
`f1e80b89b846dfd6c063f337a9f94662339c9b96` on 2026-11-04:

- [AppImage run](https://github.com/DH1KLM/AetherSDR/actions/runs/41815378368):
  Linux x86_64 and ARM64 jobs succeeded, including packaging and runtime-closure
  checks. Release attachment was skipped.
- [macOS run](https://github.com/DH1KLM/AetherSDR/actions/runs/41815391178):
  Apple Silicon and Intel build jobs succeeded, including binary and bundled
  framework checks and unsigned bundle upload. Signing/notarization was skipped;
  both subsequent app-update tarball jobs were cancelled. This is successful
  application build evidence, not a fully successful release workflow.

These packaging workflows do not replace the required PR CI test gate.
The consolidated Windows CTest pass covered seven targets:
anan_implementation_test, anan_discovery_test,
radio_setup_anan_sample_rate_test, backend_seam_affinity_test,
connection_panel_size_test, asio_output_test and asio_settings_model_test.
All seven passed with the Qt runtime directory on PATH.

A clean docs/user npm ci followed by npm run build also succeeded. npm audit
reported 34 affected dependency nodes (3 low, 11 moderate, 5 high, 15 critical).
Many critical nodes inherit the same Tinypool advisory classification through
Docusaurus. Package files were unchanged by this cleanup; no exploitation was
established. [Fork issue #8](https://github.com/DH1KLM/AetherSDR/issues/8)
tracks separate dependency triage. No forced dependency update was applied.

Review should distinguish the seven implemented cleanup issues from approval of
the inherited feature architecture. Issues #1 through #7 remain open pending
review and integration. No upstream PR, tag or release was created.

Before an RX qualification session, record the exact radio model, firmware,
P1/P2 protocol, original/extended implementation, connection method and host
audio configuration. Compare discovery, connection, slice-0 reception,
disconnect/reconnect and saved-profile behavior with the preserved original
branch. Capture logs and observations for each result. No live-radio or ASIO
driver behavior is established by the builds above; TX needs its own approved
hardware-specific procedure.

## Outstanding validation

- Continue the approval search beyond #5554, #5262, #6106 and #3242, which were read.
- Review and justify the recorded touchpoint additions; engine and command-plane
  findings match per file.
- Inspect newly added GUI touchpoints and capability verbs.
- Initial implementation-test baseline passes with Qt bin on PATH. A direct
  launch without that path failed at DLL loading; no code ran in that attempt.
- Validate RX/TX behavior after each targeted change; no radio was keyed here.

Local untracked build-environment and FFTW files were left untouched and are
not included in the tracked branch diff. This is not a malware, binary
provenance or complete transmit-safety audit.
