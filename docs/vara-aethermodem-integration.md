# VARA HF in AetherModem — integration design

Where the VARA work lands in the application, and what it needs from the radio.
The clean-room provenance record for the protocol and waveform work is
[`vara-cleanroom-design.md`](vara-cleanroom-design.md); this document is only
about integration.

`src/core/VaraProtocol.{h,cpp}` and `src/core/VaraClient.{h,cpp}` exist and are
verified against a live modem (63 protocol assertions, plus a two-instance
bench that establishes real ARQ links and transports payload byte-exactly).
They are currently wired into nothing. This is the plan for wiring them in.

## 1. Two topologies, and why the distinction drives everything

VARA can reach the air two ways, and they are **not** variations on a theme —
they land in different places in the DAX TX policy, because they differ in
whether an *OS audio device* is claimed.

**Topology A — external `VARA.exe` is the modem.** The operator's installed VARA
generates the audio itself, in its own process, through a soundcard. AetherSDR
is the *host application* (the role Winlink Express and VarAC play): it holds
the TCP session, and it owns PTT and the slice. This works today, needs no
FEC, and is the shippable increment.

**Topology B — AetherSDR is the modem.** AetherSDR synthesizes the VARA
waveform itself and no `VARA.exe` exists. This is what the modulation and FEC
recovery is for. Topology B subsumes A's control layer, so nothing built for A
is wasted.

## 2. What AetherSDR must provide — the correction that shaped this design

An early draft of this plan said VARA "drives an external modem rather than the
radio directly, so the page may need less radio coupling." **That is wrong, and
it is worth recording why**, because it inverts the requirement.

VARA does not talk to a radio. It produces audio and asserts PTT, and expects
*something else* to own the transceiver. In AetherSDR's architecture that
something else is AetherSDR: it owns DAX TX/RX, PTT, and slice frequency, mode
and filter. So a VARA page needs **more** radio coupling than a page that just
shells out to an external program, and it is unambiguously a transmit feature
under **Principle VI** — never transmit without operator intent.

Concretely, per session the page must own:

| Concern | Why it cannot be left to the operator |
|---|---|
| Slice frequency | VARA has no concept of frequency; it emits audio |
| Mode | Must be a data mode; `USB`/`DIGU` per band convention |
| **Filter width** | **Must match the VARA bandwidth.** A mismatch silently prevents any link — no error, just a connect that never completes. This cost real bench time (§3.16). |
| PTT | VARA asserts an abstract PTT; AetherSDR keys the radio |
| DAX TX/RX | The audio path in both topologies, by different routes |

## 3. DAX TX policy — the two topologies are two existing policy classes

`src/core/DaxTxPolicy.h` already draws exactly the line that matters. Reading
`evaluateDaxTxPolicy()`, requests fall into two groups:

* **Internally-generated waveform, sent as VITA-49 directly** —
  `TciTxAudio`, `RadeModemTx`, `AetherModemAx25Tx`, `WsprBeacon`. These are
  allowed on **every** platform, because they never claim an OS audio device and
  so do not contend with SmartSDR DAX2 for the Windows device layer. The radio's
  `dax_tx` stream slot is independent and each client registers its own.
* **An external program claiming an OS audio device** — `HostedDaxBridge`
  (allowed only where hosted DAX exists: macOS, or Linux with PipeWire) and
  `ExternalDaxRouteOnly` (refused, because DAX2 owns the devices).

That maps onto the two topologies without inventing anything:

**Topology A needs no new policy reason.** `VARA.exe` claims an audio device, so
it is a hosted-DAX bridge case on Linux/macOS, and on Windows the correct
behaviour is for AetherSDR *not* to create a stream at all — SmartSDR DAX2 owns
the device and VARA selects it directly, which is the standard Winlink setup.
The existing `HostedDaxBridge` / `ExternalDaxRouteOnly` decisions already
produce that outcome.

**Topology B needs one new reason.** AetherSDR generating the waveform is the
same class as AX.25, RADE and WSPR, so:

```cpp
case DaxTxRequestReason::VaraModemTx:
    // AetherSDR synthesizes the VARA CPFSK waveform internally and sends
    // VITA-49 packets directly via AudioEngine::sendModemTxAudio(). Like
    // AX.25/RADE/WSPR it claims no OS audio device, so it needs its own
    // dax_tx stream on every platform.
    return {true, QStringLiteral("vara_sends_vita49_directly")};
```

plus the enum entry and its `daxTxRequestReasonName()` string
(`vara_modem_tx`). Three existing cases are already exactly this shape.

## 4. Where it goes in the UI

`Ax25HfPacketDecodeDialog` (AetherModem) is a `PersistentDialog` holding a
`QStackedWidget` behind a row of tab buttons wired through a `QButtonGroup`:

```cpp
m_dstarTab = tabButton(QStringLiteral("D-STAR"), false, tabsFrame);
tabGroup->addButton(m_dstarTab, 4);
tabs->addWidget(m_dstarTab, 1);
...
m_dstarPage = new DStarModemPage(m_radio, m_tabStack);
```

