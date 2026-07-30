# VARA HF Clean-Room Design Note

Status: **VARA HF host-interface client implemented and tested against a live
modem; the VARA HF waveform characterised from captures. No VARA binary has
been disassembled — every input is listed in §3.**

Purpose: an append-only provenance record for AetherSDR's native **VARA HF**
support, per Constitution **Principle IV (Every Contribution Is Clean-Room)**.

**Scope note (2026-07-30).** This effort originally also targeted VarAC, a
third-party chat application that runs *on top of* VARA. That work has been
removed: VarAC's application protocol is undocumented and closed, and an attempt
to characterise it produced exactly one unreproducible observation. The VARA HF
modem itself is a separate product with a **published** host interface, so it
stands on its own and is what this document now covers. Provenance entries that
existed solely to support the VarAC work have been dropped along with the code;
every entry retained below informs VARA HF code that is still in the tree. Every input that informs the implementation is logged here,
dated, before or as it is used — never retrofitted.

This file is the authority on *where our knowledge came from*. If an input is
not listed here, it did not inform the code.

---

## 1. The line

Principle IV names the clean inputs explicitly: *"Reading FlexLib's published
open-source code, capturing and studying the protocol as it actually behaves on
the wire, and reading official or public documentation are all clean-room."*

**Clean — permitted:**

- The author's own published specification (see §3.1).
- Public documentation: VARA's published TCP host interface and specification,
  and third-party implementations' public docs (BPQ32, RadioMail, Winlink).
- Behaviour observed on the wire: RF captures, audio-domain captures of a
  modulator's output, bytes observed on a TCP socket.
- Our own analysis, design and implementation derived from the above.

**Contaminated — forbidden, without exception:**

- Opening `VARA.exe` or any of its DLLs in a disassembler,
  decompiler, or any tool that recovers source or intermediate representation.
- Reading, transcribing, translating or paraphrasing such output.
- Inspecting proprietary binaries' internals by debugger attach, memory
  dumping, or DLL import interception for the purpose of recovering algorithm
  structure. (Observing a program's *external* outputs — audio, RF, sockets,
  files it writes — is clean. Reaching inside the process is not.)

*Why absolute:* decompiled code carries its original copyright. Merging it
silently relicenses someone else's proprietary work as GPLv3, and the
contamination travels to everything written by reading it. The only remedy is
to rip out all of it and rebuild. Trivial to refuse at the door; ruinous to
undo afterward.

**If in doubt, log the question here and ask before proceeding.** An input that
turns out to be contaminated is far cheaper to reject before it is used.

---

## 2. Roles and separation

The bench topology keeps the party that accepts the vendor EULAs separate from
the party that performs the analysis, at no extra cost:

| Role | Environment | Notes |
|---|---|---|
| **Signal generation** | Windows environment (WINE or one VM) running VARA | Accepts VARA's terms of use. Generates audio and TCP traffic. |
| **Observation & analysis** | Linux host — AetherSDR, capture tooling, analysis scripts | Never runs the proprietary software. Consumes only recorded outputs. |

Rationale: copyright-wise, reverse engineering for interoperability is
protected (US: *Sega v. Accolade*, *Sony v. Connectix*, DMCA §1201(f); EU:
Software Directive Art. 6, with Art. 8 voiding contrary contract terms).
Contract-wise, an anti-reverse-engineering EULA clause has been enforced as
contract in at least one US case (*Bowers v. Baystate*, Fed. Cir. 2003). That
hook is weakest when the analysing party never accepted the terms. The split
above is free given the topology, so we take it.

Recording the output of one's own licensed station is in any case not
"reverse engineering of software."

---

## 3. Allowed inputs

Append-only. Newest last. Each entry: date, what, where obtained, what it was
used for.

### 3.1 — 2026-07-28 · VARA HF Modem Specification, Rev 2.0.0

- **What:** "VARA HF Modem Specification, Revision 2.0.0, April 5, 2018",
  authored and published by Jose Alberto Nieto Ros, EA5HVK.
- **Source:** public PDF, `https://sbcara.org/wp-content/uploads/2025/08/VARA-Specification.pdf`
- **Status:** **clean** — the author's own published technical documentation.
- **Discloses** (verbatim parameters, internally consistent):
  - OFDM, 2.4 kHz SSB bandwidth, 11 adaptive levels, 60–7536 bps net.
  - Symbol 24 ms = 2.6 ms cyclic prefix + 21.33 ms FFT block; 42 baud.
    (§2.6 says "cyclic prefix of 3 ms"; §5.1 says 2.6 ms — noted discrepancy,
    resolve against capture.)
  - **52 carriers.** Carrier spacing is not stated but derives as
    1/21.33 ms = 46.875 Hz; 52 × 46.875 = 2437.5 Hz ≈ the stated 2.4 kHz.
  - DATA frame = 196 OFDM symbols, 5225 ms. 208 sync pilots (4 per carrier,
    4 rows). 9984 data carriers — consistent: 52 × (196 − 4) = 9984.
  - Levels 1–8 differential PSK; levels 9–11 QAM plus 1020 EQ pilots,
    payload 8964 — consistent: 9984 − 1020 = 8964.
  - Data block: 1 control byte (changeover) + 2 bytes CRC16.
  - ACK frame: two parallel FSK modulations, 48 tones, 31 symbols each
    (62 total), 26.66 ms/symbol, 37.5 baud, 842 ms. Eight types: START,
    ACK1, ACK2, ACK3, NACK, BREAK, REQ, QRT. **ACK codes vary per session**
    "to avoid false commands when 2 stations are using in the same dial."
  - PAPR 9 dB (data), 6 dB (ACK). ±50 Hz offset tolerance, <0.5 Hz/s
    stability. 48 kHz / 16-bit soundcard. TCP host interface.
  - ARQ: ISS/IRS roles with changeover; ~100 ms PTT allowance.
- **Does not disclose:** turbo generator polynomials, interleaver permutation,
  puncturing patterns, per-level code rates, constellation/bit mappings, pilot
  values, scrambler, CRC16 polynomial, preamble/acquisition sequence.
- **Caveat:** Rev 2.0.0 predates VARA's narrow (500 Hz) mode, later speed
  increases, and VARA FM/SAT. Authoritative for the 2.4 kHz HF skeleton only;
  every parameter to be re-verified against capture before being relied on.
  §2.5's claim of compliance with a 300-baud symbol-rate rule is stale — that
  limit was removed by FCC 23-93 (2023).
- **Used for:** demodulator front-end geometry; framing model; the decision
  that L1 is a bounded problem rather than blind waveform recovery.

### 3.2 — 2026-07-28 · Third-party manual (VarAC), for VARA's multi-instance pattern

- **What:** "VarAC manual in English", by ON2AD Pat, 158 pp, dated 22/07/2026.
- **Source:** public PDF, operator-supplied copy at `~/Downloads/`.
- **Status:** **clean** — published user documentation.
- **Used for:** one thing that survives into VARA HF work — the documented
  pattern for running **multiple VARA instances on one machine** (separate
  program folders, one `VARA.ini` per instance, distinct TCP port pairs), which
  is what `tools/vara_bench_up.sh` implements. Also documents VARA's modem
  configuration surface (bandwidth, ports, paths).
- **Note:** it documents configuration, not wire formats. It is not a protocol
  source, and nothing in it informed the VARA protocol implementation.

### 3.3 — 2026-07-28 · VARA TCP host interface (public)

- **What:** VARA's TCP command/data interface — command port 8300, data port
  8301 by default; second instance conventionally 8302/8303.
- **Source:** public third-party documentation of independent implementations —
  BPQ32 (`cantab.net/users/john.wiseman/Documents/VARA.html`), RadioMail
  (`radiomail.app/help/vara.html`), plus the author's own spec §3.6.
- **Status:** **clean** — public interface documentation, independently
  implemented by multiple third parties.
- **Used for:** Phase 1 `VaraClient`. No reverse engineering is involved in
  using this interface.

### 3.4 — 2026-07-29 · Patent clearance search

See §4. Recorded as an input because its outcome gates the work.

### 3.5 — 2026-07-29 · VARA host command vocabulary

- **What:** the concrete command and notification vocabulary of the VARA host
  interface — the exact ASCII strings on the command channel.
- **Sources:**
  - `petrkr/pyvara` — "Python library for VARA Modem", OK1PKR Petr Kracik,
    2024, **MIT licence**, `https://github.com/petrkr/pyvara`.
  - BPQ32's VARA driver documentation, John Wiseman G8BPQ,
    `https://www.cantab.net/users/john.wiseman/Documents/VARA.html`
    (confirms host/port configuration and the BW500/BW2300/BW2750 selectors).
  - EA5HVK's own specification §3.6 (§3.1 above) — "VARA uses a TCP link to
    connect with others applications".
- **Status:** **clean.** Principle IV names open-source references and public
  documentation as clean inputs. pyvara is an independent third-party
  implementation of a public interface, published under a permissive licence.
- **Method:** read for *protocol facts only* — which strings exist and what
  they mean. **No code was copied, transcribed or translated**, and none of
  pyvara's structure survives in ours: AetherSDR's implementation is a
  C++/Qt design of our own (a protocol/transport split mirroring our existing
  `AcomProtocol`/`AcomConnection` pair, with buffered CR framing and an
  explicit FIFO for reply correlation). Facts about a public interface are not
  copyrightable; the expression is, and none was taken.
- **Vocabulary recorded** (implemented in `src/core/VaraProtocol.{h,cpp}`):
  - *Transport:* command port 8300, data port 8301 (second instance
    8302/8303). Command channel is ASCII terminated by a bare **CR**, not
    CRLF. Data channel is an opaque bidirectional payload stream.
  - *Commands:* `VERSION`, `MYCALL <c1..c5>`, `LISTEN ON|OFF`, `LISTEN CQ`,
    `CONNECT <src> <dst>`, `DISCONNECT`, `ABORT`, `BW500|BW2300|BW2750`
    (no space), `CQFRAME <call> <bw>`, `COMPRESSION OFF|TEXT|FILES`,
    `CHAT ON|OFF`, `CLEANTXBUFFER`, `TUNE`.
  - *Notifications:* `OK`, `WRONG`, `IAMALIVE`, `PENDING`, `CANCELPENDING`,
    `CONNECTED <src> <dst> [<bw>]`, `DISCONNECTED`, `BUSY ON|OFF`,
    `PTT ON|OFF`, `VERSION <text>`, `REGISTERED <calls>`, `SN <dB>`,
    `TUNE <v>`, `BUFFER <bytes>`,
    `CLEANTXBUFFER BUFFEREMPTY|FAILED|OK`,
    `LINK REGISTERED|UNREGISTERED|ENCRYPTED|UNENCRYPTED`,
    `ENCRYPTED LINK` / `UNENCRYPTED LINK` (opposite word order — both forms
    occur), `BITRATE (<level>) <bps> bps`, `ENCRYPTION DISABLED|READY`,
    `CQFRAME <call> <bw>`.
- **Two findings worth carrying forward:**
  1. **Replies are positional.** `OK`/`WRONG` identify nothing. Correlation is
     by order against a FIFO of outstanding commands. A pipelining client that
     ignores this misattributes results silently.
  2. **`BITRATE` reports the adaptive speed level (1–11) directly.** The level
     is therefore observable from the host interface, without demodulating
     anything. This is how Phase 3 will confirm that AWGN injection has pinned
     the modem to an intended level — see the plan's §5.1 graph.
