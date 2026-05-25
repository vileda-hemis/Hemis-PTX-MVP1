# Session Context v6 — PTX Phase 2 Handoff

**Branch**: `feature/ptx-phase2-bls`  
**Date**: 2026-05-25  
**Session result**: KDD-034 (PTXCONSOLIDATE) shipped and verified on 11-node testnet. PTXSETTLE confirmed twice in consecutive windows. Pool UTXO count stabilised at 70-85. ConnectBlock view-state ordering bug fixed (BUG-012 closed).

---

## Section 2 — Current commit state

| Commit | Description |
|--------|-------------|
| `737214d` | **KDD-034**: PTXCONSOLIDATE (nType=8) + ConnectBlock pool-input pre-check ← HEAD |
| `f45bbcf` | KDD-032 PTXSETTLE (nType=7) + ODC-020 GM payment address |
| `29d9af7` | Previous baseline |

**Remote**: `https://github.com/vileda-hemis/Hemis-PTX-MVP1` branch `feature/ptx-phase2-bls`  
**Binary**: `/mnt/pve/Node14TB/hemis-ptx/docker/Hemisd` (built 2026-05-25 20:40 UTC from `737214d`)  
**Compile container**: `ptx-compile`; build with `docker exec ptx-compile bash -c "cd /build/hemisd && make -j\$(nproc)"`; binary at `/build/hemisd/src/Hemisd`.

---

## Section 3 — Testnet verification log

| Block | Event | Result | Notes |
|-------|-------|--------|-------|
| h=9874 | PTXCONSOLIDATE first attempt | STALL | ConnectBlock `ptxconsolidate-missing-input` — view-state ordering bug; fixed and redeployed |
| h=9875 | PTXCONSOLIDATE round 1 confirmed | PASS | 500 inputs merged; 1546 → 1047 UTXOs |
| h=9875 | PTXSETTLE settlement window | DEFER | `txn-mempool-conflict` with consolidate (correct coexistence behaviour) |
| h=9876 | PTXCONSOLIDATE round 2 confirmed | PASS | 500 inputs merged; 1047 → 548 UTXOs |
| h=9877 | PTXCONSOLIDATE round 3 confirmed | PASS | 548 → 70 UTXOs (below 150 threshold) |
| h=9880 | PTXSETTLE settlement window | SKIP | no eligible nodes (tickets reset on restart) |
| h=9885 | PTXSETTLE settlement window | **PASS** | gm08 won; **1,581.98 HMS** (85 inputs, fee 1281 sat); txid `df7115e8…` |
| h=9890 | PTXSETTLE settlement window | **PASS** | gm02 won; **12.00 HMS** (12 inputs); txid `2126d78d…` |

**Pool UTXO oscillation (steady state)**: drains to ~0 at settlement → refills from ptx_roll contributions → consolidates when count ≥ 150 → settles at next window. Observed 1546 → 70 → 85 during this session.

---

## Section 4 — Open bugs

| ID | Severity | Status | Description |
|----|----------|--------|-------------|
| BUG-005 | Low | OPEN | Round status P2P not implemented; T17/T74 fail in test suite v4 |
| BUG-009 | Medium | OPEN | peers.dat not persisted on ptx-caller; must `addnode` all 11 GMs after every restart |
| BUG-011 | Low | OPEN | banlist.dat persists across container rebuilds; must `setban … remove` after each restart |
| BUG-012 | High | **CLOSED** | PTXSETTLE cap exceeded when pool_utxo_count > 200 — closed by KDD-034 (PTXCONSOLIDATE) |
| BUG-013 | Medium | **PARTIAL** | PTXCONSOLIDATE / PTXSETTLE mempool race — natural defer (txn-mempool-conflict → AdvanceLotteryWindow) works correctly; explicit auto-evict of PTXCONSOLIDATE when PTXSETTLE fires is not implemented but not required for correctness |
| BUG-014 | Low | OPEN | `lottery.html` winner card: address text overflows card boundary on long bech32 addresses; CSS `word-break: break-all` missing on `.winner-address` |

---

## Section 5 — KDD feature status

| KDD | Title | Status | Notes |
|-----|-------|--------|-------|
| KDD-027 | PTX auto-commit | LIVE | |
| KDD-028 | Commit-reveal quorum | LIVE | |
| KDD-029 | BLS12-381 threshold sig | LIVE | |
| KDD-030 | Beacon from last PTX hash | LIVE | Deterministic sort by node_id |
| KDD-031 | Caller address in payload | LIVE | |
| KDD-032 | PTXSETTLE (nType=7) | LIVE | Confirmed h=9885 and h=9890 |
| KDD-033 | ODC-020 GM payment address | LIVE | scriptPTXPayment field; falls back to getnewaddress |
| **KDD-034** | **PTXCONSOLIDATE (nType=8)** | **LIVE** | Threshold 150 (spec said 50 — 150 chosen for testnet cadence; functionally equivalent). No "last 2 blocks" pause condition (spec draft); CONSOLIDATE defers naturally via txn-mempool-conflict. CONSOLIDATE wins mempool race over SETTLE at same height — settle deferred by one window (correct). |
| **KDD-035** | **ConnectBlock pre-UpdateCoins pool-input rule** | **LIVE** | New consensus rule `ptx-non-pool-input` added to ConnectBlock TX loop, before `UpdateCoins`. Applies to both PTXSETTLE and PTXCONSOLIDATE. CheckSpecialTx now skips `IsSpent()` inputs and guards `vout-vs-sum_in` on `any_unspent`. This is consensus rule 10 of the PTXSETTLE/PTXCONSOLIDATE rule set, beyond the original nine in KDD-032. |
| KDD-036 | PoSe ban threshold | NOT IMPL | Repeat stop + roll until GM pose_score crosses ban; verify exclusion from quorum |

