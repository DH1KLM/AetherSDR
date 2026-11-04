# ANAN fork integration of upstream main

Tracking: [fork issue #10](https://github.com/DH1KLM/AetherSDR/issues/10).

Integration branch: `codex/anan-upstream-integration-20261104`.
Parents: tested cleanup `6d310a168e38a1d6df381cad19f48b41eddbc677` and
upstream main `231895b0bc2d2d2962c1e2b2ed27352a3152ee02`.
There are 23 upstream-only commits relative to the cleanup parent.
The cleanup branch, its original target and fork PR #9 are preserved.

## Conflict resolution

- AudioEngine.cpp: retain fork diagnostic includes and upstream TxChainLatency.
- RadioModel.cpp: retain queued ANAN front-panel initialization and all six
  upstream administration reconciliation connections.
- Touchpoint tags: merge semantic entries from both parents, rejecting an
  incompatible concurrent change to any existing entry. Regenerate the manifest.
- Log Analyzer: retain the merged rules and regenerate moved source citations.
- Manifest generator: keep upstream's explicit UTF-8/LF output and universal
  newline comparison, which supersede the fork's shorter UTF-8 fix.

Automatically merged areas include WDSP 2.11, typed CW/administration routes,
Line Out capture, AudioEngine, TX coordinator and ANAN capabilities. Textual
merge success alone does not establish behavior of these combined changes.

## Validation

All 16 selected static commands passed: engine boundary, touchpoint manifest,
seam table freshness, seam generator unit checks, generated user references,
test registration, WDSP test links, capability records, command plane, frozen
CI gate, network timeouts, QSettings, theme seed, shader dialects, Qt pin and
build-option documentation. Engine-boundary output retains 101 tracked findings
and zero blocking findings. Command-plane output is 110 commands across 15
files, matching the reduced upstream baseline; no exemptions were added.

The Log Analyzer passes all 767 checks after updating 21 moved source citations.
A clean docs/user npm ci and npm run build succeeded. The same 34 inherited
dependency audit findings remain tracked separately in fork issue #8; package
files were not changed.

The additional WDSP checker self-test exposed two Windows failures: fallback
diagnostics emitted backslashes where the test and other diagnostics use
repository-relative slash paths. Formatting that diagnostic with as_posix()
preserves the guard and makes all 14 existing cases pass. The real-tree strict
WDSP check also passes. This is the only post-merge code adjustment so far.

The full Windows AetherSDR target and affected test executables built successfully
using the existing PowerShell environment and shared build console. All 16
selected CTest cases passed (116.54 seconds): five AudioEngine rate/negotiation/
queue/Line-Out cases, radio administration, WDSP channel, ANAN implementation,
ANAN discovery, ANAN setup, CWX routing, HL2 TX latency, backend seam affinity,
connection-panel size, ASIO output and ASIO settings model. No radio, firmware
peer or physical ASIO driver was opened by these tests.

The compiler reported existing FFTW macro and deprecated Qt signal warnings.
The Windows checker path fix was demonstrated by the two failing existing
self-tests before the change and all 14 passing afterward.

Linux/macOS application builds were not repeated for this integration. The
previous cross-platform runs cover the earlier cleanup only. Independent review,
upstream scope approval and hardware testing of the integrated executable remain
separate from the completed Windows checks. No upstream PR or release is created
by this integration.

The operator observations in anan-upstream-conformance.md predate this merge
and do not qualify the integrated executable. No radio is keyed by this work.