- **Used for:** `src/core/VaraProtocol.{h,cpp}`, `src/core/VaraClient.{h,cpp}`,
  `tests/vara_protocol_test.cpp`, `tools/vara_probe.py`.
- **Not adopted:** `TUNE` and `ENCRYPTION` are parsed but never issued — see
  the header comment in `VaraProtocol.h` for why (transmit-keying and
  §97.113(a)(4) respectively).

---

### 3.6 — 2026-07-29 · VARA HF v4.9.0 installed on the bench; observed behaviour

- **What:** VARA HF v4.9.0 (`VARA HF v4.9.0  setup.zip`, note the double space
  in the filename; 4,511,767 bytes, dated 2026-01-07,
  sha256 `5ad7d75c722e4414705dec998c28a711b7567f8568bea75cb84e8aa7c991f48a`),
  obtained from the author's official distribution point,
  `https://downloads.winlink.org/VARA%20Products/`.
- **Status:** **clean.** Running the program and observing its external
  outputs — TCP messages on its own published host interface — is behaviour
  observed on the wire, which Principle IV names as a clean input. The binary
  has not been, and will not be, disassembled or decompiled.
- **Environment:** WINE prefix `~/.local/share/wineprefixes/vara-hf`, running
  under `/opt/wine-cachyos/bin/wine`, headless on Xvfb `:99`. Setup is
  scripted and reproducible.
- **Observed structure** (from the installed tree, not from the binary):
  VARA is a **Visual Basic 6** application. It ships and requires the ActiveX
  controls `MSWINSCK.OCX` (Winsock — this is what provides the 8300/8301 host
  interface), `MSCOMM32.OCX` (serial, for CAT/PTT), `MSCOMCTL.OCX`,
  `MSCHRT20.OCX`, `COMDLG32.OCX`, `MSSTDFMT.DLL`. It also ships three
  bandwidth data files — `VARAHF500.dat`, `VARAHF2300.dat`, `VARAHF2750.dat` —
  which correspond exactly to the `BW500` / `BW2300` / `BW2750` commands.
- **Observed messages**, confirming the §3.5 vocabulary against the real
  modem:
  - `VERSION` → `VERSION VARA HF v4.9.0`
  - `LISTEN ON` → `OK` (positional reply correlation confirmed working)
  - unprompted `BUSY OFF` / `BUSY ON` immediately after connect
- **NEW message not in the §3.5 vocabulary: `MISSING SOUNDCARD`.** Emitted
  repeatedly when the modem can enumerate no audio device. Our parser
  classified it as `Unknown` and surfaced it rather than dropping it, which is
  the behaviour that header comment was written for. To be added as a typed
  error message.
- **RESOLVED — the bench now carries a live VARA-to-VARA ARQ link.**
  **Root cause: `VARA.ini` `[Soundcard]` device names were empty.** WINE was
  exposing audio devices the whole time — VARA's own Settings → SoundCard
  dialog lists `PulseAudio Input` and `PulseAudio Output`. Nothing was
  *selected*, and `MISSING SOUNDCARD` conflates "none selected" with "none
  found", so the symptom is indistinguishable from a broken audio stack. Three
  wine builds were investigated over a first-run configuration field.
  - The fix is to populate, in each instance's `VARA.ini`:
    `Input Device Name=PulseAudio Input` / `Output Device Name=PulseAudio Output`.
    Selecting them once in the dialog writes exactly those strings.
  - Confirmation: `MISSING SOUNDCARD` ceases, `IAMALIVE` keepalives begin, and
    a PulseAudio client named **"VARA HF Modem"** appears.
  - **`wine-cachyos-opt` is NOT broken** — an earlier note here said it was,
    and that was wrong. Its prefix registry contains
    `Software\Wine\Drivers\winepulse.drv\devices\0,alsa_output…` and
    `…\devices\0,varaA`; those keys are written only by a *successful*
    enumeration. It was unconfigured, not defective. The bench uses soda
    because that is what is proven end to end, not because cachyos cannot work.
  - **`wine-tkg-staging-wow64-bin` 11.9 genuinely cannot do audio** — no
    `winepulse.drv` / `winealsa.drv` PE modules exist for either architecture.
    That is the one build where the message reflects a real driver failure.
  - **VARA uses the MME path (`winmm`: `waveIn*` / `waveOut*`), not
    `mmdevapi`.** This is why `MMDevices` registry entries were never a valid
    signal in either direction. Drop them from the mental model.
  - **WINE's `winmm` builds its device list ONCE per process**, behind
    `InitOnceExecuteOnce`, and never refreshes. Two consequences:
    (a) the `varaA` / `varaB` sinks must exist **before** VARA starts;
    (b) if PipeWire restarts, every running VARA keeps a stale, dead device
    list and fails **silently** — no new error, no re-enumeration. Mitigated by
    declaring the sinks in
    `~/.config/pipewire/pipewire.conf.d/99-vara-null-sinks.conf` rather than
    creating them at runtime with `pactl`. After any PipeWire restart, restart
    the modems too.
  - **The SmartSDR bottle is not a control** — it has never been run (no
    sessions recorded, its configured runner no longer exists, newest file
    2025-06-21). It was cited earlier as evidence that wine audio worked on
    this host; it is not evidence of anything.
  - **WINE exposes ONE generic device pair**, not one entry per PipeWire node.
    Two instances therefore cannot be pointed at different sinks from inside
    VARA's dialog — separate them with **`PULSE_SINK` / `PULSE_SOURCE` per
    process** instead.
  - **Two instances, one prefix**, using separate program folders
    (`C:\VARA`, `C:\VARA2`) each with its own `VARA.ini` — the multi-instance pattern
  documented in §3.2.
  - **Live link achieved** (no radio, no RF, audio crossing two null sinks):
    `CONNECTED KK7GWY-1 KK7GWY 2300` on both sides, `SN 6.6` / `SN 7.5`,
    `BITRATE (4)  175 bps TX` / `RX`, `UNENCRYPTED LINK`, `LINK UNREGISTERED`,
    with PTT alternating as ARQ turns over. Reproducible.
  - **NEW protocol finding:** `BITRATE` carries a trailing **`TX`/`RX`
    direction token** that no published description mentions. Now parsed
    (`BitrateDirection`), optional so older builds still parse.
  - **Correction to the plan's §5.1 assumption.** It predicted a digitally
    perfect loopback would pin the modem at level 11 and that AWGN injection
    would therefore be mandatory. Observed reality: the virtual path yields
    **SN ≈ 7 dB and settles at level 4**, not level 11. The null-sink path is
    not high-SNR — likely the −5 dB ALC drive level plus format conversion.
    Level control still needs deliberate handling, but the premise was wrong
    and the mechanism must be re-derived from measurement.
  - **The data path works — payload crosses the modulated link.** VARA's own
    session logs record, on three separate sessions:
    `VARA2  Disconnected  TX: 35 Bytes (Max: 175 bps)` and
    `VARA   Disconnected  RX: 35 Bytes (Max: 175 bps)` — exactly the length of
    the 35-byte probe payload. An earlier entry here called this unresolved;
    that was a fault in the *test harness* (a data-socket reader thread dying
    without reporting), not in the data path.
  - **Confirmed directly** by `tools/vara_link_selftest.py` once the harness
    was fixed — a 44-byte payload written to the sending modem's data socket
    arrived byte-exact on the receiving modem's data socket ~30 s later at
    level 4:

    ```
    A  <== PLAINTEXT 44 bytes: b'AETHERSDR PHASE2 PLAINTEXT READ 0123456789\r\n'
    PAYLOAD MATCH: True
    ```

    **This is the Phase 2 mechanism demonstrated end to end: whatever an
    application frames inside a VARA ARQ link is readable in the clear at the
    far end, with no access to that application's internals.** Put any VARA host
application on one modem and our client on the other, and its traffic is
observable
    without decompiling anything — which is precisely the clean-room route
    Principle IV sanctions.

- Historical record of the diagnosis, kept because the false trails are
  instructive:
  - `VARA.ini` `[Soundcard]` has `Input Device Name=` and
    `Output Device Name=` **empty** — first-run state. VARA cannot distinguish
    "no device selected" from "no device exists" in this message.
  - `wine-tkg-staging-wow64-bin` (`/usr/bin/wine`) ships `winepulse.so` /
    `winealsa.so` but no matching `.drv` PE modules — genuinely incomplete.
  - **But that was not the whole cause.** `wine-cachyos-opt`
    (`/opt/wine-cachyos/bin/wine`) ships the complete stack (`.drv` for i386
    and x86_64 plus the `.so` counterparts) and **still** produces
    `MISSING SOUNDCARD`.
  - Under wine-cachyos, `winepulse.so`, `libpulse.so.0`, `mmdevapi.dll` and
    `winmm.dll` are all mapped into the VARA process, and `libpulse` resolves
    correctly — yet `HKCU\...\MMDevices\Audio\{Render,Capture}` is absent
    under **both** the `pulse` and `alsa` drivers, and PulseAudio reports the
    **same 19 clients before and after** launching VARA. The driver loads but
    never opens a connection to the sound server.
  - **Bottles / soda-9.0-1 fails identically.** A separate bottle (`VARA-HF`,
    port 8310/8311 via `VARA.ini`) with the soda runner — which carries a
    complete audio stack *including* the 32-bit halves
    (`lib32/wine/i386-unix/winepulse.so`) that a 32-bit VB6 app needs — still
    emits `MISSING SOUNDCARD`. That makes **three independent wine builds**
    (tkg 11.9, cachyos 10.0, soda 9.0) failing the same way, so this is
    systemic to wine audio on this host rather than a per-build defect.
  - **Beware the misleading indicator:** the operator's *working* SmartSDR
    bottle also has **zero** `MMDevices` entries in its registry, so that key
    is not evidence of anything either way. The only meaningful signals are
    whether a PulseAudio client appears (it never does) and whether the modem
    stops emitting `MISSING SOUNDCARD`.
  - Untested, in rough order of expected value: running under a **real
    session display** rather than Xvfb (every attempt so far has been
    headless, and wine audio init may depend on session context); driving
    VARA's own Settings → Soundcard dialog with `xdotool` to see whether the
    device list is empty or merely unselected; a **Windows VM**, which
    removes this whole class of problem.
  - A prefix cannot be moved between wine builds — wine's unixlib interface is
    version-locked, so the prefix must be created by the build that runs it.
- **Configuration surface** (`C:\VARA\VARA.ini`, plain INI): `[Soundcard]`
  input/output device names, ALC drive level, channel; `[PTT]` rig/port/CAT;
  `[Setup]` **`TCP Command Port=8300`**, registration codes, `Encryption=0`,
  `Enable KISS=0` / `KISS Port=8100`, retries, persistence. The TCP port being
  settable here is how the bench's second instance will be stood up on 8302.
- **Note for the bench:** VARA binds its host ports on **`0.0.0.0`**, not
  loopback — i.e. reachable from the whole LAN. Firewall it or bind the bench
  to an isolated interface.
