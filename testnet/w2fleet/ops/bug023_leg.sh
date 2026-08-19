#!/bin/bash
# Composition proof — BUG-023 leg (adapted trigger, recorded honestly):
# no PTXCOALESCE has ever fired on this fleet (pool never crosses the UTXO
# threshold at current demand), so the pool-mutating block class in the
# -checkblocks window is the SETTLE/PAYOUT block. Wait for the h2640 payout,
# restart non-quorum-member gm18 inside the 6-block window, assert its
# post-restart lottery pool equals the fleet's at the same tip (the
# 15.00-not-14.00 assertion shape, BUG-023's original clobber signature).
set -u
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
python3 - <<'EOF'
import time, subprocess
from harness.cluster import W2Cluster
c = W2Cluster(98)
gm01 = c.gms[0]
gm18 = next(n for n in c.gms if n.name == "gm18")
print("[023] waiting for settle @2640…")
while True:
    st = gm01.call("ptx_lottery_status")
    ls = st.get("last_settle") or {}
    if (ls.get("height") or 0) >= 2640:
        break
    time.sleep(15)
print(f"[023] settle observed: h{ls['height']} amount {ls.get('amount')} "
      f"pool_now={st['pool_balance_sat']}")
pre = gm18.call("ptx_lottery_status")["pool_balance_sat"]
print(f"[023] gm18 pool pre-restart: {pre}; restarting gm18 NOW (inside checkblocks window)")
subprocess.run(["docker", "restart", "ptx-w2-gm18"], check=True)
deadline = time.time() + 300
while time.time() < deadline:
    try:
        gm18.getblockcount(); break
    except Exception:
        time.sleep(5)
# let it catch up to the fleet tip so pools are compared at the same height
while gm18.getblockcount() < gm01.getblockcount():
    time.sleep(5)
a = gm18.call("ptx_lottery_status")["pool_balance_sat"]
b = gm01.call("ptx_lottery_status")["pool_balance_sat"]
t18, t01 = gm18.getblockcount(), gm01.getblockcount()
verdict = "PASS" if a == b else "FAIL"
print(f"[023] VERDICT: {verdict} — gm18 pool {a} vs fleet {b} at tips {t18}/{t01} "
      f"(walk crossed payout@2640; BUG-023 clobber would diverge these)")
EOF
