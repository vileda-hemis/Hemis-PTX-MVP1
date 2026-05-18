# Hemis PTX — Session Handoff

**Date:** 18 May 2026
**Branch:** `feature/ptx-phase2-bls`
**Completed milestone:** P2-INF-01 (Phase 2 infrastructure — Docker cluster + GM registration)

---

## Commit chain (today)

| Hash | Summary |
|------|---------|
| `939703e` | ptxtestnet: activate UPGRADE\_V4\_0 at height 265, not ALWAYS\_ACTIVE |
| `5e3849b` | ptxtestnet: nTimeSlotLength=1s for rapid PoS block generation |
| `1259f8b` | kernel: use GetAdjustedTime() when IsTimeProtocolV2 not yet active |
| `2f22c52` | kernel: monotonic nTimeTx during free-time bootstrap phase |
| `7a0e31a` | gamemaster: allow RFC1918 addresses on ptxtestnet |

---

## Cluster state

**Location:** `/mnt/pve/Node14TB/hemis-ptx/docker/`
**Network:** `ptx-net` bridge, 172.28.0.0/24

### Containers (14 total — all Up)

```
NAME             STATUS          PORTS
ptx-caller       Up              0.0.0.0:29902->29902/tcp, 0.0.0.0:29910->29910/tcp, 29993/tcp
ptx-gm01         Up              29902/tcp, 29993/tcp
ptx-gm02         Up              29902/tcp, 29993/tcp
ptx-gm03         Up              29902/tcp, 29993/tcp
ptx-gm04         Up              29902/tcp, 29993/tcp
ptx-gm05         Up              29902/tcp, 29993/tcp
ptx-gm06         Up              29902/tcp, 29993/tcp
ptx-gm07         Up              29902/tcp, 29993/tcp
ptx-gm08         Up              29902/tcp, 29993/tcp
ptx-gm09         Up              29902/tcp, 29993/tcp
ptx-gm10         Up              29902/tcp, 29993/tcp
ptx-gm11         Up              29902/tcp, 29993/tcp
ptx-grafana      Up              0.0.0.0:3000->3000/tcp
ptx-prometheus   Up              9090/tcp
```

### Chain

- **Height:** 326 and advancing (background staker on gm01)
- **Staking:** active — `staking_status: true`, ~174K HMS staking balance
- **Gamemasters:** 11/11 ENABLED (`stable: 11, enabled: 11, inqueue: 11`)
- **GM collateral:** 100 HMS × 11 = 1,100 HMS (ptxtestnet value; mainnet target is 1,000 HMS per KDD-022)
- **UPGRADE\_V4\_0:** activates at height 265 (time-protocol V2 + message signing)

### Caller node

- **Container:** `ptx-caller` (172.28.0.10)
- **Payout address:** `yL2B4HSKr4yjjs5VWG3TdN5LJ8q3t1eeFY`
- **Balance:** 1,000 HMS (funded from gm01 on 18 May 2026)
- **Role:** sole `ptx_roll()` caller — no `-ptxmynodeid` set

### GM private keys (gamemaster.conf — stored in ptx-gm01 container)

Path inside container: `/root/.hemis-ptxtestnet/ptxtestnet/gamemaster.conf`

Keys survive container restarts (Docker volume `ptx-data-gm01`), but
**`initgamemaster <privkey> <ip:29993>` must be re-run on each GM node
after any container restart** — the active-GM state is not persisted across
daemon restarts. Re-run the loop:

```sh
# /tmp/gm_data.txt on the host (regenerate if lost — see session transcript)
while IFS=' ' read -r alias privkey txhash outputidx ip; do
  docker exec "ptx-${alias}" Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet \
    initgamemaster "$privkey" "${ip}:29993"
done < /tmp/gm_data.txt
# Then: docker exec ptx-gm01 Hemis-cli ... startgamemaster "all" false "" true
```

---

## Open items

### Infrastructure / ops

| Item | Status | Notes |
|------|--------|-------|
| Grafana password | **Pending** | Default credentials not working (`ptxgraf2026`). Reset via `docker exec ptx-grafana grafana-cli admin reset-admin-password <new>` |
| SPORKs | **Not activated** | No sporkkey loaded. `strSporkPubKey` in chainparams is the testnet5 key — matching private key not available. Current SPORK defaults (4070908800 = not enforced) are acceptable for Phase 2 testing. |
| `initgamemaster` persistence | **Manual** | Must be re-run after every daemon/container restart (see above). |
| UPGRADE\_V6\_0 (DGM) | Disabled (`NO_ACTIVATION_HEIGHT`) | `getblockchaininfo` falsely reports `status: active` at `activationheight: 0` — display bug only; `protx_register` correctly rejects with "Evo upgrade is not active yet". Legacy GM system is in use. |

### Code bugs

| Bug | Description |
|-----|-------------|
| BUG-005 | (carry-over from Phase 1) |
| BUG-006/007/008 | Input validation fixes landed in `da55bb5`; test-suite salt/assumption corrections applied |
| BUG-009 | Open |
| BUG-011 | Open |
| T13 | Fix pending |

---

## Next task: P2-BLS-01

**Objective:** Integrate `supranational/blst` (BLS12-381), replace the current
hash-based commit-reveal round state machine with threshold BLS signatures.

**Entry point:** PTX round state machine is in `src/ptx/` (Phase 1 modules).
The `ptx_roll()` RPC stub on the caller node is the integration surface.

**Key constraints:**
- blst must build inside the existing Autotools + Rust (librustzcash) build graph
- Threshold scheme: t-of-n where n=11 (all registered GMs), threshold TBD by KDD
- Commit phase stays on-chain; reveal phase may move to BLS aggregate signature

---

## Rebuild reference

If a full cluster rebuild is needed:

```sh
cd /mnt/pve/Node14TB/hemis-ptx/docker
docker compose down
docker volume rm $(docker volume ls -q | grep ptx)   # only if wipe intended
docker compose build --no-cache
docker compose up -d
```

**Important:** `docker compose build` builds per-service images (`docker-gm01`,
`docker-caller`, etc.). Do NOT use `docker build -t hemisd-ptx:latest` —
that image is not used by compose.

The Dockerfile clones `feature/ptx-phase2-bls` from GitHub at build time.
Push all changes before rebuilding.