- **Changelog note (public, shipped with the product):** v4.8.9 records
  "Encryption enabled for MWXXXX callsigns (Emergency control centers in
  Cardiff County and Vale of Glamorgan area)" — i.e. encryption is gated to
  specific callsigns rather than generally available. Consistent with our
  decision to parse but never request it (§3.5).

### 3.8 — 2026-07-29 · Level 4 is NOT OFDM. VARA is a dual-waveform system.

Measured from a bench capture (48 kHz, no radio, no RF, verified unclipped:
peak 0.38, RMS 0.27, zero samples near full scale).

- **The level-4 waveform is CONSTANT ENVELOPE.** Peak/RMS = 1.42 = **3.05 dB
  crest factor**, the textbook value for constant modulus; analytic-envelope
  PAPR ~1 dB, power kurtosis 1.00. **OFDM cannot be constant-envelope** — it
  would show ~9 dB PAPR and kurtosis ~2, exactly as the spec claims for its
  DATA frame. So level 4 is not the waveform the spec describes.
- **Instantaneous-frequency analysis shows discrete tones**: ~15 clusters
  between ~842 and ~2161 Hz, mean spacing 94.25 Hz — consistent with
  **93.75 Hz = 48000/512**, centred near 1500 Hz. Constant envelope plus a
  regular discrete tone grid is **M-FSK**.
- **The arithmetic closes.** 16 tones × 93.75 Hz = 1500 Hz occupied. At
  93.75 baud and 4 bits/symbol that is 375 bps raw; against level 4's known
  175 bps net this implies a code rate of **~0.47 ≈ 1/2** — an entirely
  ordinary turbo rate. (Exact figures pending the cross-validation in §3.9.)
- **CORRECTION to an earlier entry.** A previous commit here concluded "the
  2018 spec does not describe VARA 4.9.0". That was overstated. The accurate
  statement is that **it does not describe level 4**, because level 4 is not
  OFDM at all. The spec may well be accurate for the high levels, which a free
  licence cannot reach (§3.7). We were analysing the wrong waveform, not
  reading a stale document.
- **The reconciling hypothesis: VARA is dual-waveform** — constant-envelope
  MFSK for the robust low levels, OFDM for the high-rate levels. This explains
  every anomaly at once: no cyclic prefix was found because there is no OFDM
  here; PAPR was 3 dB not 9 dB because MFSK is constant-modulus; and the
  advertised **−22 dB SNR** performance is reachable precisely because MFSK
  excels there (the same reason FT8 and JS8 use it). The switchover level is
  unknown and is a structural fact worth establishing before implementing
  either half.
- **Consequence for Phase 3:** the job changed shape rather than growing. An
  MFSK demodulator is materially simpler than an OFDM one — no cyclic prefix,
  no channel estimation, no equalisation; just per-symbol tone detection — and
  symbol boundaries are unambiguous, which makes bit alignment for the
  differential attack far easier than it would have been in OFDM. Levels 1–4
  are fully reachable on the free licence.

**METHOD NOTE, recorded because it cost four wrong answers.** Every estimator
used here must be validated against a synthetic signal with known parameters
*before* its output on real data is believed. That discipline is what turned
"I cannot get a clean CP reading" into the firm negative result that redirected
the whole investigation: the CP estimator recovers the true period on synthetic
OFDM at peak/mean 2.64, and scores 1.1 — noise — on the real capture. Four
unvalidated measurements preceded it, each of which looked like a result:
a passband correlation swamped by its double-frequency term; a lag scan whose
window scaled with the lag so the metric rose trivially; a spectrum
autocorrelation over too long a window; and a recorder (`pw-record --target`)
that silently fell back to the default source and captured a microphone instead
of the sink. Assume the first attempt is wrong until synthetic truth says
otherwise.

### 3.9 — 2026-07-29 · The modem itself is a decoder oracle (replay validated)

The single most useful thing the bench can do, and it was sitting unused for
most of a day: **play a candidate waveform into the sink that a VARA instance
is listening on, and let VARA tell you whether it decodes.** Binary verdict, no
statistics, no confidence metric to misread.

Routing (from `tools/vara_bench_up.sh`): instance A captures `varaB.monitor`,
so `paplay --device=varaB <file>` feeds audio to instance A. The reverse holds
for instance B via `varaA`.

**Test 1 — replay fidelity: PASS.** A previously captured 66.9 s recording of
instance B's transmissions, replayed into `varaB` with instance A in
`LISTEN ON`, produced on A:

```
PENDING
SN 6.6                       ...later SN 5.3
UNENCRYPTED LINK
CONNECTED KK7GWY-1 KK7GWY 2300
LINK UNREGISTERED
BITRATE (4)  175 bps RX
PAYLOAD 44 bytes: b'AETHERSDR PHASE2 PLAINTEXT READ 0123456789\r\n'
DISCONNECTED
```

The 44-byte payload returned byte-exact from replayed audio alone. Two
consequences:

1. **The capture chain is lossless.** The WAV contains everything needed to
   reconstruct a full ARQ session, so every measurement taken from these
   captures is being taken on a faithful signal. The constant-envelope /
   not-OFDM finding in §3.8 rests on good data.
2. **One-way replay is sufficient to test decodability.** The far end never
   had to answer; VARA decoded the data frames from the recording alone.

**This is the definitive Phase 3 validation loop.** Synthesise a waveform from
hypothesised parameters, play it in, and see whether VARA decodes it. If it
does, the model is right — there is no interpretation step and no metric to
argue with. That is a far stronger oracle than any internal goodness-of-fit
score, and it should be the acceptance test for every parameter claim from here
on.

It also reframes the whole effort: we do not have to *infer* the waveform and
hope. We can *propose* one and be told, by the authoritative implementation,
whether we are right.

### 3.10 — 2026-07-29 · Frame-type ablation: the 1759 ms burst is the connect frame

Each burst from a captured session was cut out with 300 ms of leading and
trailing silence and replayed **individually** into a listening modem
(§3.9 oracle). What VARA does with a frame in isolation identifies it:

| burst | length | modem response | conclusion |
|---|---|---|---|
| 0 | **1759.3 ms** | **`PENDING`** | **connection-request frame — self-contained** |
| 1 | 4351.2 ms | none | session-dependent (data) |
| 2 | 692.6 ms | none | session-dependent |
| 3 | 1375.3 ms | none, then `CANCELPENDING` | session-dependent |

**Why this matters more than it looks.** The connect frame is the ideal target
for the differential attack, better than any data frame:

1. **It is self-contained and independently verifiable.** Replay it alone and
   the modem says `PENDING`. That is a binary oracle for "is this frame
   well-formed?", available without establishing a session.
2. **Its payload is known and operator-controlled.** A connect frame encodes
   the source and destination callsigns. Issuing
   `CONNECT KK7GWY-1 KK7GWY` versus `CONNECT KK7GWY-2 KK7GWY` changes exactly
   one character of known plaintext — which is precisely the controlled
   single-symbol perturbation the generator-matrix recovery needs, on a frame
   we can validate in isolation.
3. **It is cheap and endlessly repeatable.** One command produces one, with no
   ARQ session, no payload transfer and no waiting.

So the recovery work should start on the **connect frame**, not the data
frames. Vary one callsign character at a time, capture, align, and difference.
The oracle confirms each captured frame is well-formed before it enters the
analysis, which removes an entire class of silent error.

### 3.11 — 2026-07-29 · Level-4 DATA modulation determined exactly

Three independent analyses — instantaneous-frequency, spectral/cyclostationary,
and frame-timing — each validated against synthetic ground truth first, agree
to the digit. These are not fitted estimates with error bars; the residual
against the model sits at the WAV's 16-bit quantisation floor, so they are the
generator's own constants recovered exactly.

**DATA frames (the 4351.2 ms bursts, indices 1 and 6):**

| Parameter | Value |
|---|---|
| Modulation | **16-ary orthogonal CPFSK**, modulation index h = 1.000 |
| Tones | **16**, contiguous DFT bins 9…24 |
| Tone spacing | **93.750000 Hz** = 48000/512 |
| Tone range | **843.750 … 2250.000 Hz** |
| Centre | **1546.875 Hz** = 16.5 × 93.75 |
| Symbol period | **512 samples = 10.666667 ms** |
| Symbol rate | **93.7500 baud** |
| Phase at symbol start | **−90.000°** (pure sine) on all 814 symbols |
| Raw bit rate | 4 bits × 93.75 = **375 bps** |
| Implied FEC | **rate 1/2** → 187.5 bps → 175 bps net after overhead |

The rate-½ fit against the known 175 bps net is the independent arithmetic
check, and it closes cleanly.

**A SECOND waveform exists.** The frame-timing analysis found the ten bursts
split into two structurally distinct classes: the two long DATA bursts above,
and **eight bursts using a 2048-sample symbol on a 23.4375 Hz tone grid**
(= 48000/2048). Those are the control/ACK-class frames — a different, much
finer tone grid with four times the symbol length. Combined with the OFDM the
published spec describes for high levels, VARA uses **at least three distinct
waveforms**. All burst lengths are exact integer multiples of 512 samples, and
inter-burst regions are true digital zeros, so burst boundaries are exact
rather than threshold-dependent.

**CORRECTIONS to earlier entries in this document.**

* §3.8 recorded "~15 clusters, mean spacing 94.25 Hz". The true spacing is
  **93.750 Hz** and there are **16** tones — the instantaneous-frequency
  histogram missed one and its bin quantisation inflated the spacing. That
  first reading was closer to right than what followed.
* A later single-threaded scan concluded "**8 active tones at 187.5 Hz**".
  That is **wrong and is withdrawn**. 187.5 Hz is exactly twice the true
  spacing, so a 187.5 Hz grid samples every second real tone and lands 8 of
  them — a textbook aliasing artifact, made worse by a confidence metric that
  was already known to be defective. The correct reading was the one I
  second-guessed.
* The top tone is **2250.0 Hz**, not the ~2161 Hz previously recorded.

The lesson generalises beyond this measurement: three independent methods with
mandatory synthetic validation resolved in one pass what four sequential
single-method attempts got wrong. Where a number matters, measure it more than
one way.

### 3.12 — 2026-07-29 · DATA frames demodulate cleanly to a symbol stream

Using the §3.11 parameters, `tools/vara_mfsk.py` locks onto both DATA bursts:

- Tone grid lands on **DFT bins 9.000 … 24.000** exactly, confirming §3.11.
- **Median detection confidence 4296 and 5218** (winning tone over runner-up),
  against 1.6 for the earlier wrong parameter set. Unambiguous.
- **407 symbols** per frame; burst length 208859 samples = 407.93 x 512.
- Both DATA bursts align at the same symbol offset (240 samples).
- Symbol histogram is near-uniform over all 16 tones (counts 19-34), i.e. the
  symbol stream is whitened or coded — there is no plaintext structure visible
  at symbol level, which is expected and is what makes differential analysis
  the right approach rather than inspection.

Rate check: 407 symbols x 4 bits = **1628 raw bits** per frame. At rate 1/2
that is 814 information bits over 4.3512 s = 187 bps, against the modem's
reported 175 bps net after protocol overhead. Consistent.

