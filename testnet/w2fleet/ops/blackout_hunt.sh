#!/bin/bash
# §13 blackout hunt at d200 (the rung where §12 measured the class, ~2/72),
# CRASH-RESUMABLE: the host has hard-reset 21 times and this hunt is longer than
# the mean time between crashes, so it must resume itself rather than restart.
#
# Checkpoint lives in blackout_hunt.ckpt.json, written ATOMICALLY by the python
# (tmp + os.replace, never appended) so it is structurally immune to the
# crash-NUL-glue class that broke the ladder's vacuity check. The done-callers
# list and per-caller roll counter mean a restart re-enters where it stopped.
# Terminates on the first catch — no further stakes burned.
#
# Callers are rotated because each has its OWN quorum and §12 saw blackouts on
# caller1 and caller3; a single-caller hunt could miss the population.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
# Single source of truth: the exhaustion check below MUST test the same roster
# the sweep walks, or "all callers done" can be declared while one still has budget.
CALLERS="caller3 caller1 caller2 caller7"
LOG=$W2/blackout_hunt.log
CKPT=$W2/blackout_hunt.ckpt.json
LOCK=$W2/ladder_ckpt.lock
say(){ echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
exec 9>"$LOCK"; flock -n 9 || { say "SKIP: ladder lock held"; exit 0; }

finished(){ python3 - "$CKPT" <<'PY' 2>/dev/null
import json,sys
try: print("yes" if json.load(open(sys.argv[1])).get("finished") else "no")
except Exception: print("no")
PY
}
done_caller(){ python3 - "$CKPT" "$1" <<'PY' 2>/dev/null
import json,sys
try: print("yes" if sys.argv[2] in json.load(open(sys.argv[1])).get("done_callers",[]) else "no")
except Exception: print("no")
PY
}

outcome(){ python3 - "$CKPT" <<'PY' 2>/dev/null
import json,sys
try: print(json.load(open(sys.argv[1])).get("outcome") or "caught")
except Exception: print("caught")
PY
}

# ★ FIX 2026-08-19 — BUDGET EXHAUSTION IS ALSO A TERMINAL OUTCOME.
# `finished` was set ONLY on a catch, so once every caller had exhausted its
# budget the checkpoint stayed `finished: false` for ever: EVERY boot re-applied
# d200 netem to all 161 veths, skipped all four done callers, and cleared it
# again -- ~60s of fleet-wide latency perturbation per boot for ZERO samples,
# and a silent contaminant for any timing measurement taken near a boot
# (3 no-op passes fired: 04:37, 05:49, 08:53). The hunt is over either way; the
# distinction the old flag carried is preserved in `outcome`, not in `finished`.
mark_exhausted(){ python3 - "$CKPT" "$CALLERS" <<'PY' 2>/dev/null
import json,os,sys
p=sys.argv[1]; roster=sys.argv[2].split()
raw=None
try:
    with open(p,errors="replace") as f: raw=f.read()
    st=json.loads(raw)            # STRICT: no NUL-repair here on purpose.
except Exception: sys.exit(0)
# A torn checkpoint may have lost roll counts, and `finished` is TERMINAL --
# never declare the hunt over from a damaged file. The reader repairs NULs so the
# hunt can RESUME; marking it finished must be the one place that refuses to.
if raw is None or "\0" in raw: sys.exit(0)
if st.get("finished"): sys.exit(0)
if not all(c in st.get("done_callers",[]) for c in roster): sys.exit(0)
st["finished"]=True; st["outcome"]="exhausted"
tmp=p+".tmp"                      # atomic, same discipline as the python's ckpt_write
with open(tmp,"w") as f:
    json.dump(st,f); f.flush(); os.fsync(f.fileno())
os.replace(tmp,p)
print("marked")
PY
}

if [ "$(finished)" = yes ]; then
  if [ "$(outcome)" = exhausted ]; then
    say "SKIP: checkpoint FINISHED (budget exhausted on every caller, no catch)"
  else
    say "SKIP: checkpoint FINISHED (blackout already caught)"
  fi
  exit 0
fi

say "=== BLACKOUT HUNT (resumable): applying d200 ==="
"$W2/netem_mesh.sh" apply 200 >>"$LOG" 2>&1
for _ in $(seq 1 16); do
  p50=$("$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}')
  [ -n "$p50" ] && [ "$p50" -ge 360 ] && { say "convergence p50=${p50}ms"; break; }
  sleep 15
done

for c in $CALLERS; do
  [ "$(finished)" = yes ] && { say "caught — stopping sweep"; break; }
  if [ "$(done_caller "$c")" = yes ]; then say "--- $c already exhausted (checkpoint) — skip ---"; continue; fi
  say "--- hunting on $c ---"
  HUNT_CALLER=$c HUNT_WANT=1 HUNT_BUDGET=30 "$W2/blackout_hunt.py" 2>&1 \
      | tee -a "$LOG" | grep -a "CAUGHT\|caught\|budget exhausted\|quorum\|resuming"
done

if [ "$(finished)" != yes ] && [ -n "$(mark_exhausted)" ]; then
  say "all callers exhausted their budget — checkpoint marked FINISHED (outcome=exhausted); no further passes will fire"
fi

say "--- clearing netem ---"; "$W2/netem_mesh.sh" clear >>"$LOG" 2>&1
if [ "$(finished)" = yes ]; then
  if [ "$(outcome)" = exhausted ]; then say "=== BLACKOUT HUNT DONE — BUDGET EXHAUSTED, no catch ==="
  else say "=== BLACKOUT HUNT DONE — CAUGHT ==="; fi
else
  say "=== BLACKOUT HUNT DONE — no catch this pass (budget remains) ==="
fi
