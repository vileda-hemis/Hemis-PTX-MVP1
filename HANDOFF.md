# Hemis PTX — Session Handoff

**Date:** 18 May 2026  
**Branch:** `feature/ptx-phase2-bls`  
**Completed milestone:** P2-BLS-02 (threshold BLS end-to-end verified in Docker cluster)

---

## Commit chain (today)

| Hash | Summary |
|------|---------|
| `e1bcb55` | bls: fix init_priority — use constructor(101) for non-class bool |
| `cefd5e1` | bls: fix static-init-order crash (SIGSEGV at startup) |
| `2bb52dd` | ptx: config status — fix arg name, add caller ptxnode registry |
| `fcb566c` | chore: end-of-session handoff — P2-BLS-01 complete |
| `5ed4dea` | ptx: P2-BLS-01 — replace commit-reveal beacon with threshold BLS12-381 |

---

## P2-BLS-02 — What was verified

### End-to-end `ptx_roll` via threshold BLS12-381

Called via JSON-RPC from `ptx-caller`. Two rounds confirmed:

**Round 1 (1 draw, 1–100):**
```json
{
  "results": [84],
  "round_seed": "36bf5d66...",
  "quorum_sig": "b4391bb5...66" (192 hex chars = 96 bytes),
  "quorum_sig_hash": "c169f7db...",
  "quorum_members": ["gm01".."gm11"]
}
```

**Round 2 (3 draws, 1–52, game "poker"):**
```json
{ "results": [34, 16, 7], "quorum_sig": "ad71012d...", ... }
```

All 11 GMs signed both rounds. Key observations:
- BLS key shares auto-distributed in round 1 via `gm_bls_keyset`
- Round 2 reused existing key shares (keyset_sent set)
- `quorum_sig` is 96 bytes (192 hex) as expected
- `round_seed` and `beacon` are distinct across rounds
- 11/11 GMs participated; threshold = 6 (floor(11/2)+1)

### Bugs fixed during P2-BLS-02

**BUG: SIGSEGV at startup (Static Initialization Order Fiasco)**  
- **Root cause:** `PrivateKey::AllocateKeyData()` calls `Util::SecAlloc` → `secureAllocCallback` (set by `BLS::Init()`). The file-scope `static CBLSSecretKey g_ptx_my_bls_sk` in `rpc/ptx.cpp` constructed before `BLS::Init()` ran, calling through a null function pointer (crash at 0x0).
- **Fix:** Added `__attribute__((constructor(101)))` on a void function in `src/chiabls/src/bls.cpp` to guarantee `BLS::Init()` runs at priority 101 (before all user-code statics at priority 65535). Also added `init_priority(102)` to `pScheme` in `src/bls/bls_wrapper.cpp`.
- **Commits:** `cefd5e1` + `e1bcb55`

**BUG: RPC only binding to 127.0.0.1 (not reachable from other containers)**  
- **Root cause:** Hemis ignores `rpcbind=` in the config file — must be a CLI argument.
- **Fix:** Added `-rpcbind=0.0.0.0` to `entrypoint.sh` exec line.
- **Location:** `/mnt/pve/Node14TB/hemis-ptx/docker/entrypoint.sh` (local file, not in git)

---

## Cluster state

**Location:** `/mnt/pve/Node14TB/hemis-ptx/docker/`  
**Network:** `ptx-net` bridge, 172.28.0.0/24

### Containers (14 total — all Up after P2-BLS-02)

```
NAME             STATUS
ptx-caller       Up
ptx-gm01..gm11  Up (11 nodes)
ptx-grafana      Up
ptx-prometheus   Up
```

### Chain (last known)
- **Height:** 417+ and advancing
- **Staking:** active on gm01 (183K HMS balance)
- **Gamemasters:** 11/11 ENABLED

### Caller node
- **Container:** `ptx-caller` (172.28.0.10)
- **Payout address:** `yL2B4HSKr4yjjs5VWG3TdN5LJ8q3t1eeFY`
- **Balance:** 1,000 HMS
- **Role:** sole `ptx_roll()` caller

### RPC call format (ptx_roll)

The Hemis-cli does NOT parse `false` and `[]` correctly from the shell when combined with string arguments. Use curl with JSON-RPC directly:

```sh
source /mnt/pve/Node14TB/hemis-ptx/docker/.env
docker exec ptx-caller curl -s --user "${RPCUSER}:${RPCPASSWORD}" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"1.0","id":"test","method":"ptx_roll","params":[1,1,100,false,[],"mygame","00aabbcc"]}' \
  http://127.0.0.1:29902/
```

### initgamemaster persistence

After any container restart, re-run:
```sh
while IFS=' ' read -r alias privkey txhash outputidx ip; do
  docker exec "ptx-${alias}" Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet \
    initgamemaster "$privkey" "${ip}:29993"
done < /tmp/gm_data.txt
docker exec ptx-gm01 Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet startgamemaster "all" false "" true
```

Wait for `IsBlockchainSynced: true` before running `startgamemaster`.  
Wait for 11/11 ENABLED before calling `ptx_roll`.

---

## Next tasks

### P2-BLS-03 — On-chain quorum_sig embedding

The `quorum_sig` field is in `CProbabilisticTxPayload` (96 bytes) but `tx_id` in the response is `"pending"` — the probabilistic transaction is not yet broadcast to the mempool.

**Steps:**
1. Implement `ptx_submittx` RPC (or extend `ptx_roll`) to serialize the PTX payload + quorum_sig into a transaction and broadcast it.
2. The transaction should be accepted by gm01's mempool and staked into a block.
3. Verify: `gettransaction <tx_id>` returns the quorum_sig.

### P2-BLS-04 — PoSe scoring live test

Call `ptx_roll` with one or more GMs stopped (or firewall their port), verify:
- Non-signing GMs accumulate positive `pose_score`
- Signing GMs accumulate lottery tickets
- POSE-ineligible GMs are excluded from future quorums

### Infrastructure

| Item | Status | Notes |
|------|--------|-------|
| Docker images | **Current** | Rebuilt with SIGSEGV + rpcbind fixes |
| Grafana password | Pending | `docker exec ptx-grafana grafana-cli admin reset-admin-password <new>` |
| SPORKs | Not activated | Defaults (4070908800) acceptable for Phase 2 |
| `initgamemaster` persistence | Manual | Must re-run after every container restart |
| UPGRADE\_V6\_0 (DGM) | Disabled | Legacy GM system in use |

### Code bugs (carry-over)

| Bug | Status |
|-----|--------|
| BUG-005 | Open |
| BUG-009 | Open |
| BUG-011 | Open |
| T13 | Fix pending |

---

## Rebuild reference

```sh
# entrypoint.sh is a local file in /mnt/pve/Node14TB/hemis-ptx/docker/ (not git-tracked)
# It now has -rpcbind=0.0.0.0 in the exec Hemisd line — DO NOT remove this.

cd /mnt/pve/Node14TB/hemis-ptx/docker
docker compose down
docker compose build       # uses cached layers; only runtime stage rebuilds if only entrypoint changed
docker compose up -d
```

Full rebuild from scratch (if binary needs recompile):
```sh
git push origin feature/ptx-phase2-bls  # push new code first
docker compose build --no-cache
docker compose up -d
```

**Important:** Use `docker compose build`, NOT `docker build -t hemisd-ptx:latest`.