Symbol-to-bit mapping (natural binary vs Gray vs something else) is NOT yet
determined and is not needed for the differential attack, which operates on
whatever consistent labelling the demodulator produces.

### 3.13 — 2026-07-29 · Reconciled inventory; the model is exact; burst 9 is a CW ID

Five independent analyses (four methods plus a reconciler that rebuilt every
estimator and re-measured every contested quantity against its own synthetics
with negative controls) converge. Supersedes the burst table in §3.12.

**The modulation model is exact, not fitted.** Resynthesising each DATA burst
from `round(12510 * sin(2*pi*k*n/512))` — that expression and nothing else —
reproduces the capture with **80.9 % of samples bit-exact and 100.000 % within
+/-1 LSB** (rms 0.437 LSB against a 16-bit dither floor of 0.289). Per-symbol
off-bin energy is **1.17e-9 (-89.3 dB)**: the quantisation floor and nothing
else. Symbol amplitude is 12509.71 +/- 0.07 LSB (5.6 ppm over 812 symbols) and
every symbol starts at phase -90.00002 deg with R = 1.00000000.

**Corrected burst inventory.** Inter-burst regions are **exact digital zeros**
(70.28 % of the file), so boundaries carry zero uncertainty, and every length is
an exact multiple of 512 samples:

| # | start | ms | symbol | #sym | class |
|---|---|---|---|---|---|
| 0 | 1123840 | 1749.333 | 2048 | 41 | control type A |
| 1 | 1265856 | **4341.333** | **512** | **407** | **DATA** |
| 2 | 1507584 | 682.667 | 2048 | 16 | control type B |
| 3 | 1571328 | 1365.333 | 2048 | 32 | control type A |
| 4 | 1668608 | 1365.333 | 2048 | 32 | control type A |
| 5 | 1994752 | 1365.333 | 2048 | 32 | control type A |
| 6 | 2091712 | **4341.333** | **512** | **407** | **DATA** |
| 7 | 2333696 | 725.333 | 2048 | 17 | control type B |
| 8 | 2399168 | 682.667 | 2048 | 16 | control type B |
| 9 | 2432512 | 3413.333 | — | — | **CW identifier** |

**My `demod3.json` boundaries were wrong** — starts 168-239 samples early,
lengths ~478 samples (~10 ms) long. The DATA frame is **4341.333 ms**, not the
4351.2 ms recorded in §3.11/§3.12; the 407-symbol count was right. The
"lead-in / trailing ramp" seen earlier was **my energy detector over-running**,
not a feature of the signal.

**Burst 9 is a Morse station identifier, and it decodes to `KK7GWY`.** A
continuous 750.0 Hz sub-tone (amplitude identical in all 320 blocks, min = max)
with 1500.0 Hz OOK-keyed on top at exactly 2x amplitude; bin 24 (2250 Hz) is
**exactly zero in all 320 blocks**. Keying runs are exact multiples of 4 blocks
(2048 samples = 42.667 ms = one Morse unit, 28.125 WPM), runs of 4 and 12 only,
decoding `-.- -.- --... --. .-- -.--` = **K K 7 G W Y**. I had recorded this
burst merely as "3423.3 ms, other". VARA transmits a CW station ID at session
end — worth knowing, and a rare piece of *known plaintext* present in every
session.

**Two waveform classes below the OFDM levels**, both constant-envelope FSK:
DATA on a 512-sample symbol / 93.75 Hz grid with M = 16, and **control frames
on a 2048-sample symbol / 23.4375 Hz grid with M = 70**, in two length families
(type A: 41/32 symbols; type B: 16/17 symbols).

**A real anomaly, mechanism unknown:** DATA symbol amplitude is 12509.71 LSB
and control-frame amplitude is 12472.42 LSB — a stable **0.0264 dB (0.3 %)**
difference across all bursts of each class. Not a quantisation artifact (both
classes contain odd-k tones whose peak sample equals A exactly). Two modulator
paths with slightly different scaling. Also: DATA bursts end with a 16-sample
raised-cosine taper beginning at sample 496 of 512; control bursts have **no
taper at all**.

### 3.14 — 2026-07-29 · DETERMINISM PROBE: PASS. The encoder is a pure function.

**The gate on generator-matrix recovery is passed, decisively.**

Two independent sessions, same payload, captured separately (DATA frames at
different absolute file offsets, so genuinely different runs):

| Comparison | Result |
|---|---|
| Frame 0 symbols | **407 / 407 = 100.00 %** identical |
| Frame 1 symbols | **407 / 407 = 100.00 %** identical |
| Frame 0 raw audio | **sample-exact — 0 differing samples of 208384** |
| Frame 1 raw audio | **sample-exact — 0 differing samples of 208384** |

Chance agreement at symbol level is 6.25 % (1 in 16). Sample-exact audio
equality across separate sessions is far stronger still: it means the DATA path
carries **no session key, no sequence number, no seeded scrambler, and no
timestamp** — nothing that varies between runs. Encode() is a pure function of
the payload.

**Consequences.**

* The differential attack is **tractable linear algebra over GF(2)**, exactly
  as designed: capture Encode(0), capture Encode(e_i), XOR, and each difference
  is a row of the generator matrix. No per-session re-derivation of a baseline,
  no statistical averaging.
* The spec's note that ACK codes "are different for each session" is therefore
  **confined to the control/ACK path** and does not touch DATA. That was the
  single most likely hard blocker, and it is ruled out for the frames that
  matter.
* Captures are reusable across sessions, so the ~900 single-bit experiments can
  be batched and compared freely rather than needing a within-session baseline.

**Probe validation.** `tools/vara_determinism.py --selftest` recovers a known
407-symbol truth exactly (407/407) *and* correctly flags a deliberately seeded
control as non-identical (348/407 = 85.5 %). Both directions matter: a probe
that could not detect seeding would report "deterministic" for everything.

The self-test also caught a real bug before it reached live data. Because every
symbol is `A*sin(2*pi*k*n/512)`, **the first sample of a burst is an exact
zero**, so first-nonzero detection lands late and breaks the 512-sample
alignment. The splitter now snaps starts back onto the grid. This is the same
class of boundary error that made an earlier energy detector over-run by ~478
samples — with the alignment wrong, the comparison would have shown ~6 % and
been reported as a hard blocker.

### 3.15 — 2026-07-29 · Control-frame structure: preamble, type marker, addressee

**The control waveform.** 2048-sample symbols (42.667 ms, 23.4375 baud) on a
23.4375 Hz grid (= 48000/2048). Occupied bins **29…98 — exactly 70 tones**,
679.7…2296.9 Hz, independently confirming M = 70. Per-symbol spectral purity is
**1.000**: one clean tone per symbol, no residual.

**Symbol 0 is a frame-type marker.** Bin **74** opens every 41- and 32-symbol
frame; bin **62** opens every 16- and 17-symbol frame. The frame declares its
class before any content, so frames can be classified without decoding them.

**The 41-symbol frame is a 9-symbol preamble plus the 32-symbol frame.**
Verified exactly: `f41[9:] == f32`, symbol for symbol. The preamble is

```
[74, 68, 70, 60, 60, 77, 50, 76, 78]
```

and is **identical in every 41-symbol frame ever captured**, across different
destinations and different sessions. A first transmission carries it; the eight
retries that follow omit it and send the bare 32-symbol frame. That is an
acquisition/sync preamble, not content.

**Symbols 10…40 encode the DESTINATION callsign — not the source.** The
differential capture is unambiguous:

| Capture | source | destination | 32-sym frame |
|---|---|---|---|
| conn_KK7GWY-1 | KK7GWY-1 | NOBODY | identical |
| conn_KK7GWY-2 | KK7GWY-2 | NOBODY | identical |
| conn_KK7GWY-3 | KK7GWY-3 | NOBODY | identical |
| varaB_tx / det1 | KK7GWY-1 | **KK7GWY** | **31/41 symbols differ** |

Varying the *source* SSID across three captures changes **nothing** — 0/41
symbols differ. Changing the *destination* changes all 31 content symbols.
**The source callsign is not carried in this frame at all.**

**All eight retries within a capture are symbol-identical**, which is further
determinism evidence on the control path — and note this is the path whose ACK
codes the specification says vary per session. Whatever that per-session
variation is, it does not reach the connect frame's content.

**Capacity check.** 31 content symbols x log2(70) = ~190 bits to carry a
callsign plus whatever CRC and framing accompany it — comfortable for a
FEC-coded 6-12 character callsign, and consistent with heavy coding.

**Consequence for the attack.** The addressee field is now localised to symbols
10…40 of a self-validating frame (replaying one alone yields `PENDING`). A
destination-callsign sweep — one character at a time — is the cheapest
available probe into the coding, because the plaintext is fully known and
operator-chosen and each capture is verifiable before use.

### 3.16 — 2026-07-29 · Addressee is fully diffused; tone parity alternates by position

**Destination sweep.** Source held fixed, destination varied one character at a
time (W1AAA / W1AAB / W1AAC / W1BAA / W2AAA, plus a repeat control and the
earlier NOBODY / KK7GWY captures). All eight retries within every capture are
symbol-identical, and the repeat of W1AAA reproduced exactly — determinism
holds on this path too.

**Result: FULL DIFFUSION.** A single character change alters **28-31 of the 31
content symbols**. Nothing is positionally encoded; the addressee is spread
across the whole field by FEC plus interleaving or scrambling. Symbol 0 (the
type marker, bin 74) is the only invariant.

That settles the shape of the remaining work: symbol-to-character
correspondence cannot be read off directly, and **generator-matrix recovery is
the only route** to the coding layer. The differential machinery is still the
right tool — full diffusion is exactly what a linear code with an interleaver
produces, and §3.14 proved the encoder deterministic, so each single-character
difference is a valid linear combination of generator rows.

**A hard structural constraint: tone parity alternates with symbol position.**
Verified across all six frames, all 31 content positions, with **zero
violations**:

```
odd-numbered  positions (1,3,5,...)  ->  EVEN tone index
even-numbered positions (2,4,6,...)  ->  ODD  tone index
```

Consequences:

* The effective alphabet is **~35 values per position, not 70**. Even-parity
  bins {30,32,...,98} and odd-parity bins {29,31,...,97} are 35 each.
* Every symbol boundary carries a guaranteed frequency change, which removes
  timing ambiguity when consecutive symbols would otherwise repeat a tone.
* This is plausibly what the specification means by ACK frames using "two
  parallel FSK modulations" — not simultaneous (the signal is constant
  envelope, one tone at a time) but **time-interleaved** between two tone sets.

**Open question, needs more data.** Of the 35 even-parity bins, 32 distinct
values were observed; of the 35 odd-parity, 34. Six frames undersample the
alphabet, so this cannot yet distinguish "35 values, three unseen" from
"exactly 32 values, i.e. a clean 5 bits per symbol". 31 symbols x 5 bits = 155
bits would be a suspiciously tidy field size. A wider destination sweep settles
it, and the answer determines the symbol-to-bit mapping that generator recovery
depends on.

### 3.17 — 2026-07-29 · Alphabet is exactly 35 per position — in CONTROL frames only

