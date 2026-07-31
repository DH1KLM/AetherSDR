#!/usr/bin/env python3
"""Extract connect frames from a directory of captures once, and cache them.

Every analysis so far has re-run the extractor over every WAV, and the extractor
is not cheap: it searches for the acquisition preamble around each burst and then
scores tone purity over 41 symbols. At a few hundred captures that was tolerable.
At thirteen hundred it is minutes per question, which is enough friction to
discourage asking questions — the wrong property for an analysis tool.

So: extract in parallel, write a JSON index keyed by callsign, and skip anything
already indexed whose file has not changed. Analyses then load the index.

The index stores the frame exactly as the extractor returned it, including its
tone-purity acceptance. A capture that yields no clean frame is recorded as null
rather than omitted, so a later run does not keep retrying it and so the failure
is visible rather than silent.

    tools/vara_connect_index.py --data DIR [--out DIR/index.json]
"""

import argparse
import glob
import json
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vara_callsign_analyse as A  # noqa: E402


def extract(path):
    try:
        body = A.connect_body(path)
    except Exception:
        body = None
    return path, (list(body) if body else None)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    a = ap.parse_args()

    index = {}
    if os.path.exists(a.out):
        with open(a.out) as fh:
            index = json.load(fh)

    paths = []
    for d in a.data:
        paths += sorted(glob.glob(os.path.join(d, "cs_*.wav")))

    todo = []
    for p in paths:
        call = re.match(r"cs_(.+)\.wav$", os.path.basename(p)).group(1)
        st = os.stat(p)
        prev = index.get(call)
        if prev and prev.get("size") == st.st_size and prev.get("mtime") == int(st.st_mtime):
            continue
        todo.append(p)

    print(f"{len(paths)} captures, {len(todo)} to (re-)extract, "
          f"{len(paths)-len(todo)} cached", flush=True)

    if todo:
        with ProcessPoolExecutor(max_workers=a.jobs) as ex:
            for n, (path, body) in enumerate(ex.map(extract, todo, chunksize=4), 1):
                call = re.match(r"cs_(.+)\.wav$", os.path.basename(path)).group(1)
                st = os.stat(path)
                index[call] = {"frame": body, "size": st.st_size,
                               "mtime": int(st.st_mtime), "path": path}
                if n % 100 == 0:
                    print(f"  {n}/{len(todo)}", flush=True)

    with open(a.out, "w") as fh:
        json.dump(index, fh)
    good = sum(1 for v in index.values() if v["frame"])
    print(f"indexed {len(index)} callsigns, {good} with a clean frame, "
          f"{len(index)-good} without")
    return 0


if __name__ == "__main__":
    sys.exit(main())
