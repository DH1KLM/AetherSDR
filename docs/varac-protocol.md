# VarAC Application Protocol — Observed Notes

**Status: incomplete. Very early.** This documents only what has been directly
observed on the wire, byte for byte. Everything not observed is marked unknown
rather than inferred. Do not implement against a guess in this file.

Provenance and method: see [`varac-cleanroom-design.md`](varac-cleanroom-design.md).
All bytes here were read from the data socket of a VARA modem on a bench with no
radio and no RF, while a genuine VarAC v15.0.18 instance drove a second modem.
Nothing was disassembled.

---

## 1. Where this sits

VarAC speaks its own protocol **inside** a VARA ARQ link. VARA delivers the
payload de-framed and error-corrected, so what a peer receives on its data
socket is exactly what VarAC wrote — no VARA framing to strip.

```
VarAC  ->  VARA modem  ->  (audio / RF)  ->  VARA modem  ->  our data socket
                                                              ^ bytes observed here
```

## 2. Hard prerequisite: bandwidth must match

VarAC sets its modem to **500 Hz** on startup — its own log records
`Setting Bandwith to 500Hz`. A peer at 2300 Hz **cannot** form an ARQ link with
it, and the failure presents as an unexplained connect timeout with no
diagnostic. Match the bandwidth first.

VarAC also runs with **`Unattended links enabled`**, so it auto-accepts inbound
connections without operator interaction.

## 3. Observed: the first message on an inbound connection

Sequence, with our side as the caller:

```
we send : CONNECT KK7GWY-9 KK7GWY        (VARA host command, BW500)
modem   : CONNECTED KK7GWY-9 KK7GWY 500
+6.8 s  : VarAC sends 5 bytes on the data channel
```

Those bytes, verbatim:

```
00000000  33 20 3c 51 3e                    |3 <Q>|
```

i.e. the ASCII string `3 <Q>`.

**What this establishes:**

* **The protocol is plaintext ASCII.** Not compressed, not binary, not
  obfuscated. This is the single most consequential observation so far — it
  means the protocol can be read directly rather than reverse-engineered from
  entropy.
* **It uses angle-bracket-delimited tokens** — `<Q>` here.
* VarAC speaks **first** on an inbound connection, unprompted.

**What is NOT established, and must not be assumed:**

* What `Q` means. "Query" is a guess, not an observation.
* What the leading `3 ` is. Candidates: a protocol version, a message counter,
  a session or slot id, a length. **Unknown.**
* Whether the leading field is space-delimited from the token by convention or
  coincidence.
* Whether messages are terminated (no terminator was observed — the 5 bytes
  arrived as one complete read with no trailing CR or LF).
* The rest of the vocabulary. Only one token has ever been seen.

## 4. Observed: what happens when the reply is wrong

After receiving `3 <Q>` we sent the non-protocol text
`HELLO FROM AETHERSDR\r`. The link **dropped ~16 s later** (`DISCONNECTED`).

That is consistent with VarAC expecting a specific handshake response and
tearing down when it does not arrive — but a timeout unrelated to our payload
would look identical. **Not yet distinguished.** The controlled test is to
connect and send *nothing*, and compare how long the link survives.

## 4a. The framing is `<decimal-length> <payload>`

VarAC's own database records the outgoing entry as exactly:

```
<Q>
```

with no prefix (`datastream` table, `datastream_entry_type_id = 2` = Outgoing,
`chat_id = 1`). The bytes on the wire were `3 <Q>`. `<Q>` is **three
characters**.

So the leading numeric field is a **decimal length of the payload that
follows**, separated by a single space:

```
<len> SP <payload>
```

That resolves the "unknown leading field" from §3 above. It still needs
confirming against a payload of a different length — one observation is
consistent with a length, a version, and a counter all at once, and only a
second differently-sized message separates them.

## 4b. VarAC rejects malformed payloads and tears down the link

Its own database records, immediately after we sent `HELLO FROM AETHERSDR\r`:

```
Error: UNABLE TO DECODE DATA FROM KK7GWY-9. ABORTING. See VarAC.log for more
info. In most cases a VARA packet from another QSO on the same frequency
slipped into this QSO and corrupted the link. VarAC will now restart the VARA
modem. Standby....
```

This settles the ambiguity flagged in §4: the link dropped because **our
payload was invalid**, not because of an idle timeout. VarAC parses strictly and
treats an unparseable payload as channel corruption.

Note the practical consequence: a wrong guess does not merely fail, it makes
VarAC restart its modem. Experiments need spacing.

## 4c. VarAC's database is a plaintext protocol transcript

`drive_c/VarAC/VarAC.db` (SQLite) is written by VarAC and is therefore an
external output — a clean input under Principle IV. The `datastream` table
records both directions with `datastream_entry_type`:

| id | meaning |
|---|---|
| 1 | Incoming |
| 2 | Outgoing |
| 3 | Info (local status, not on the wire) |

Observed session, in order: `INCOMING CONNECTION REQUEST. Still don't know who
that is.` -> `CONNECTED TO KK7GWY-9 (BANDWIDTH: 500 FREQUENCY: 14.105.000)` ->
outgoing `<Q>` -> decode error -> `QSO SUMMARY ... Duration: 00:00:28` ->
`DISCONNECTED FROM KK7GWY-9`.