> **CORRECTION (see §3.21).** This entry's original heading read "This breaks
> the GF(2) plan", and that conclusion was **over-generalised and is
> withdrawn**. The 35-value alphabet is a property of **control frames**. DATA
> frames use **16 tones = exactly 4 bits per symbol**, and the GF(2)
> differential attack applies to them unchanged. Phase 3 was stopped partly on
> this mistake.

**Settled by a 21-destination sweep** (651 content symbols):

| Parity class | Distinct values | Draws | Missing |
|---|---|---|---|
| EVEN tones (odd positions) | **35 of 35** | 336 | none |
| ODD tones (even positions) | **35 of 35** | 315 | none |

The position-parity rule held with **zero violations** across all 21 frames,
so §3.16's constraint is now on very firm ground.

**The per-position alphabet is 35, not 32.** The tidy "5 bits per symbol"
hypothesis is refuted — every one of the 35 values in each class appears.

**Why this matters, and it is not a detail.** 35 is not a power of two, and
35 = 5 x 7 is not a prime power, so **GF(35) does not exist**. The planned
attack — recover a generator matrix by XOR-differencing codewords over GF(2) —
assumed a bit-to-symbol mapping. There is none:

* If the encoder converts a bit string into base-35 digits, that conversion is
  **not linear over GF(2)**, so XOR differences do not compose and the
  differential attack as designed **does not apply**.
* A non-binary code over a field would need GF(37) or GF(41) with values
  truncated to 35, which is possible but speculative.
* 31 symbols x log2(35) = **159.0 bits** of capacity, suspiciously close to
  160 bits (20 bytes) — consistent with a fixed-size field packed by base
  conversion rather than by bit-grouping.

**Assessment: this is the point to stop and decide rather than continue.** The
waveform and framing layers are solved exactly and reproducibly. The coding
layer now requires either (a) determining the base-35 packing and working over
the right algebraic structure, which is a research question rather than a
measurement campaign, or (b) a fundamentally different approach. The
~900-experiment GF(2) campaign that looked like the path forward would produce
differences that do not compose linearly, and would waste the effort.

Recorded here so the next person does not spend that effort on the strength of
§3.14's determinism result alone. Determinism is necessary for the differential
attack but not sufficient — the algebra has to close too, and here it does not.

### 3.20 — 2026-07-29 · Bench defect: both VARA instances fought over KISS port 8100

A run of five handshake experiments returned `NO LINK` on **every** trial,
including the control that sent nothing. The cause was not the protocol:

* Both `VARA.ini` files ship with **`KISS Port=8100`**. Instance A wins the
  bind; instance B loses it and is left unstable.
* After a host application hit a decode error and restarted its modem, instance B
  **died outright and never returned**, taking 8310/8311 with it. Every later
  connect attempt then failed with an unexplained "no link".
* Only one VARA process was alive, holding 8300/8301 **and** 8100, while the
  host application still held stale ESTABLISHED sockets to a vanished
  8310/8311.

Fixed by giving each instance its own KISS port — A keeps 8100, B uses 8110 —
and pointing any host application's KISS port setting at 8110 to match. All six ports now
bind: 8300/8301/8100 and 8310/8311/8110, with two audio clients.
`tools/vara_bench_up.sh` now enforces this rather than leaving it to chance.

**Worth recording as a diagnostic lesson.** Five trials produced five identical
null results, which reads as a strong finding about the protocol and was in fact
a broken bench. The control trial is what exposed it: sending *nothing* should
not have failed to link, so the failure could not be about our reply. **A
control that fails tells you to doubt the apparatus, not the hypothesis.**

### 3.21 — 2026-07-30 · CORRECTION: the GF(2) attack was never blocked for DATA frames

**What I got wrong.** §3.17 concluded that a 35-value-per-position alphabet
"breaks the GF(2) plan" and Phase 3 was stopped partly on that basis. The
finding was real but its scope was not: **35 values per parity class is a
property of CONTROL frames**, measured on connect frames during a
destination-callsign sweep.

**DATA frames are a clean power of two.** §3.11 records them as **16-ary**
orthogonal CPFSK — DFT bins 9…24, `4 bits × 93.75 baud = 375 bps` raw, implying
rate 1/2 against the 175 bps net. Sixteen tones is **exactly 4 bits per
symbol**. There is no base conversion, no non-power-of-two alphabet, and
therefore no obstacle to XOR-differencing codewords over GF(2).

| Frame type | Alphabet | Bits/symbol | GF(2) differential attack |
|---|---|---|---|
| **DATA** (payload) | 16 tones | **exactly 4** | **applies, unchanged** |
| CONTROL (connect) | 35 per parity class | 5.13 | genuinely obstructed |

**Why the mistake happened.** The attack was pivoted onto control frames
because they carry **known plaintext** — a destination callsign that can be
varied one character at a time and validated in isolation via the `PENDING`
oracle (§3.10). That was a good tactical reason. The error was treating an
obstacle found on that detour as a verdict on the main route.

**Every prerequisite for the DATA-frame attack is already in place:**

* symbols recoverable exactly (§3.11/§3.13, bit-exact resynthesis);
* the encoder proven **deterministic sample-exact** across sessions (§3.14);
* payload fully operator-controlled through VARA's data socket, so
  `Encode(0)` and `Encode(e_i)` are both directly producible;
* 4 bits per symbol, so XOR differences compose and each difference is a row
  of the generator matrix.

**Standing conclusion:** the DATA-frame coding layer is **unattempted, not
blocked**. What remains is the ~900-experiment measurement campaign described
earlier — mechanical and automatable, not a research dead end. The genuine open
questions are the effort involved, and the separate matter of levels 5-11
requiring a paid licence.

**Method note.** A correct finding with an incorrectly scoped conclusion is
harder to catch than a wrong measurement, because the number itself survives
scrutiny. The check that would have caught it: **before generalising a result,
name the population it was measured on.** This one was measured on control
frames and generalised to all frames.

### 3.22 — 2026-07-30 · FEC differential: first generator-row data

The differential attack on DATA-frame FEC is running and producing real
structure. `tools/vara_fec_capture.py` links the two bench instances, sends an
exact payload with `COMPRESSION OFF`, captures the transmitter's audio and
demodulates to symbols.

**Session shape (measured, not assumed).** No payload size produces a single
DATA frame. A transfer is: one **164-symbol control frame** (the connect frame,
§3.10), then **407-symbol DATA frames**, then control frames, and often a
**CW ID** at the end. The CW ID is 320-321 symbols and was initially
misclassified as DATA by a naive `>= 200 symbols` filter; it is now separated by
its 750 Hz continuous sub-tone signature (§3.13).

**Frame 1 is payload-independent.** With three payloads of the *same length* but
different content (64 zeros; 64 bytes with bit 0 set; with bit 1 set), the first
407-symbol DATA frame is **407/407 identical** in all three. It is a fixed
session frame. (An earlier version of this claim compared only all-zero payloads
of differing lengths and was worthless — near-identical inputs give
near-identical outputs.)

**~~An all-zero payload produces NO payload frame.~~ RETRACTED — see §3.23.**
This was wrong. Enumerating *every* burst rather than only exact-407 ones shows
the all-zero capture has two 407-symbol DATA frames like every other payload.
`Encode(0)` **is** directly obtainable, so the pairwise-difference workaround
described here was never necessary.

**First result — `c(bit 0) XOR c(bit 1)` on the payload frame:**

| quantity | value |
|---|---|
| first differing symbol | **12** (header 0-11 untouched) |
| last differing symbol | 406 |
| symbols differing | 277 / 407 |
| symbols unchanged | **130** / 407 (chance would be ~25) |
| Hamming weight (4-bit natural-binary XOR) | **582 / 1628 bits = 35.7 %** |
| per-symbol popcount histogram | `{0:130, 1:85, 2:100, 3:71, 4:21}` |

Random 4-bit XOR would give `{25, 102, 153, 102, 25}`. The observed excess of
untouched symbols is **structure, not noise**.

**Cross-comparison: 46 positions are unchanged in all four independent
comparisons** (bit0/bit1, zeros32/64, zeros32/96, frame1/frame2) against **~7.5
expected by chance** — a 6x enrichment. Their values are *nearly* constant
across every frame observed, with a couple of exceptions, so they are most
likely **not pure pilots** but positions the interleaver and puncturing leave
untouched for these particular input bits. That is exactly what sparse
generator rows look like, and it is the behaviour the attack needs.

**Unconfirmed observation, recorded but not relied on:** the 46 positions show
visible regularity — many adjacent pairs (42/43, 76/77, 157/158, 224/225,
239/240, 256/257, 338/339, 372/373, 403/404) and repeated gaps of 16 and 17,
with several pairs sharing a residue mod 17. Forty-six points is far too few to
fit a permutation, and 407 = 11 x 37 is not divisible by 17, so this is flagged
for more data rather than treated as a finding.

**What this establishes:** the differential machinery works end to end on DATA
frames, the header/payload boundary is at symbol 12, and the code behaves
linearly with sparse rows. **What remains is the campaign** — many single-bit
differentials to accumulate rows, which is mechanical and automatable rather
than exploratory.

### 3.23 — 2026-07-29 · Half of every symbol is exactly GF(2)-linear

Three findings, in the order they forced each other.

**Correction first: `Encode(0)` is available.** §3.22 claimed an all-zero payload
emits no payload frame. Enumerating every burst by length, instead of filtering
for exactly 407 symbols, shows the truth:

    zeros64   164c 407D 64c 128c 128c 128c 407D 68c 68c 128c 128c 450?
    bit0      164c 407D 64c 128c 128c 128c 407D 68c 68c 114? 63? 128c 128c 128c 321CW

Two 407-symbol DATA frames in both, at burst indices 1 and 6. Burst 1 is the
payload-independent session frame; burst 6 carries payload. The earlier claim
came from a filter that silently dropped frames, and the lesson is the same one
as the CW-ID misclassification: **a filter that drops data is not a negative
result.** So generator rows come out directly, `row_i = Encode(e_i) XOR
Encode(0)`, with no pairwise workaround.

Rows for the first three payload bits, against `Encode(0)`:

| row | symbols differing | Hamming weight | first | last |
|---|---|---|---|---|
| 0 | 260 / 407 | 547 / 1628 (33.6 %) | 13 | 406 |
| 1 | 270 / 407 | 587 / 1628 (36.1 %) | 12 | 405 |
| 2 | 259 / 407 | 577 / 1628 (35.4 %) | 13 | 405 |

`row0 XOR row1` reproduces the independently measured `c(bit0) XOR c(bit1)`
difference **exactly**, weight 582 both ways, which confirms frame alignment and
demodulation are right.

**No shifted-impulse structure.** Cross-correlating consecutive rows at every bit
lag peaks at lag 0 (0.64-0.67 agreement) with no sharp off-zero peak, so
`row_(i+1)` is *not* a delayed copy of `row_i`. A bare RSC encoder would show
that peak; an interleaver destroys it. Consistent with a real turbo code, and it
means Berlekamp-Massey cannot be applied to the transmitted symbol order
directly.

**The payload frame is reproducible across sessions.** Same payload, two separate
ARQ sessions: **0 / 407 symbols differ**, in both the session frame and the
payload frame. §3.14 proved determinism on one frame; this extends it to the
payload frame specifically. That matters because it **rules out session state**
(sequence numbers, ARQ metadata) as an explanation for anything below.

