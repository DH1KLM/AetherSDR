#!/usr/bin/env python3
"""Survey every capture: burst layout in 512-sample units, and classify each
burst as DATA (16-ary 512-grid) / CONTROL (70-ary 2048-grid) / CW-ID / other."""
import sys, os, glob, json
sys.path.insert(0, '/tmp/claude-1000/-home-jeremy-AetherSDR/dba67c7c-7678-4762-97b3-32d25143fd29/scratchpad/wt-varac/tools')
import numpy as np
import vara_fec_capture as cap

SP = '/tmp/claude-1000/-home-jeremy-AetherSDR/dba67c7c-7678-4762-97b3-32d25143fd29/scratchpad'

def classify(x, a, b):
    n = b - a
    seg = x[a:b].astype(np.float64) / 32768.0
    out = dict(units=n / 512.0, samples=n)
    if n % 512:
        out['cls'] = 'nonmultiple'
        return out
    # CW identifier test (750 Hz sub-tone) on first 2048 samples if available
    if n >= 2048:
        spec = np.abs(np.fft.rfft(seg[:2048]))
        if spec[32] ** 2 > 0.15 * (spec[36:100] ** 2).sum():
            out['cls'] = 'cwid'
            return out
    # Energy distribution: DATA lives in 512-DFT bins 9..24 -> 843.75..2250 Hz
    # Control lives in 2048-DFT bins 29..98 -> 679.7..2296.9 Hz. Overlapping, so
    # use the symbol-grid coherence test instead: how concentrated is a
    # 512-point DFT of each 512 block vs a 2048-point DFT of each 2048 block.
    nb512 = n // 512
    r512 = []
    for i in range(nb512):
        m = np.abs(np.fft.rfft(seg[i*512:(i+1)*512]))
        o = np.sort(m)[::-1]
        r512.append(o[0] / max(o[1], 1e-15))
    out['r512_med'] = float(np.median(r512))
    out['r512_min'] = float(np.min(r512))
    if n % 2048 == 0:
        r2048 = []
        for i in range(n // 2048):
            m = np.abs(np.fft.rfft(seg[i*2048:(i+1)*2048]))
            o = np.sort(m)[::-1]
            r2048.append(o[0] / max(o[1], 1e-15))
        out['r2048_med'] = float(np.median(r2048))
    else:
        out['r2048_med'] = None
    if out['r512_med'] > 1000:
        out['cls'] = 'DATA'
    elif out['r2048_med'] is not None and out['r2048_med'] > 1000:
        out['cls'] = 'CTRL'
    else:
        out['cls'] = 'other'
    return out

res = {}
for path in sorted(glob.glob(os.path.join(SP, '*.wav'))):
    name = os.path.basename(path)
    try:
        x = cap.load_wav(path)
    except Exception as e:
        res[name] = dict(error=str(e)); continue
    bursts = cap.split_bursts(x)
    lst = []
    for i, (a, b) in enumerate(bursts):
        c = classify(x, a, b)
        c['i'] = i; c['a'] = int(a); c['b'] = int(b)
        lst.append(c)
    res[name] = dict(nbursts=len(bursts), bursts=lst)

with open(os.path.join(SP, 'wt-varac/an/survey.json'), 'w') as h:
    json.dump(res, h)

# print a compact table
for name, r in res.items():
    if 'error' in r:
        print(f"{name:22s} ERROR {r['error']}"); continue
    parts = []
    for c in r['bursts']:
        u = c['units']
        us = f"{u:g}"
        tag = {'DATA': 'D', 'CTRL': 'C', 'cwid': 'W', 'other': 'o',
               'nonmultiple': 'x'}[c['cls']]
        parts.append(f"{tag}{us}")
    print(f"{name:22s} {len(r['bursts']):3d}  " + " ".join(parts))