`DStarModemPage` is the precedent to follow: a self-contained `QWidget` taking
`(RadioModel* radio, QWidget* parent)`, owning its own header, footer and
refresh methods. A `VaraModemPage` with the same shape becomes index 5.

This keeps `Ax25HfPacketDecodeDialog` from growing — it is already 3906 lines,
and the D-STAR page demonstrates the pattern for keeping a new mode's
complexity out of it.

## 5. Settings — Principle V

`class TncSettings` in the same file is the template: one nested JSON object
under a single root key, with a `migrateLegacy()` path for flat keys. A
`VaraSettings` follows it exactly, under a `vara` root:

* modem host and command port (data port is command port + 1 — the modem's own
  convention, not a free choice)
* callsign, bandwidth (500 / 2300 / 2750), compression
* Topology A only: path to `VARA.exe` and whether AetherSDR launches it
* radio coupling: which slice, and whether to enforce the filter width

Bandwidth belongs in settings *and* must be pushed to the radio's filter on
session start, since those two must agree or nothing links.

## 6. What is deliberately not in scope

* **`TUNE`** — the modem's carrier-tune command keys the transmitter. Parsed by
  `VaraProtocol` so a session transcript is understood, never issued.
* **`ENCRYPTION`** — parsed, never issued; §97.113(a)(4).
* **Winlink / B2F** — a separate feature that would sit *on top of*
  `VaraClient`, reusing this transport. Published protocol, vendor-blessed, open
  reference implementations. Noted as the natural follow-on, not part of this.

## 7. Order of work

1. `VaraSettings` + tests (no UI, no radio — pure and independently testable).
2. `DaxTxRequestReason::VaraModemTx` + policy case + name string.
3. `VaraModemPage` shell as stack index 5, RX/monitor only — no TX controls, so
   nothing can key a radio before the TX path is reviewed.
4. Session control driven by `VaraClient`: connect, link state, payload in/out.
5. TX: PTT and DAX wiring, behind explicit operator action per Principle VI.

Steps 1–3 are Topology A groundwork and depend on nothing outstanding. Step 5 is
where Principle VI review matters most.

## Mercury (MercuryV2) engine

MercuryV2 — Rhizomatica, GPL-3.0, `github.com/Rhizomatica/mercury`, branch
`mercuryv2` — is the genuinely cross-platform half of AetherHF. VARA is a
Windows binary; Mercury is C, builds on Linux, macOS, Windows and Android, and
is what lets AetherSDR do ARQ HF without emulation.

### Why it is a process, not a library

Mercury is a standalone binary: its own Makefile, its own audio backends for
every platform (`alsa`, `pulse`, `oss`, `coreaudio`, `aaudio`, `dsound`,
`wasapi`, `jack`, plus `shm`/`null`/`fifo` for testing), and its own radio
control via Hamlib. It exposes a **VARA-compatible TCP host interface**
precisely so applications drive it from outside; its own GUI, `mercury-qt`,
does exactly that.

Vendoring 1300 files and a second audio stack into this build would buy nothing
and would put the Windows and macOS builds at risk for a dependency the
operator can install as a package. So AetherSDR supervises the process and
speaks to it over TCP.

### Why there is no MercuryClient

Mercury implements VARA's host protocol deliberately — same CR-terminated ASCII
commands, same base/base+1 port layout, same `CONNECTED`/`DISCONNECTED`/`BUFFER`
/`SN`/`BITRATE` responses. `VaraClient` already speaks all of it, and
`VaraProtocol` already parses every message Mercury emits. A second client would
be a second thing to keep in step for no gain, so the same one drives both and
`AetherHfSettings::active*()` supplies whichever engine's host and ports are
selected.

The differences are small and mostly additive: Mercury has `PUBLIC`, `RETRIES`
and `CALLINT`, which VARA has no equivalent for; it treats `COMPRESSION`, `P2P`
and `IGNOREKISSDCD` as no-ops accepted for compatibility; and it has no notion
of registration or encryption, so the VARA-only parts of the page stay hidden
when Mercury is selected.

### Ports

Mercury's own default base port is **8300 — the same as VARA's**. AetherSDR
defaults Mercury to **8400** instead, because sharing the default would make the
two engines collide the moment an operator runs them together, which is exactly
what comparing them requires. Mercury takes the base port with `-p`.

### Audio

Mercury opens the sound devices itself rather than being handed audio, so the
capture and playback device settings are how it gets pointed at the same virtual
cables the rig is on. On a FLEX that means the DAX devices; on a rig with a USB
codec it is that codec. Leaving the sound system blank uses Mercury's own
per-platform default.

### Launching

Optional, and off by default. An operator may already run Mercury themselves, or
run it on another machine entirely — in which case AetherSDR needs only a host
and port. When launching is enabled, `MercuryProcess` starts the binary, merges
its output into the page's event log, and stops it on disconnect. The connection
waits a short grace period after the process starts, because the control port is
not bound the instant the process exists and connecting too early fails in a way
that reads as a wrong host.