**Linearity fails — but by only 5 %.** The affine identity
`c(e_i) XOR c(e_j) XOR c(e_i + e_j) XOR c(0) = 0` should hold for any linear
encoder. Measured on two independent triples:

| triple | mismatching bits | failing symbols |
|---|---|---|
| A (bits 0, 1) | 86 / 1628 (5.3 %) | 67 / 407 |
| B (bits 2, 3) | 73 / 1628 (4.5 %) | 58 / 407 |

Random would be ~50 %. Natural binary is also clearly the best mapping (86
mismatches, versus Gray 134 and inverse-Gray 192).

**The mapping was solved, not guessed.** The identity is *linear in the unknown
tone-to-bit mapping*, so the mapping is the null space of a GF(2) system: each
symbol position contributes `v[w] + v[x] + v[y] + v[z] = 0` over 16 unknowns.
From 290 non-trivial constraints the null space has **dimension 3**, and every
basis vector depends only on `s mod 4`:

    [1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0]
    [1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0]
    [1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1]

A bijective 4-bit mapping needs 4 independent bit planes and only 3 exist, so
**no tone-to-bit mapping can rescue full linearity.** The 5 % is not a mapping
artefact. (The space is exactly the even-weight functions of `s mod 4`; a free
constant per bit plane is expected, since a constant cancels in a four-term sum.)

**What the null space was actually saying.** Nothing in it depends on `s >> 2`.
Splitting the tone index confirms that directly:

| component | triple A | triple B |
|---|---|---|
| `lo = s & 3` (low 2 bits) | **0 / 407 violations** | **0 / 407 violations** |
| `hi = s >> 2` (high 2 bits) | 67 / 407 | 58 / 407 |

**Two of the four bits of every symbol are exactly GF(2)-linear in the payload** —
zero violations across two independent triples, an exact result rather than a
statistical one. That is **814 linear bits per frame**, and on those bits the
generator-matrix attack works exactly as designed.

It is not a wrong-algebra problem either; every alternative is worse:

| algebra | triple A | triple B |
|---|---|---|
| XOR, `lo` only | **0** | **0** |
| XOR, whole symbol | 67 | 58 |
| integer mod 16, whole symbol | 146 | 140 |
| integer mod 4, `lo` / `hi` | 82 / 108 | 88 / 92 |
| first differences, XOR / mod 16 | 243 / 241 | 228 / 227 |

**Open: why `hi` is nonlinear.** Not yet known. A plain additive scrambler is
excluded by construction — a constant cancels in a four-term sum — and session
state is excluded by the reproducibility result above. Demodulation error is
implausible because the tone decisions are not marginal (best-to-runner-up
magnitude ratio ~186 000, since a 512-sample symbol sits exactly on a DFT bin),
but that is an argument from confidence and not yet a direct check; whether
violations correlate with the lowest-confidence symbols is still to be tested.

**Bench constraint discovered.** The VARA host interface is **single-client**: a
second TCP connection to the command port is refused while a capture holds it.
This surfaced as a spurious `ConnectionRefusedError` in one batch and as ports
appearing closed while a capture was in fact running normally. Captures must be
serialised, and the port must not be probed during one.

### 3.24 — 2026-07-29 · The linear half, confirmed against controls

§3.23's result was measured on two triples of adjacent payload bits. It now holds
across independent bit choices, including the two most widely separated bits in
the payload, and — importantly — **against controls that prove the test can
fail**.

| affine identity | `lo` violations | `hi` violations |
|---|---|---|
| bits 0, 1 — adjacent, byte 0 | **0 / 814** | 86 |
| bits 2, 3 — adjacent, byte 0 | **0 / 814** | 73 |
| bits 0, 32 — different bytes | **0 / 814** | 68 |
| bits 0, 255 — first and last bit | **0 / 814** | 63 |
| bits 0, 1, 2 — three-way | **0 / 814** | 80 |
| *control: deliberately wrong combination* | *329 / 814* | *265* |
| *control: a second wrong combination* | *318 / 814* | *253* |

The controls matter as much as the results. A test that always passes proves
nothing, and these fail at about 40 % — so zero violations on the true
identities is a real measurement. The three-way case is included because
`Encode` is affine: `c(e0) + c(e1) + c(e2) + c(e0+e1+e2)` must also vanish, and
it does.

**Demodulation error is now excluded directly**, not merely argued from
confidence. At the positions where `hi` violates the identity, the runner-up tone
magnitude is 0.0000 and the tones four steps either side (the only slip that
would corrupt `hi` while preserving `lo`) are also 0.0000. Minimum confidence at
violating positions is 109 787, *higher* than the 49.5 seen at some clean
positions, so violations are not marginal decisions.

**Per-bit-plane, the split is exactly the two LSBs:**

| bit plane | triple A | triple B |
|---|---|---|
| `s & 1` | 0 / 407 | 0 / 407 |
| `s >> 1 & 1` | 0 / 407 | 0 / 407 |
| `s >> 2 & 1` | 38 | 29 |
| `s >> 3 & 1` | 48 | 44 |

Both nonlinear planes are independently nonlinear, and `hi` is **not** a function
of `(position, lo)` — 458 conflicts across seven captures — so the high bits
carry information of their own rather than being derivable from the linear part.

**Payload length.** A 32-byte payload produces the same burst structure as a
64-byte one (`164c 407D 64c 128c 128c 128c 407D ...`, payload frame at burst 6),
so the sweep needs 256 captures rather than 512. A 16-byte payload does not: its
bursts come out misaligned and it emits a 468-symbol frame. 32 bytes is the floor.

Confirmed at that length: a 32-byte triple gives **0 / 814** `lo` violations,
85 `hi` violations, and a three-term control violating at 409 / 814.

`tools/vara_fec_sweep.py` runs the campaign — resumable, one capture per payload
bit, strict validation of each capture, and it discards WAVs after extracting
symbols because 256 captures would otherwise be about 1.3 GB. Its extraction and
analysis paths were exercised against the existing 64-byte captures before being
trusted: all seven were accepted at 407 symbols, and a negative control
(`probe_16.wav`, known-misaligned) was correctly **rejected** with
"burst 6 is not a whole number of symbols". From six rows the partial generator
matrix has full rank 6, row weights 288-333 of 814 (38.4 %), and 212 of 814
columns untouched by any row — payload-independent positions whose count should
fall as rows accumulate.

The 256-capture sweep is running. Captures are serialised (single-client host
interface) at roughly three minutes each, so about 13 hours.

### 3.25 — 2026-07-29 · The bit planes have graded algebraic degree 1, 1, 2, 3

§3.24 left one thing open: why the two high bits are not linear. They are not
nonlinear in an arbitrary way. **The four bit planes of the tone index have
algebraic degree 1, 1, 2 and at least 3, graded by bit significance.**

Over GF(2) a function of algebraic degree *d* has a vanishing (*d*+1)-th
derivative, so degree is directly measurable. The third derivative over
`{e0, e1, e2}` is the XOR of `c(S)` over all eight subsets `S`, which needed two
captures that did not exist (`c(e0+e2)`, `c(e1+e2)`); the 256-capture sweep was
paused for them and resumed afterwards, which is what its resumability is for.

| plane | `D²` at base 0 | `D²` at base `e2` | `D³` | degree |
|---|---|---|---|---|
| `s & 1` | 0 | 0 | 0 | **1 — linear** |
| `s >> 1 & 1` | 0 | 0 | 0 | **1 — linear** |
| `s >> 2 & 1` | 38 | 38 | **0** | **2 — exactly quadratic** |
| `s >> 3 & 1` | 48 | 42 | 10 | **at least 3, and sparse** |
| *control — 7 of the 8 terms* | | | *182-210* | |
| *control — a duplicated term* | | | *112-168* | |

For plane 2 the second derivative is not merely the same *weight* at both base
points, it is the same *vector* — that is what `D³ = 0` says, and base-point
independence of the second derivative is the definition of degree 2.

**Two independent lines of evidence that this is real, not a fluke:**

*All ten third-derivative residue bits lie in the single plane `s >> 3`.* None in
`s >> 2`. If the residue were scattered noise across the two high planes, the
chance of landing all ten in one is 2⁻¹⁰. The residue symbols are 63, 69, 156,
162, 228, 276, 310, 311, 326, 393 — none inside the 12-symbol fixed header, and
confidence there is 109 787 or better, so they are not marginal decisions.

*The two second-derivative supports coincide almost exactly.* Base 0 has support
86, base `e2` has 80, intersection 78, **Jaccard 0.886**. Three random control
pairs of matched sizes give Jaccard 0.031 to 0.044. A genuinely cubic map would
give near-independent supports, so a symmetric difference of 10 instead of the
~149 expected under independence is the measurement that pins the degree.

**What this buys.** A quadratic map is a linear part plus a bilinear form, and
both are measurable — the linear part from single-bit captures, the bilinear form
from pair captures. So three of the four bits of every symbol become computable
from the payload: two linear planes plus one exactly-quadratic plane, **1221 of
1628 bits per frame (75 %)**, up from the 814 (50 %) established in §3.23.

**Mechanism: not identified, and worth saying so plainly.** Graded degree is the
signature of carry propagation in integer arithmetic — each successive bit of a
sum gains a degree from the carry. But a plain two-operand add would give degrees
1, 2, 3, 4, and the measurement is 1, 1, 2, 3, so it is not that. Phase
accumulation across symbols is excluded independently: every symbol starts at
-90.00002° (§3.12), so phase is reset per symbol rather than accumulated. No
mechanism is claimed.

**Confirmed on a second independent triple.** `{e0, e1, e3}` reproduces it: plane
`s>>2` gives `D²` = 38 at base 0 and 38 at base `e3` with `D³` = 0, and plane
`s>>3` gives 48, 44 and `D³` = 6. Controls 108-217. (Note `D²(e0,e1)` at base 0 is
the *same four captures* in both triples, so that particular 38 is shared by
construction, not an independent confirmation — the independent parts are the
base-`e3` second derivative and the third derivative, which use new captures.)

### 3.26 — 2026-07-29 · Degree is fixed exactly by binary place value

§3.25 measured the degrees. There is an exact algebraic law behind them.

A "plane" here is any assignment of one bit to each of the 16 tones, i.e. a
vector in GF(2)^16. Whether a plane's *k*-th derivative vanishes is a GF(2)
linear condition on that vector, so the set of planes of degree ≤ *k* is a null
space and can be computed exactly rather than sampled.

| condition | null space | closed form | exact match |
|---|---|---|---|
| 2nd derivative vanishes (degree ≤ 1) | dim 3, 8 vectors | even-weight functions of `s mod 4` | **yes** |
| 3rd derivative vanishes (degree ≤ 2) | dim 7, 128 vectors | even-weight functions of `s mod 8` | **yes** |

These are **set equalities, not approximations**, and both reproduce on two
independent triples (`{e0,e1,e2}` and `{e0,e1,e3}`). So:

* a plane determined by `s mod 4` — bits 0 and 1 — has degree 1;
* a plane determined by `s mod 8` — adding bit 2 — has degree 2;
* a plane sensitive to `s >> 3` has degree at least 3.

**Degree is fixed exactly by which binary place of the tone index the plane can
see.** The graded 1, 1, 2, 3 of §3.25 is a consequence, not a coincidence.

