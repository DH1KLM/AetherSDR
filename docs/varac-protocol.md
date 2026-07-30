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