---

## Section 6 — Cluster state

- **Docker cluster**: UP (all 14 containers: 12 Hemisd nodes, 1 explorer, 1 grafana/prometheus, 1 cloudflared)
- **Block height**: ~9893 at session close
- **Pool balance**: ~12–1582 HMS oscillating (drains to near-zero on each settlement)
- **Pool UTXO count**: ~12–85 (consolidation fires above 150; resets after settlement)
- **GMs**: all 11 enabled, pose_score=0, quorum_eligible=true

### Post-restart checklist

```bash
# 1. Start cluster
cd /mnt/pve/Node14TB/hemis-ptx/docker
docker compose up -d

# 2. Wait for RPC
until Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet -rpcport=29902 getblockcount 2>/dev/null; do sleep 2; done

# 3. Re-add peers (ptx-caller doesn't persist peers.dat)
CLI="Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet -rpcport=29902"
for ip in $(seq 11 21); do
  docker exec ptx-caller $CLI addnode "172.28.0.$ip" "add" 2>/dev/null
done

# 4. Clear any stale bans (if GMs banned caller from previous session)
for i in $(seq 1 11); do
  [ $i -lt 10 ] && c="ptx-gm0${i}" || c="ptx-gm${i}"
  docker exec $c $CLI setban "172.28.0.10" "remove" 2>/dev/null
done

# 5. Do ≥10 ptx_rolls to accumulate lottery tickets before next settlement window
for i in $(seq 1 10); do
  curl -s -u ptxrpc:ptxpass2026 \
    --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"x\",\"method\":\"ptx_roll\",\"params\":[1,1,100,false,[],\"warmup-$i\",\"$(openssl rand -hex 8)\"]}" \
    -H 'content-type:text/plain;' http://172.28.0.10:29902/ > /dev/null
done
```

**RPC credentials**: `ptxrpc:ptxpass2026` on port 29902; caller at `172.28.0.10`.

---

## Section 7 — Roadmap

### Phase 2 — RECENTLY DONE

- KDD-029: BLS12-381 threshold beacon (blst migration, Lagrange fix)
- KDD-030: deterministic winner selection from beacon
- KDD-031: caller address in PTX payload
- KDD-032: PTXSETTLE (nType=7) — consensus-enforced pool disbursement
- KDD-033 / ODC-020: GM registered PTX payment address (scriptPTXPayment)
- KDD-034: PTXCONSOLIDATE (nType=8) — pool UTXO consolidation; threshold 150, cap 500; coexistence rules in ProcessSpecialTxsInBlock; scriptSig exempt; mempool reject rule ← **this session**
- KDD-035: ConnectBlock pre-UpdateCoins pool-input rule — fixes view-state ordering so PTXSETTLE and PTXCONSOLIDATE pass CheckSpecialTx in block validation context ← **this session**

### Phase 3 — CRITICAL (next)

- **KDD-036**: PoSe ban threshold — stop a GM repeatedly; verify pose_score crosses ban and GM is excluded from quorum selection; verify score decays on recovery
- **BUG-009 fix**: persist peers.dat or seed from a static addnode list in hemis.conf so ptx-caller reconnects automatically after restart
- **BUG-011 fix**: wipe banlist.dat on container rebuild, or sign peer connections so bans are keyed by pubkey rather than IP
- **BUG-014 fix**: `lottery.html` CSS `word-break: break-all` on `.winner-address`
- **Stress test**: 200 rolls across two settlement windows with full 11-GM quorum; verify UTXO count stays < 150 and both settlements confirm

### Phase 4 — PLANNED

- scriptPTXPayment enforcement: reject getnewaddress fallback; require all GMs to register on-chain payment address before settlement
- Multi-window settlement: if pool balance too low for fee, accumulate across windows
- Explorer integration: settlement_history table on lottery.html pulled from ptx_lottery_status

---

## Section 8 — Key file index (KDD-034 changes)

| File | Change |
|------|--------|
| `src/primitives/transaction.h` | `PTXCONSOLIDATE = 8` in TxType enum; `IsPTXConsolidateTx()` predicate |
| `src/ptx/ptx_lottery.h` | `CPTXConsolidatePayload` struct; `PTX_ConsolidateLotteryPool()` declaration |
| `src/ptx/ptx_lottery.cpp` | `PTX_ConsolidateLotteryPool()` implementation (threshold 150, cap 500, PTXSETTLE-in-mempool guard) |
| `src/ptx/ptx_validation_interface.h` | Call `PTX_ConsolidateLotteryPool()` on every `BlockConnected`; settlement still gated on window boundary |
| `src/evo/specialtx_validation.cpp` | V6_0 gate exemption for PTXCONSOLIDATE; Rules C1-C5 in `CheckSpecialTx`; duplicate + coexist guards in `ProcessSpecialTxsInBlock`; PTXSETTLE Rule 1/3 `IsSpent()` → skip pattern |
| `src/validation.cpp` | Pre-UpdateCoins pool-input check in ConnectBlock TX loop; PTXCONSOLIDATE scriptSig exemption; PTXCONSOLIDATE mempool reject rule |
| `src/rpc/ptx.cpp` | `pool_utxo_count` field in `ptx_lottery_status` response |
