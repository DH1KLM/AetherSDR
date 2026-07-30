#!/bin/bash
# Full capture campaign, in priority order, resumable.
#
# WHY THIS EXISTS. The first campaign's captures lived in a session scratchpad
# under /tmp and were destroyed by a reboot: about 100 WAVs, 281 extracted
# generator rows, and the exported model. The branch survived because git
# worktree objects live in the main repository, but the experimental DATA did
# not. Two changes follow from that: captures go to a persistent directory, and
# the distilled results (symbol JSONs and the model) are COMMITTED, because they
# are the product of ~9 hours of bench time and are what makes the model
# reproducible without repeating it.
#
# Order is by value, so an interruption loses the least:
#   1. identity captures  — pairs needed to constrain the scrambler offsets
#   2. 32-byte sweep      — 256 generator rows
#   3. multi-bit captures — pin the offsets at every position
#   4. 64-byte sweep      — generator rows 256-511
set -u
D="${VARA_DATA:-/home/jeremy/aethersdr-vara-data}"
W="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$D" "$D/fec_sweep"
cd "$W"
cap(){ # cap <wavname> <args...>
  local n="$1"; shift
  if [ -s "$D/$n" ] && [ "$(stat -c%s "$D/$n")" -gt 1000000 ]; then
    echo "  $n: already present, skipping"; return 0
  fi
  echo "=== $n : $* ==="
  timeout 400 python3 tools/vara_fec_capture.py "$@" --wav "$D/$n" 2>&1 | tail -1
  sleep 5
}
echo "### phase 1: identity captures"
cap probe_64.wav   --zeros 64
cap probe_32.wav   --zeros 32
cap fec_c1.wav     --onebit 64,0
cap fec_c2.wav     --onebit 64,1
cap fec_b2.wav     --onebit 64,2
cap fec_b3.wav     --onebit 64,3
cap v_b32.wav      --onebit 64,32
cap v_b255.wav     --onebit 64,255
cap fec_b01.wav    --bits 64,0,1
cap d3_b02.wav     --bits 64,0,2
cap t2_b03.wav     --bits 64,0,3
cap d3_b12.wav     --bits 64,1,2
cap t2_b13.wav     --bits 64,1,3
cap fec_b23.wav    --bits 64,2,3
cap v_b0_32.wav    --bits 64,0,32
cap p_2_32.wav     --bits 64,2,32
cap p_3_32.wav     --bits 64,3,32
cap v_b0_255.wav   --bits 64,0,255
cap p_1_255.wav    --bits 64,1,255
cap p_2_255.wav    --bits 64,2,255
cap p_3_255.wav    --bits 64,3,255
cap p_32_255.wav   --bits 64,32,255
cap v_b012.wav     --bits 64,0,1,2
cap t2_b013.wav    --bits 64,0,1,3
cap t32_b0.wav     --onebit 32,0
cap t32_b1.wav     --onebit 32,1
cap t32_b01.wav    --bits 32,0,1
echo "PHASE1_DONE"
echo "### phase 2: 32-byte generator sweep (256 rows)"
python3 tools/vara_fec_sweep.py --length 32 --bits 0-255 --outdir "$D/fec_sweep"
echo "PHASE2_DONE"
echo "### phase 3: multi-bit captures"
python3 - "$D" <<'PY' > "$D/mb_cmds.sh"
import json,random,sys
D=sys.argv[1]
random.seed(20260730)                     # same seed as the lost run
spec={f"mb_{k}":sorted(random.sample(range(256), random.choice([24,32,40,48])))
      for k in range(1,25)}
json.dump(spec,open(f"{D}/mb_spec.json","w"))
rp={"rp_1":[3,11,19,27,35,43,51,59,67],"rp_2":[1,5,9,13,17,21,25,29,33,37,41],
    "rp_3":[0,7,14,21,28,35,42,49,56,63],"rp_4":[2,6,10,22,34,46,58,66,68,69],
    "rp_5":[4,8,12,16,20,24,44,52,60,64,65],"rp_6":[15,23,31,39,47,55,61,62,63],
    "rp_7":[18,26,30,38,45,50,53,57,69],"rp_8":[0,1,2,3,4,5,6,7,8,9,10,11]}
for k,v in list(rp.items())+list(spec.items()):
    print(f'cap {k}.wav --bits 32,{",".join(map(str,v))}')
PY
source /dev/stdin < <(cat <(declare -f cap) "$D/mb_cmds.sh")
echo "PHASE3_DONE"
echo "### phase 4: 64-byte generator sweep (rows 256-511)"
python3 tools/vara_fec_sweep.py --length 64 --bits 256-511 --outdir "$D/fec_sweep"
echo "CAMPAIGN_DONE"
