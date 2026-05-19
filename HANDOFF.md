# End-of-Session Handoff — P2-BLS-00 COMPLETE

**Branch**: `feature/ptx-phase2-bls`  
**Date**: 2026-05-19  
**Session result**: P2-BLS-00 COMPLETE — chiabls → blst migration, both ptx_roll() verified

---

## What was done this session

### P2-BLS-00: Migrate PTX BLS from chiabls to supranational/blst (KDD-032)

**Design ref**: KDD-032 — replace chiabls (relic-toolkit wrapper) with supranational/blst,
a modern, audited BLS12-381 implementation. Wire format (96-byte G2 sig) is unchanged.

**Source changes** (all committed in `cd6bf1b`):
- `src/blst/` — vendored blst (bindings/ + src/), pure-C build via `__BLST_PORTABLE__` + `__BLST_NO_ASM__`
- `src/ptx/ptx_bls.h` — blst API types, `PTXBLSState` with `blst_scalar`/`blst_p1_affine`, `extern PTX_BLS_DST`
- `src/ptx/ptx_bls.cpp` — trusted-dealer DKG, `PTX_BLS_PartialSign`, `PTX_BLS_Recover` (Lagrange in G2), `PTX_BLS_Verify` (pairing), `PTX_BLS_SigToBeacon`
- `src/Makefile.am` — `LIBBLST=libblst.a`, `libblst_a_SOURCES=blst/src/server.c`, `-D__BLST_PORTABLE__ -D__BLST_NO_ASM__`, added to `EXTRA_LIBRARIES` and `Hemisd_LDADD`
- `src/rpc/ptx.cpp` — HexStr Span API fix for `sig_buf`
- `src/ptx/ptx_fanout.cpp` — HexStr Span API fix for `sk_bytes`

**Build fixes required**:
- `blst_fr_set_to_one` does not exist in blst — replaced with `blst_fr_from_uint64(&x, (uint64_t[]){1,0,0,0})`
- `blst_p2_mult` takes `blst_p2*` (Jacobian), not `blst_p2_affine*` — added `blst_p2_from_affine` conversions
- `HexStr` API changed to `Span<const uint8_t>` — fixed in 2 sites
- `static const char* PTX_BLS_DST` in header caused unused-variable warnings — moved definition to ptx_bls.cpp, `extern` decl in header
- blst's `no_asm.h` is 32-bit-only (`llimb_t` undefined for 64-bit) — added `typedef unsigned __int128 llimb_t` for `LIMB_T_BITS==64`
- `-D__BLST_NO_ASM__` is required (not just `__BLST_PORTABLE__`) to activate `no_asm.h` inclusion in `vect.c`

**Two verified ptx_roll() calls**:

Call #1 — `ptx_roll(1, 1, 100, false, [], "game", "deadbeef01020304")`:
```
quorum_sig: 845a94b8522766fca5e7251bd645a20071d6fe5c4964ccd5f8eb2d5cc768e7c24518248ee2f5bc6795c76d6a626652210b21be2183fe272a67c1d8b8edb97359eeb71540446d49c37f44f8c954745d6f7baa5d75ceba68028881a834f5525f96
results:    [35]
tx_id:      7624e1afdcfadd7db146ac27832487a196d4f42fe36228033f4e4b86eab7e380
```

Call #2 — `ptx_roll(3, 1, 52, false, [], "poker", "cafebabe01020304")`:
```
quorum_sig: a61e9d2cc3c7bab90e1f059b4d9d9ef23e60b06e2d3cb9cec2db6b7ed4f1d0464e52947e3bd3db9e4a837e54f08d163013e7b730bf8280967ee3fb1d3eef4e16a7f7c209b822b6a0331e3e1b750588cbdb6bfbf05c064e006699a4cf6a673a45
results:    [39, 45, 4]
tx_id:      3ff7f275c957420d85e5b859486cb1c0ca9f1cc60762457bbe7efd94f57d3761
```

Both `quorum_sig` fields are exactly 192 hex chars (96 bytes compressed G2). ✓

**Test suite** (ptx_test_suite_v4, --skip-fail-modes --skip-stress --skip-advanced --skip-excl --skip-excl-probe):
```
PASS: 59   FAIL: 3   SKIP: 4   TOTAL: 66
```
Failures are pre-existing (T13=node naming mismatch; T26/T27=same; all in carry-over list).
All crypto tests T11-T20 pass. Pass profile unchanged from pre-migration.

---

## Cluster state

- Docker cluster: **UP** (all 14 containers)
- All 11 GMs: **ENABLED**
- Block height: ~1256 at close of session
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

RPC credentials: `ptxrpc:ptxpass2026` on port 29902 (inside container at 127.0.0.1:29902).

---

## Completed milestones (P2-BLS series)

| Milestone | Commit | Description |
|-----------|--------|-------------|
| P2-BLS-00 | `cd6bf1b` | chiabls → blst migration (KDD-032) |
| P2-BLS-01 | `5ed4dea` | Threshold BLS12-381 beacon |
| P2-BLS-02 | `09024d6` | End-to-end test PASSED |
| P2-BLS-03 | `2c6fe54` | PTXSESS wallet-funded tx |
| P2-BLS-04 | `75ea59c`+`2a204fd` | GM lottery distribution |

---

## Next milestone: P2-BLS-05

**PoSe scoring live test** — stop one or more GMs mid-round, verify `pose_score` accumulates on the absent nodes and they eventually get banned from the quorum.

Steps:
1. Run `ptx_roll` to start a round with 11/11 GMs
2. `docker stop ptx-gm05` (or any GM) mid-fanout
3. Confirm quorum still resolves with 10/11 sigs (above threshold)
4. Check `ptx_pose_scores` or equivalent RPC to see score increment on gm05
5. Repeat until ban threshold crossed; verify gm05 excluded from next fanout

Carry-over open items: BUG-005, BUG-009, BUG-011, T13.