**No relabelling of the tones can improve this.** A bijective 4-bit mapping must
distinguish all 16 tones, which requires at least one plane sensitive to `s >> 3`,
and every such plane lies outside the degree ≤ 2 null space. Verified by
exhaustive search over both spaces (8 and 128 vectors): no bijection exists in
either. The residual cubic component is intrinsic to the waveform, not an
artefact of how the tones are indexed.

**A hypothesis that fits the degrees — carry bits. UNCONFIRMED.** In binary
addition the carry out of bit 0 is `a₀b₀`, degree 2, and the carry out of bit 1 is
degree 3, while a sum taken *without* carry propagation stays degree 1. A
representation where the low two tone bits hold a carry-free sum of two linear
quantities and the high two hold that addition's carries would produce degrees
1, 1, 2, 3 exactly. This is consistent with every measurement here and with
nothing else tried, but it is a hypothesis fitted to a degree pattern, and a
degree pattern is weak evidence for a mechanism. §3.25's note stands: no mechanism
is claimed. The test is whether plane `s>>2` is a product of just two linear forms
— a rank-2 quadratic form rather than a general one — which is measurable from the
sweep once enough rows exist.

**Where this leaves recovery.** Planes 0 and 1 are linear and directly recoverable.
Plane 2 is exactly quadratic, so it is a linear part plus a bilinear form, both
measurable. That is **1221 of 1628 bits per frame, 75 %**. Plane 3 is degree ≥ 3;
its cubic part is sparse in these measurements (residues of weight 10 and 6), but
"sparse in two triples" is not a bound, and the number of possible degree-3
coefficients over a 256-bit payload is far too large to enumerate, so recovering
plane 3 will need structure rather than brute force.

### 3.27 — 2026-07-29 · The high bits are carries: an additive mod-16 scrambler

§3.26 recorded a hypothesis fitted to the degree pattern and warned that a degree
pattern is weak evidence for a mechanism. It now has direct evidence, and it
predicts things that were then measured and found true.

**The model.** The transmitted tone index is

    s = (d + r) mod 16

where `d` is a 4-bit symbol *linear* in the payload and `r` is a per-position
offset that does not depend on the payload — an **additive mod-16 scrambler**,
not the XOR scrambler already excluded in §3.23.

**Why it explains the degrees.** Work the full adder with `r` constant per
position:

| output | expression | degree |
|---|---|---|
| `sum0` | `d0 ⊕ r0` | 1 |
| `carry0` | `d0 ∧ r0` | 1 — because `r0` is a constant |
| `sum1` | `d1 ⊕ r1 ⊕ carry0` | **1** |
| `carry1` | `maj(d1, r1, carry0)`, contains `d1·carry0` | 2 |
| `sum2` | `d2 ⊕ r2 ⊕ carry1` | **2** |
| `sum3` | `d3 ⊕ r3 ⊕ carry2` | **3** |

That is 1, 1, 2, 3 — every measured degree in §3.25, including the non-obvious
one. A plain two-operand add of two *variable* numbers gives 1, 2, 3, 4; it is
`r` being constant that keeps plane 1 linear, and that is what the measurement
says.

**Prediction, then measurement: the nonlinearity is gated by the low bits.** If
the high bits are carries out of the low-bit addition, then at a symbol where the
low nibble does not move, no carry can change and the high bits cannot violate
the affine identity. Binning all eight available identities by how many distinct
`lo` values their four frames take:

| distinct `lo` values | positions | violating | rate |
|---|---|---|---|
| 1 | 869 | **0** | 0.000 |
| 2 | 1865 | 171 | 0.092 |
| 4 | 522 | 289 | 0.554 |

**Zero violations in 869 opportunities**, and non-vacuously — at 180 of those the
four high values were not all equal, so the identity could have failed and did
not. (`lo` counts of 3 never occur, necessarily: `lo` is linear, so the four
values form a coset `{a, a⊕u, a⊕v, a⊕u⊕v}` and the count must be 1, 2 or 4.)

**`r` is real and shared.** Searching all 16 candidate offsets per position
against eight independent affine identities:

| test | result |
|---|---|
| positions admitting a valid offset | **407 / 407** |
| *control — position-shuffled captures* | *only 31 / 407* |
| fitted on 6 identities, valid on 2 **held out** | **407 / 407** |
| contradicted by held-out identities | **0 / 407** |
| every solution set closed under `r → r+8` | **True** |

The closure under `r → r+8` was **predicted before it was measured**: subtracting
8 mod 16 flips only bit 3, and four terms cancel it, so `r` can never be
determined better than modulo 8. That ambiguity is harmless — it is absorbed into
bit 3 of `d(0)` and leaves the generator rows untouched.

**Cross-length falsifier passed.** If `r` is a fixed per-position sequence it must
be the same for every payload length. The 32-byte captures took no part in
deriving it, and their offset sets intersect the 64-byte sets at **407 / 407**
positions; against position-shuffled 32-byte data, only 36 / 407.

**Not yet pinned.** Solution sets are still of size 4 to 16 (191 positions accept
all 16, which are the payload-independent positions where any offset trivially
works). Seven further pair captures are running to add seven more identities. The
target is sets of size 2 — `{r, r+8}` — which is the information-theoretic floor.

**Why this matters for the goal.** If `r` is pinned, then `d = (s − r) mod 16` is
linear in **all four** bit planes, not two. The sweep's generator matrix then
describes the whole symbol, giving both directions: encode a payload to `d`, add
`r`, synthesise; and demodulate to `s`, subtract `r`, invert the linear map. That
is a complete level-4 codec rather than the 75 % of §3.26.

**The test that would settle it** is prediction, not identity-checking: with `r`
and the single-bit rows, predict a pair capture's 407 symbols exactly and compare
against the real capture. Beyond that, the decoder oracle (§3.9) allows the
end-to-end proof — synthesise a frame for a payload never captured and see
whether a listening VARA decodes it.

### 3.28 — 2026-07-29 · The scrambler model predicts held-out captures exactly

§3.27's model was validated by identity-checking, which is weak: it asks whether
a relation holds, not whether the model can *produce* unseen data. It now
predicts held-out captures symbol by symbol.

**Method.** Fit the offset set `r[p]` on all identities but one, then predict the
held-out pair capture's 407 symbols from `r` and the two single-bit captures:

    d(0)      = (s(0) - r) mod 16
    row_i     = ((s(e_i) - r) mod 16) XOR d(0)
    d(e_i+e_j)= d(0) XOR row_i XOR row_j
    predicted   s(e_i+e_j) = (d(e_i+e_j) + r) mod 16

**The first version of this test was near-vacuous and is recorded as such.** It
counted a hit if *any* offset in the candidate set predicted correctly. Where the
set holds all 16 offsets that succeeds by construction, and 184 of 407 positions
are that unconstrained. It reported 14/14 identities "EXACT", which was true and
almost meaningless. The rigorous form asks whether **every** candidate predicts
correctly, and whether offsets outside the set fail — and restricts to positions
whose symbol actually varies, since a payload-independent position predicts
correctly under any offset at all.

| restricted to the 349 payload-varying positions | rate |
|---|---|
| offsets **inside** the fitted set | **49784 / 49812 = 99.94 %** |
| at maximally-constrained positions (set size 4) | **5540 / 5540 = 100.00 %** |
| offsets **outside** the fitted set | 15684 / 28364 = 55.30 % |
| separation | **0.4465** |

The 28 failures fall at only five symbol positions — 70, 164, 227, 393, 405 — and
every one is at a candidate-set size of 8 or 16. **None at size 4.** They are
residual offset ambiguity where the identities under-constrain `r`, not model
failure; one (393) is a known cubic-residue symbol from §3.25.

**Cross-length held-out prediction — the strongest check available.** `r` fitted
*entirely* on 64-byte captures, then used to predict the 32-byte **pair** capture
from the 32-byte singles. That capture was never used to fit anything, and the
payload length differs:

| | rate |
|---|---|
| offsets from the 64-byte fit | **3216 / 3216 = 100.00 %** |
| offsets outside that fit | 988 / 2016 = 49.01 % |
| separation | **0.5099** |

**The model reproduces unseen data exactly.** `s = (d + r) mod 16` with `d`
linear and `r` a fixed per-position sequence is confirmed.

**On the residual ambiguity.** Multiple `(r, D)` pairs describe the data equally
well, because the choice of `r[p]` within its valid set trades off against `d`.
This is not an obstacle: *any* consistent choice yields a working codec, since
every member of the set satisfies every identity. Pinning `r` uniquely is
therefore a tidiness matter, not a blocker.

**Remaining to a level-4 codec.** The sweep supplies the 256 generator rows for
`d`. Then encoding is payload → `d` → `+ r` → the §3.12 synthesiser, and decoding
is demodulate → `− r` → invert. The end-to-end proof is the decoder oracle
(§3.9): synthesise a frame for a payload never captured and see whether a
listening VARA decodes it. That test has not been run and nothing here should be
read as claiming a working modem — soft-decision decoding for real HF noise,
control-frame generation and the ARQ state machine all remain.

### 3.29 — 2026-07-29 · A real VARA decoded a waveform we generated

Everything up to here was self-referential: the model predicted captures, and
those captures are what built the model. This is the test that is not.

**Method.** A recorded session is replayed into the listening instance's input
sink, with the payload-bearing DATA frame (burst 6) **replaced** by one
synthesised from `tools/vara_codec.py` + `tools/vara_synth.py` for a payload that
was never transmitted. Everything else — connect handshake, control frames,
session frame — is the original recording, so the receiver sees a well-formed
session. The listening modem is a genuine VARA HF v4.9.0 that knows nothing about
any of this work.

**The control ran first and mattered.** Trial 1 replays the recording unmodified.
Without it, a negative result in trial 2 could not be distinguished from a broken
replay path.

    target payload  : 1000400000200000000000000000000000000000000000000000000000000000
    modem delivered : 1000400000200000000000000000000000000000000000000000000000000000

| trial | result |
|---|---|
| CONTROL — original recording replayed | decoded 32 bytes of zeros, **exact** |
| TEST — synthesised payload frame spliced in | decoded the target payload, **exact** |

The synthesised frame differs from the all-zero frame at 269 of 407 symbol
positions and changed 138 688 samples, so this is not a near-miss that happened
to decode.

**What this proves.** The level-4 DATA **encode** path is correct end to end:
payload → generator rows → `d` → `+ r mod 16` → waveform → a real modem's
decoder. The scrambler model of §3.27, the generator rows from the sweep, and
the synthesiser of the previous entry are all confirmed together, by an
independent decoder.

**What it does not prove, stated plainly.** The surrounding session was
*replayed*, not generated: the connect handshake and control frames came from a
recording. So this is not a working modem. Still outstanding:

* the receive path (the inverse map is straightforward for a clean signal, but
  untested);
* soft-decision decoding, which is what real HF noise requires — everything here
  is a clean-channel result;
* control-frame generation and the ARQ state machine;
* levels 5-11, unreachable without a paid licence.

**A run note.** The first attempt aborted between trials with
`ConnectionRefusedError`: the host interface is single-client and briefly refuses
connections while a session tears down. `tools/vara_oracle.py` now retries rather
than treating the first refusal as fatal.

