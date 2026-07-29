# VARA / VarAC Clean-Room Design Note

Status: **Phase 1 — host-interface client implemented from published sources.
No captures taken and no VARA or VarAC software installed as of this entry;
everything so far derives from the documents and open-source references logged
in §3.**

Purpose: an append-only provenance record for AetherSDR's native VARA / VarAC
interoperability work, per Constitution **Principle IV (Every Contribution Is
Clean-Room)**. Every input that informs the implementation is logged here,
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
- Public documentation: the VarAC manual, VARA's published TCP interface,
  third-party implementations' public docs (BPQ32, RadioMail, Winlink).
- Behaviour observed on the wire: RF captures, audio-domain captures of a
  modulator's output, bytes observed on a TCP socket.
- Our own analysis, design and implementation derived from the above.

**Contaminated — forbidden, without exception:**

- Opening `VARA.exe`, `VarAC.exe`, or any of their DLLs in a disassembler,
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
| **Signal generation** | Windows environment (WINE or one VM) running VarAC + VARA | Accepts the VARA / VarAC terms of use. Generates audio and TCP traffic. |
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

### 3.2 — 2026-07-28 · VarAC manual, EN v15.0.18

- **What:** "VarAC manual in English", by ON2AD Pat, 158 pp, dated 22/07/2026.
- **Source:** public PDF, operator-supplied copy at `~/Downloads/`.
- **Status:** **clean** — published user documentation.
- **Used for:** enumerating VarAC's external interfaces (CAT paths, VARA modem
  configuration, logger TCP/UDP output, ADIF), the multi-instance cluster
  pattern used for the bench rig, and the feature inventory that Phase 2 must
  exercise (CQ, beacons, broadcasts, VMail, file transfer, alert tags,
  pathfinder, digipeater chaining).
- **Note:** documents the UI and configuration of every feature and the wire
  format of none. It is not a protocol source.

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
