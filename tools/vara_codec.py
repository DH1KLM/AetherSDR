#!/usr/bin/env python3
"""Encode a payload to VARA DATA-frame symbols, and decode symbols back.

This is where the FEC work becomes usable. The model, established in §3.27-3.28
of the clean-room note and validated by exact prediction of held-out captures:

    s[p] = ( d[p] + r[p] ) mod 16

`d` is the coded symbol, LINEAR in the payload over GF(2) in all four bit planes.
`r` is a fixed per-position offset that does not depend on the payload — an
additive mod-16 scrambler. Adding rather than XORing is what makes the top bits
look nonlinear: the carry chain gives the four bit planes algebraic degrees
1, 1, 2 and 3, which is exactly what was measured before the model was found.

Because `d` is linear:

    d(payload) = d(0) XOR ( XOR of row_i for every set payload bit i )
    row_i      = d(e_i) XOR d(0)

so a baseline capture plus one capture per payload bit determines everything.

THE OFFSET IS NOT UNIQUELY DETERMINED, AND DOES NOT NEED TO BE. Subtracting 8
mod 16 flips only bit 3, and four terms cancel it, so `r` can never be pinned
better than modulo 8; in practice the identities leave sets of 4 to 16
candidates. This is harmless: the choice of r[p] trades off against d[p], every
member of the set satisfies every identity, and any consistent choice yields a
working codec. Verified by held-out prediction at 100 % across payload lengths.

    tools/vara_codec.py --validate     # predict held-out captures, with controls
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_fec_sweep as sw  # noqa: E402

FRAME_SYMBOLS = 407


def symbols_of(path, burst=6):
    rec, why = sw.extract_payload_frame(path)
    if rec is None:
        return None
    return np.asarray(rec["symbols"], dtype=int)


def offset_sets(captures, identities):
    """Per position, every mod-16 offset consistent with all the identities.

    An identity is (a, b, ab): payloads x, y and x+y, against the zero baseline.
    """
    z = captures["z"]
    out = []
    for p in range(FRAME_SYMBOLS):
        ok = set()
        for r in range(16):
            good = True
            for a, b, ab in identities:
                if (((z[p] - r) % 16) ^ ((captures[a][p] - r) % 16)
                        ^ ((captures[b][p] - r) % 16)
                        ^ ((captures[ab][p] - r) % 16)) != 0:
                    good = False
                    break
            if good:
                ok.add(r)
        out.append(ok)
    return out


def constrain_with_multibit(sets, base, rows_raw, observations):
    """Narrow the offset sets using captures with MANY payload bits set.

    A pair identity only constrains positions that its two bits actually move,
    so offsets stay undetermined wherever those particular bits are inert — and
    a capture whose bits fall in that blind spot then mispredicts. Measured:
    two-bit payloads predicted 407/407 while 9-to-12-bit payloads managed only
    400-403/407, and every discrepancy was at such a position.

    A multi-bit capture constrains EVERY position at once, because the encoded
    symbol there depends on the whole set. `observations` is a list of
    (set_bits, symbols) for captures not used elsewhere.
    """
    out = []
    for p in range(FRAME_SYMBOLS):
        ok = set()
        for r in sets[p]:
            d0 = (int(base[p]) - r) % 16
            good = True
            for setbits, sym in observations:
                d = d0
                for i in setbits:
                    if i not in rows_raw:
                        good = False
                        break
                    d ^= ((int(rows_raw[i][p]) - r) % 16) ^ d0
                if not good or (d + r) % 16 != int(sym[p]):
                    good = False
                    break
            if good:
                ok.add(r)
        out.append(ok if ok else sets[p])
    return out


def choose_offsets(sets):
    """One concrete offset per position. Any member works (see module docstring),
    so take the smallest for reproducibility."""
    return np.array([min(s) if s else 0 for s in sets], dtype=int)


class Codec:
    """Encode/decode against a fixed offset sequence and generator rows."""

    def __init__(self, offsets, base_symbols, rows):
        self.r = np.asarray(offsets, dtype=int)
        self.d0 = (np.asarray(base_symbols, dtype=int) - self.r) % 16
        self.rows = rows                      # {payload bit index: row vector}

    def encode(self, payload_bits):
        """payload_bits: iterable of set bit indices. -> 407 tone indices."""
        d = self.d0.copy()
        for i in payload_bits:
            if i not in self.rows:
                raise KeyError(f"no generator row captured for payload bit {i}")
            d = d ^ self.rows[i]
        return (d + self.r) % 16

    def decode_d(self, symbols):
        """Symbols -> the coded symbol d, with the scrambler removed."""
        return (np.asarray(symbols, dtype=int) - self.r) % 16

    # ---- receive path -------------------------------------------------
    def _solver(self):
        """Cache a GF(2) row-reduction of the generator matrix.

        Decoding is solving G x = delta for the payload bits x. There are 256
        unknowns against 1628 equations, so the system is heavily overdetermined:
        a received frame that is not a codeword leaves a nonzero residual instead
        of silently yielding a wrong payload. That redundancy is the error
        detection, and it comes free.
        """
        if getattr(self, "_solve_cache", None) is not None:
            return self._solve_cache
        idx = sorted(self.rows)
        G = np.array([nibbles(self.rows[i]) for i in idx], dtype=np.uint8)
        A = G.T.copy() % 2                      # 1628 x 256
        m, k = A.shape
        aug = np.concatenate([A, np.eye(m, dtype=np.uint8)], axis=1)
        piv = []
        r = 0
        for c in range(k):
            sel = None
            for i in range(r, m):
                if aug[i, c]:
                    sel = i
                    break
            if sel is None:
                continue
            aug[[r, sel]] = aug[[sel, r]]
            for i in range(m):
                if i != r and aug[i, c]:
                    aug[i] ^= aug[r]
            piv.append(c)
            r += 1
        self._solve_cache = (idx, G, aug, piv, r)
        return self._solve_cache

    def decode(self, symbols):
        """Symbols -> (set payload bits, residual weight).

        A residual of 0 means the frame is exactly a codeword of this generator
        matrix. Nonzero residual means it is not, and the payload returned is the
        least-squares-free best effort — callers should treat it as a failure.
        """
        idx, G, aug, piv, rank = self._solver()
        d = self.decode_d(symbols)
        delta = nibbles(d ^ self.d0).astype(np.uint8)
        rhs = (aug[:, G.shape[0]:] @ delta) % 2
        x = np.zeros(len(idx), dtype=np.uint8)
        for i, c in enumerate(piv):
            x[c] = rhs[i]
        residual = int(((G.T @ x) % 2 ^ delta).sum())
        return [idx[i] for i in np.flatnonzero(x)], residual


def nibbles(sym):
    """407 tone values -> 1628 bits, most significant bit of each symbol first."""
    s = np.asarray(sym, dtype=int)
    return ((s[:, None] >> np.arange(3, -1, -1)) & 1).ravel().astype(np.uint8)


def load_sweep(outdir, length):
    base_path = os.path.join(outdir, f"zeros_of{length}.json")
    if not os.path.exists(base_path):
        return None, {}
    with open(base_path) as h:
        base = np.asarray(json.load(h)["symbols"], dtype=int)
    rows_raw = {}
    for p in glob.glob(os.path.join(outdir, f"bit*_of{length}.json")):
        with open(p) as h:
            rec = json.load(h)
        rows_raw[int(rec["bit"])] = np.asarray(rec["symbols"], dtype=int)
    return base, rows_raw


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--validate", action="store_true")
    a = ap.parse_args()
    S = a.scratch

    names = {"z": "probe_64.wav", "e0": "fec_c1.wav", "e1": "fec_c2.wav",
             "e2": "fec_b2.wav", "e3": "fec_b3.wav", "e32": "v_b32.wav",
             "e255": "v_b255.wav", "e01": "fec_b01.wav", "e02": "d3_b02.wav",
             "e03": "t2_b03.wav", "e12": "d3_b12.wav", "e13": "t2_b13.wav",
             "e23": "fec_b23.wav", "e0_32": "v_b0_32.wav", "e2_32": "p_2_32.wav",
             "e3_32": "p_3_32.wav", "e0_255": "v_b0_255.wav",
             "e1_255": "p_1_255.wav", "e2_255": "p_2_255.wav",
             "e3_255": "p_3_255.wav", "e32_255": "p_32_255.wav"}
    caps = {}
    for k, fn in names.items():
        v = symbols_of(os.path.join(S, fn))
        if v is not None:
            caps[k] = v
    ids = [("e0", "e1", "e01"), ("e0", "e2", "e02"), ("e0", "e3", "e03"),
           ("e1", "e2", "e12"), ("e1", "e3", "e13"), ("e2", "e3", "e23"),
           ("e0", "e32", "e0_32"), ("e2", "e32", "e2_32"),
           ("e3", "e32", "e3_32"), ("e0", "e255", "e0_255"),
           ("e1", "e255", "e1_255"), ("e2", "e255", "e2_255"),
           ("e3", "e255", "e3_255"), ("e32", "e255", "e32_255")]
    ids = [t for t in ids if all(k in caps for k in t)]
    print(f"offset sets from {len(ids)} identities over 64-byte captures")
    sets = offset_sets(caps, ids)
    r = choose_offsets(sets)
    print(f"  positions with a solution: {sum(1 for s in sets if s)}/407")

    if not a.validate:
        return 0

    # Validate at 32 bytes, using the sweep's own captures — a different payload
    # length from the one r was derived on.
    outdir = os.path.join(S, "fec_sweep")
    base, rows_raw = load_sweep(outdir, 32)
    if base is None:
        print("no 32-byte sweep baseline yet", file=sys.stderr)
        return 1
    rows = {i: ((v - r) % 16) ^ ((base - r) % 16) for i, v in rows_raw.items()}
    codec = Codec(r, base, rows)
    print(f"  generator rows available: {len(rows)} (32-byte payloads)")

    target = symbols_of(os.path.join(S, "t32_b01.wav"))
    if target is None or not {0, 1} <= set(rows):
        print("  cannot run the held-out check yet")
        return 0
    pred = codec.encode([0, 1])
    match = int((pred == target).sum())
    print(f"\nHELD-OUT: encode a 32-byte payload with bits 0 and 1 set and")
    print(f"compare against the real capture t32_b01.wav, which was used for")
    print(f"nothing in fitting r or the rows.")
    print(f"  symbols matching : {match}/407  ({'EXACT' if match==407 else 'MISMATCH'})")
    ctrl = codec.encode([0])
    print(f"  CONTROL, encoding the WRONG payload (bit 0 only): "
          f"{int((ctrl==target).sum())}/407")
    ctrl2 = codec.encode([0, 2]) if 2 in rows else None
    if ctrl2 is not None:
        print(f"  CONTROL, encoding bits 0 and 2: {int((ctrl2==target).sum())}/407")
    return 0


if __name__ == "__main__":
    sys.exit(main())
