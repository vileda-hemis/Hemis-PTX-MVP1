# End-of-Session Handoff — P2-BLS-00 + P2-POSE-01 COMPLETE

**Branch**: `feature/ptx-phase2-bls`  
**Date**: 2026-05-19  
**Session result**: Lagrange scalar endianness bug fixed and verified; P2-POSE-01 PoSe scoring confirmed.

---

## What was done this session

### Lagrange Montgomery fix (KDD-032, commit 37dc4ec)

Two interacting bugs in `PTX_BLS_Recover` caused `core_verify err=5` on every roll:

**Bug 1 — wrong byte position in `blst_scalar` init**  
`blst_scalar` (`pow256`) is little-endian: `b[0]` is the LSB. The WIP commit used `b[31]`
(the MSB) for `xi_s`, `xj_s`, and `one_s`, giving values `indices[i] * 2^248` instead of
`indices[i]`. The Lagrange ratios cancelled the `2^248` in numerator/denominator, but the
accumulator started at `2^248` (from `one_s.b[31]=1`) instead of `1`. Every lambda ended up
scaled by `2^248`, so the recovered signature was `2^248 * f(0) * H` ≠ `f(0) * H`.

**Bug 2 — wrong endianness for `blst_p2_mult`**  
`blst_p2_mult` copies `scalar[i]` directly into an internal `pow256` (LE), so it expects
little-endian bytes. The code used `blst_bendian_from_scalar` which outputs big-endian
(proven from `exports.c`: `limbs_from_le_bytes` + `be_bytes_from_limbs`). Changed to
`blst_lendian_from_scalar` (identity memcpy of the canonical LE representation).

**Fixes**: `b[31]` → `b[0]` for all three scalars; `blst_bendian_from_scalar` → `blst_lendian_from_scalar`.

**Build path**: source edited on host → `docker cp` to `ptx-compile` → `make` inside ptx-compile (Boost 1.74) → `docker cp` binary to `docker/` → `docker compose build` → `docker compose up -d`.

### P2-POSE-01: PoSe scoring live test — PASS

- Stopped `ptx-gm05`, ran `ptx_roll` → 192-char quorum sig from 10/11 (threshold=6).
- `ptx_pose_status`: gm05 `pose_score=5`, `tickets=0`; all others `score=0`, `tickets=7`. ✓
- `ptx-gm05` restarted and re-inited.

---

## Verification results

```
Roll 1: len=192 PASS  members=gm01..gm11
Roll 2: len=192 PASS  members=gm01..gm11
Roll 3: len=192 PASS  members=gm01..gm11
Roll 4: len=192 PASS  members=gm01..gm11
Roll 5: len=192 PASS  members=gm01..gm11
```

---

## Cluster state

- Docker cluster: **UP** (all 14 containers)
- All 11 GMs: **ENABLED** (gm05 pose_score=5 from test stop, still eligible)
- Block height: ~1600 at close of session
- `initgamemaster` syntax: `initgamemaster <privkey> <ip:port>` (no txhash/outputidx)
- GM init data: `/tmp/gm_data.txt`

### To restore after restart
```bash
cd /mnt/pve/Node14TB/hemis-ptx/docker
docker compose up -d
sleep 20

CLI="Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet -rpcport=29902"
while IFS=' ' read -r gmid privkey txhash outputidx ip; do
    docker exec "ptx-${gmid}" $CLI initgamemaster "$privkey" "${ip}:29993"
done < /tmp/gm_data.txt
```

Note: after restart run `ptx_roll` once to refund the lottery pool before the next settlement window.

RPC credentials: `ptxrpc:ptxpass2026` on port 29902 (172.28.0.10 from host).

---

## Completed milestones

| Milestone | Commit | Description |
|-----------|--------|-------------|
| P2-BLS-00 | `cd6bf1b` + `37dc4ec` | chiabls → blst migration + Lagrange fix (KDD-032) |
| P2-BLS-01 | `5ed4dea` | Threshold BLS12-381 beacon |
| P2-BLS-02 | `09024d6` | End-to-end test PASSED |
| P2-BLS-03 | `2c6fe54` | PTXSESS wallet-funded tx |
| P2-BLS-04 | `75ea59c`+`2a204fd` | GM lottery distribution |
| P2-POSE-01 | (verified 2026-05-19) | PoSe scoring live: score accumulates, tickets zeroed |

---

## Next milestone: P2-BLS-05 — PoSe ban threshold

Repeat `docker stop ptx-gmXX` + `ptx_roll` until the target GM's `pose_score` crosses the
ban threshold and it is excluded from the next fanout quorum selection. Verify that:
1. The banned GM no longer appears in `quorum_members`.
2. The roll still resolves with the remaining ≥ threshold GMs.
3. Score decays on subsequent rounds where the GM participates.

Carry-over open items: BUG-005, BUG-009, BUG-011, T13.
</content>
</invoke>
## Extended verification (2026-05-19 final)

**50-roll extended test**: 50/50 PASS, 0 container restarts

**Test suite v4** (Phase 1 suite, skip-stress/advanced/excl):
- PASS: 61  FAIL: 3  SKIP: 12  TOTAL: 76
- T13: Phase 1 expects 5 GMs, Phase 2 has 11 — expected
- T17: BUG-005 carry-over (round status P2P not implemented)
- T74: BUG-005 carry-over (PoSe withhold path changed in Phase 2)
- No new failures vs pre-migration baseline