The `qso` table additionally records `varac_version` (V15.0.18), `snr_sent`,
mode `DYNAMIC` / submode `VARA HF`, and `comments: LINK using VarAC`. The
`cqframe` table stores beacons and CQs with `frequency`, `bandwidth`, `slot`,
`locator`, `is_emcomm`, `is_bbs`, `is_email_gateway`, `is_ai_gateway`,
`diploma` — which names the fields a CQ/beacon frame can carry, even though
their wire encoding is still unobserved.

**This is the single most efficient source available**: it gives both sides of
any exchange in plaintext, so a feature can be exercised in the GUI and its
protocol entries read straight out of the database.

## 4d. RETRACTED as "confirmed" — the one positive result did not reproduce

**Read §4e before relying on anything in this section.** The heading below
originally said CONFIRMED. It should not have. The single positive observation
was not reproduced in the very next batch, and one success is not a finding.

## 4d (as originally written): framing is `<len> SP <payload>`, length exact, no terminator

Five controlled trials, each connecting to a live VarAC and sending one
candidate reply to its opening `3 <Q>`. Success was judged **not** by whether
the link survived but by whether VarAC's own database recorded an **Incoming**
`datastream` entry — i.e. whether VarAC actually parsed it.

| we sent | link | VarAC parsed it |
|---|---|---|
| *nothing* (control) | **dropped @ 106 s** | — |
| `3 <Q>` | held | **YES — Incoming `<Q>`** |
| `<Q>` | held | no |
| `3 <Q>\r` | held | no |
| `3 <A>` | held | no |

**Conclusions:**

1. **`3 <Q>` is correctly framed and VarAC parses it.** This is the first
   message we have sent that VarAC understood — it appears in its database as an
   Incoming entry, exactly as its own outgoing `<Q>` does.
2. **The length is exact and there is NO terminator.** `3 <Q>\r` fails because
   four bytes follow a declared length of three. A trailing CR is not tolerated.
   This is the strongest single piece of evidence for the length reading over
   the version or counter readings.
3. **Bare `<Q>` with no length prefix is not parsed** — the prefix is mandatory.
4. **Holding the link does not mean being understood.** Every trial that sent
   *any* bytes held the link; only the correctly framed one was parsed. Judging
   by link survival alone would have scored four failures as successes. The
   database is the only reliable oracle here.
5. **Idle timeout is ~106 s** with no data sent.
6. `3 <A>` is well-framed but produced no Incoming entry, so `A` is presumably
   not a token VarAC recognises. It ignores unknown tokens silently rather than
   erroring — unlike malformed framing, which makes it abort the link (§4b).

**What `<Q>` means is still unknown.** Both sides send it on connect. VarAC's UI
has a "QRZ?" control, so "who are you" is a plausible reading, but that is a
guess and is not recorded here as fact.

## 4e. The positive result is NOT reproducible — current honest state

A follow-up batch sent four payloads including **the identical `3 <Q>` that had
been parsed**, and VarAC recorded **no Incoming entry for any of them**:

| we sent | link | parsed |
|---|---|---|
| `5 HELLO` | held | no |
| `28 HELLO FROM AETHERSDR TEST` | held | no |
| `3 HELLO` (deliberately wrong length) | held | no |
| `3 <Q>` (the previously-successful control) | held | **no** |

So `3 <Q>` parsed **once**, in one trial, and has not parsed since. Everything
in §4d that depends on that single success is therefore **unproven**, including:

* that the leading field is a length rather than a version or counter;
* that the absence of a terminator is what made `3 <Q>\r` fail;
* that a missing prefix is what made bare `<Q>` fail.

Those readings are all still *consistent* with the data. None is established.

**Candidate explanations, untested:**

1. **Our payload never actually reached VarAC.** At 500 Hz with the free
   licence capping throughput, a few bytes take a long time to traverse an ARQ
   link. If the modem's TX buffer had not drained before we disconnected, VarAC
   would never have seen it — and the one success would be the one run where it
   happened to get through. The next test logs `BUFFER` to settle this.
2. Timing or session-state dependence in VarAC's parser.
3. `<Q>` is accepted only once per session or only at a specific handshake step.

**Method note.** This is the second time in this investigation that a result
survived one observation and died on repetition, and the second time an
apparatus problem masqueraded as a protocol finding (after the KISS-port
collision in the provenance doc §3.20). The rule that keeps proving itself:
**one observation is a hypothesis, not a finding — repeat it before writing it
down as fact.** This document should not have said CONFIRMED on a single trial.

## 5. Open questions, in the order worth answering

1. Does the link survive if we send nothing? (separates "wrong reply" from
   "idle timeout")
2. What does VarAC send if we echo `3 <Q>` back, or reply `<Q>`?
3. What is the leading numeric field — does it change between connections?
4. What tokens appear for a chat message, a VMail, a file transfer, a
   disconnect?
5. Is there a terminator, or is each write one message?

## 6. Implementation status

**Nothing is implemented.** There is no `VaracProtocol` in the tree, and there
should not be until the handshake is understood well enough that a first
exchange can be completed deliberately rather than by accident.