### 3.30 — 2026-07-29 · Code structure: characterised, not yet recovered

A generator matrix is a lookup table for one block size; the *structure* is the
algorithm and generalises. This is the state of the attack on it, run on 71 of
the 256 generator rows. `tools/vara_structure.py`.

**A methodological correction that shaped everything below.** The first pass
worked on the 1628-bit rows and looked for weight-1 columns as evidence of a
systematic code. It found 68 — but **85 % of them sat at positions where `r` is
unconstrained**, because `r` was fitted from identities involving only payload
bits {0,1,2,3,32,255}, and a position those six bits never move is undetermined.
There the choice of `r` is arbitrary and the row's *bit* values are meaningless.

The fix is to work at **symbol** level, which is `r`-independent: whether payload
bit `i` changes the symbol at position `p` does not depend on `r` at all, since
`s_i == s_0` gives a zero row for every offset. *Which bit plane* it lands in
depends on `r`; *whether it moves the symbol* does not. Everything below is
symbol-level.

**The code is densely diffused.** Each payload bit changes **260.8 of 407
symbols on average (64 %)**. That is not a sparse code — but it is what a
*recursive* systematic encoder does: one input drives the feedback register into
a periodic state, so its parity runs to the end of the block. It also explains
why the systematic search failed — with 64 % density a systematic position is
almost always co-touched by another bit's parity, so it is not isolable.

**The transmitted order is not the encoder's order.** Three independent
measurements:

| test | result |
|---|---|
| correlation of a bit's onset with its index | **+0.029** (not monotonic) |
| onset position | 12-13 for every bit — every input affects the first payload symbol |
| correlation between transmit-adjacent symbols | **-0.0006** |
| periodicity of a bit's support after onset | peak autocorrelation 0.130, shuffled control 0.112 |

A convolutional code read in time order would give a strong positive onset
correlation and a clear periodic signature. Neither is present, so **a
permutation sits between the encoder output and the transmitted order** and must
be undone before any convolutional structure can appear.

**But the permutation is not featureless.** Two strong, controlled findings:

*Symbols cluster by identical support.* Grouping the 361 active symbols by their
exact 71-bit support vector gives 297 distinct patterns and **18 groups with more
than one member**, the largest of size 8. Shuffling each bit's support
independently gives **407 singletons and no groups at all**. Grouped symbols are
not literal copies — their tone values differ — so they share which inputs drive
them, not what they carry.

*Support weight is strongly multi-modal.* Observed standard deviation **21.6**
against a shuffled control's **3.9**, with distinct classes:

| support weight | symbols | reading |
|---|---|---|
| 0 | 46 | payload-independent — header, pilots or padding |
| 1 | 10 | driven by a single payload bit |
| 40-41 | 48 | a distinct density class (~57 %) |
| 50-67 | the bulk | ~70-95 % |

Two well-separated density classes is the shape of **two parity streams punctured
differently**, which is what a turbo code has.

**What is NOT recovered, plainly.** The interleaver, the constituent encoder
polynomials, the puncturing pattern, and the code rate. The gross structure is
characterised and the negative results are firm, but no generative model of the
code exists yet. 71 of 256 rows is thin; the analysis should be re-run on the
full matrix before drawing further conclusions, and the temptation to fit a
permutation to partial data is exactly the error already made once in §3.22 with
46 points.

**Practical consequence.** Structure is not needed for encoding — that already
works end to end (§3.29). It is needed for **soft-decision decoding**, which is
what real HF noise requires. A hard-decision decoder can be built now from the
generator matrix alone by inverting the linear system, and would work on clean
signals; robust decoding waits on the structure.

### 3.31 — 2026-07-30 · Control frames: a small fixed vocabulary

The oracle test of §3.29 worked because the surrounding session was *replayed*. A
native modem must generate it. Analysing the control frames in 100 captures —
bench-free work, entirely on data already recorded — shows that is far easier
than expected.

**Control frames do not depend on the payload.** Byte-for-byte identical across
every payload tried at a given length: bursts 0, 2, 3, 5, 7, 8, 9 and 10 match
exactly whether the payload is all zeros, one bit, or 48 scattered bits. They
carry no payload CRC and no payload-derived field.

**A normal session is eight fixed waveforms.** Hashing every non-DATA burst
across 100 captures gives 62 distinct control waveforms, but eight of them carry
essentially every session, each appearing 546 to 1215 times across 72 of the
files at consistent burst positions:

| id | control symbols | occurrences | usual position |
|---|---|---|---|
| `14ec6cadd413` | 32 | 1215 | 5, 10 |
| `a314c703b37e` | 32 | 1183 | 4, 9 |
| `6c014efcea7d` | 32 | 646 | 3 |
| `ef38e81d4780` | 16 | 616 | 2 |
| `a403a713fc9d` | 17 | 600 | 7 |
| `a0928ccc657c` | 17 | 595 | 8 |
| `52ba2c9a53d4` | 41 | 547 | 0 — connect |
| `637124632991` | 32 | 346 | 11 |

Burst 4, which looked session-variable in an earlier pass, is not arbitrary: it
takes exactly **two** values, both of which are frames that appear elsewhere in
the same session.

**The connect frame decomposes exactly.** Its 41 control symbols are

    [45, 39, 41, 31, 31, 48, 21, 47, 49] ++ <a standard 32-symbol frame>

The 9-symbol prefix is precisely the acquisition preamble recorded in §3.10 as
bins 74, 68, 70, 60, 60, 77, 50, 76, 78 — these indices are relative to bin 29,
so they agree exactly. The remaining 32 symbols are literally vocabulary entry
`badf9ff5cdb4`, and symbol 9 is 45, the same frame-type marker every 32-symbol
frame starts with. So **a 41-symbol frame is a 9-symbol preamble followed by an
ordinary 32-symbol frame**, not a distinct format.

**The frame-type marker is confirmed on real traffic:**

| control symbols | first symbol | DFT bin |
|---|---|---|
| 16 | 33 | 62 |
| 17 | 33 (or 45) | 62 (74) |
| 32 | 45 | 74 |
| 41 | 45 | 74 |

**Per-session content exists but is confined.** Twenty one-off 41-symbol connect
frames all share the same 9-symbol preamble and differ only in their 32-symbol
body, so the connect frame carries something session-specific — a session
identifier or nonce is the obvious guess, but nothing here identifies it and none
is claimed.

**What this means for a native modem.** The control layer is far less work than
the DATA layer was. Eight stored waveforms reproduce a standard session, and the
ARQ state machine selects among them rather than computing them. What is still
needed is the mapping from *session state* to *which frame to send*, and the
content of the per-session connect body.

## 4. Patent clearance

**Date searched:** 2026-07-29. **Searcher:** AetherSDR maintainer + assistant.

Patents are strict liability — independent creation is **no defence**. Clean-room
protects against copyright, not patents. This search therefore gates Phase 3.

### 4.1 Method validation

Google Patents' query endpoint was first validated against known-positive
controls, so that null results carry evidentiary weight:

| Control query | Results |
|---|---|
| `inventor=Berrou` | 28 |
| `inventor="Claude Berrou"` | 15 |
| `q=turbo code` | 261,399 |

Method confirmed working.

### 4.2 Searches performed

| Database | Query | Results |
|---|---|---|
| Google Patents | `inventor="Nieto Ros"` | **0** |
| Google Patents | `inventor="Jose Alberto Nieto Ros"` | **0** |
| Google Patents | `inventor="Nieto-Ros"` | **0** |
| Google Patents | `inventor="J. A. Nieto"` | **0** |
| Google Patents | `q=EA5HVK` | **0** |
| Google Patents | `q="VARA HF"` | **0** |
| Google Patents | `q="EA5HVK" OR "rosmodem"` | **0** |
| FreePatentsOnline (US, EP, PCT, DE, JP) | `IN/"Nieto Ros"` | **0** |

### 4.3 Patent marking check

35 USC §287 requires marking to collect pre-notice damages; its absence both
caps damages and is evidence that nothing exists to mark.

- `rosmodem.wordpress.com` (author's site): **no** patent claim, patent
  marking, patent number, or "patent pending" language anywhere. Licensing is
  presented purely as shareware/terms-of-use and a paid unlock.
- The published specification (§3.1) makes **no** patent claim.

### 4.4 Prior-art position

The underlying techniques are long expired or public domain:

- **Turbo codes** — Berrou/Glavieux/Thitimajshima, filed ~1991, France Télécom.
  Expired ~2011–2013.
- **OFDM** — fundamentals date to the 1960s–70s.
- QAM, cyclic prefix, pilot-aided equalisation, adaptive modulation — decades
  of prior art.

Any live claim would have to cover a *specific novel combination*. The author's
own 2018 publication (§3.1) discloses much of that combination, which would
itself create a publication bar against any later-filed application.

### 4.5 Verdict

**PASS — no evidence of any patent covering VARA, from two independent
databases, with the search method validated.** Phase 3 is not blocked.

### 4.6 Residual items — not yet individually cleared

Recorded honestly rather than waved away. None are believed to be risks; all
should be closed before Phase 3 code is written.

1. Google Patents `inventor="Alberto Nieto"` returned **4** results, not
   individually inspected (Google rate-limited further queries mid-session).
   "Alberto Nieto" is a common Spanish name and FreePatentsOnline returns zero
   for the full surname pair across five jurisdictions. **Action:** inspect the
   four records.
2. Google Patents full-text `q="VARA modem"` returned **2** results, not
   individually inspected. Likely unrelated (`vara` is a common word and a
   company name). **Action:** inspect the two records.
3. Spanish national office (OEPM) not searched directly. Google Patents does
   index ES, but coverage depth was not independently verified.
   **Action:** direct OEPM search.
4. This is a documented good-faith search, **not** a freedom-to-operate opinion
   of counsel. If Phase 3 proceeds to substantial investment, note that
   post-*Halo* (2016) enhanced damages require egregious conduct, and a written
   non-infringement/invalidity opinion is the protective instrument.

---

## 5. Regulatory note (informational)

Not a clean-room matter, recorded because it bears on publication strategy.

VARA presently operates under 47 CFR **§97.309(b)** (unspecified digital
codes), whose only condition is that emissions "must not be transmitted for the
purpose of obscuring the meaning of any communication" — a **purpose** test,
not an accessibility test. There is no general publication requirement in
Part 97; the "documented publicly" language sits in §97.309(a)(4) and attaches
to techniques used with the *specified* codes.

Publishing a complete VARA specification would move the waveform into
§97.309(a)(4) territory ("any technique whose technical characteristics have
been documented publicly, such as CLOVER, G-TOR, or PacTOR") and would deliver
what RM-11831 (Kolarik, 2019) asked for — third-party monitorability with
freely available software. **Publication improves the regulatory posture of
every VARA user rather than threatening it.** Whether to publish is a
maintainer decision.

---

## 6. How to add an entry

Append to §3 with the next sequence number. Include: date, what the input is,
exactly where it came from, whether it is clean and why, and what it was used
for. Log the input **before or as** it informs code — never afterward.

If an input's cleanliness is uncertain, log it as a question and stop until it
is resolved. A contaminated input rejected at the door costs nothing; one
discovered after it has been built on costs the whole subtree.
