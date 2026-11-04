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
