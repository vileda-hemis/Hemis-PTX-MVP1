# Hemis PTX — Session Handoff

**Date:** 18 May 2026  
**Branch:** `feature/ptx-phase2-bls`  
**Completed milestone:** P2-BLS-01 (threshold BLS12-381 beacon)

---

## Commit chain (today)

| Hash | Summary |
|------|---------|
| `5ed4dea` | ptx: P2-BLS-01 — replace commit-reveal beacon with threshold BLS12-381 |
| `e9ce9de` | chore: end-of-session handoff — P2-INF-01 complete, 11/11 GMs ENABLED |
| `7a0e31a` | gamemaster: allow RFC1918 addresses on ptxtestnet |
| `2f22c52` | kernel: monotonic nTimeTx during free-time bootstrap phase |
| `1259f8b` | kernel: use GetAdjustedTime() when IsTimeProtocolV2 not yet active |
| `5e3849b` | ptxtestnet: nTimeSlotLength=1s for rapid PoS block generation |

---

## P2-BLS-01 — What was implemented

### Library
**chiabls** (BLS12-381, vendored at `src/bls/`) — already in the repo, fully
built. The HANDOFF mentioned "supranational/blst" but chiabls covers all needed
operations including threshold signatures.

### New files
- `src/ptx/ptx_bls.h` / `ptx_bls.cpp` — `PTXBLSState` (coordinator BLS
  state: master polynomial, verification vector, per-GM key shares),
  `PTX_BLS_Init`, `PTX_BLS_GetMasterPubKey`, `PTX_BLS_NodeId`,
  `PTX_BLS_Recover`, `PTX_BLS_SigToBeacon`

### Modified files
- `src/ptx/ptx_commit_reveal.h` — added `bls_partial_sigs` and `threshold_sig`
  to `PTXCommitRevealRound`
- `src/ptx/ptx_fanout.h` / `ptx_fanout.cpp` — added `PTX_FanOutKeySet` and
  `PTX_FanOutSign`
- `src/rpc/ptx.cpp` — added `gm_bls_keyset` / `gm_bls_sign` RPCs; rewrote
  `ptx_roll` to use BLS signing instead of hash commit-reveal
- `src/primitives/transaction.h` — added `quorum_sig` (96-byte raw threshold
  sig) to `CProbabilisticTxPayload`; serialization updated
- `src/Makefile.am`, `Makefile.in`, `Makefile` — ptx_bls.cpp added; dep stub
  `src/ptx/.deps/libbitcoin_server_a-ptx_bls.Po` created

### Protocol

**Coordinator (caller node) at `ptx_roll` time:**
1. Lazy-init BLS: generate master polynomial (degree `t-1`), compute per-GM
   key shares via `CBLSSecretKey::SecretKeyShare`.
2. `PTX_FanOutKeySet`: send each GM its 32-byte key share via `gm_bls_keyset`.
3. `PTX_FanOutSign`: ask each GM to sign `round_seed` via `gm_bls_sign`;
   collect 96-byte partial BLS signatures.
4. `CBLSSignature::Recover` (Lagrange interpolation) → threshold signature.
5. Verify: `threshold_sig.VerifyInsecure(master_pubkey, round_seed)`.
6. Beacon = `SHA256(96-byte threshold sig)`.
7. `quorum_sig` = raw 96-byte sig stored in payload;
   `quorum_sig_hash` = `SHA256(quorum_sig)`.

**GM side (`gm_bls_keyset`):**  
Parse 32-byte key share; store in `g_ptx_my_bls_sk` (in-memory, session only).

**GM side (`gm_bls_sign`):**  
Sign `round_seed` with stored key share; return `{"sig_hex": "96-byte hex"}`.

### Threshold
`floor(n/2)+1` (simple majority). For n=11: t=6. KDD-TBD for final value.

### Build note
`CBLSSecretKey` does NOT inherit `CBLSWrapper`'s vector constructor (unlike
`CBLSPublicKey`/`CBLSSignature` which have `using CBLSWrapper::CBLSWrapper`).
Use `sk.SetByteVector(bytes)` to initialise from a byte vector.

---

## Cluster state

**Location:** `/mnt/pve/Node14TB/hemis-ptx/docker/`  
**Network:** `ptx-net` bridge, 172.28.0.0/24

### Containers (14 total — last known Up as of P2-INF-01 session)

```
NAME             STATUS          PORTS
ptx-caller       Up              0.0.0.0:29902->29902/tcp, 0.0.0.0:29910->29910/tcp, 29993/tcp
ptx-gm01 … gm11 Up              29902/tcp, 29993/tcp
ptx-grafana      Up              0.0.0.0:3000->3000/tcp
ptx-prometheus   Up              9090/tcp
```

The Docker images were built from `feature/ptx-phase2-bls` **before P2-BLS-01**.
They must be rebuilt to include the new BLS RPCs. See Rebuild reference below.

### Chain (last known)

- **Height:** 326+ and advancing
- **Staking:** active on gm01
- **Gamemasters:** 11/11 ENABLED

### Caller node
- **Container:** `ptx-caller` (172.28.0.10)
- **Payout address:** `yL2B4HSKr4yjjs5VWG3TdN5LJ8q3t1eeFY`
- **Balance:** 1,000 HMS
- **Role:** sole `ptx_roll()` caller

### GM private keys (gamemaster.conf — stored in ptx-gm01 container)

Path inside container: `/root/.hemis-ptxtestnet/ptxtestnet/gamemaster.conf`

**`initgamemaster <privkey> <ip:29993>` must be re-run on each GM node
after any container restart.** Re-run the loop:

```sh
# /tmp/gm_data.txt on the host (regenerate if lost — see session transcript)
while IFS=' ' read -r alias privkey txhash outputidx ip; do
  docker exec "ptx-${alias}" Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet \
    initgamemaster "$privkey" "${ip}:29993"
done < /tmp/gm_data.txt
# Then: docker exec ptx-gm01 Hemis-cli ... startgamemaster "all" false "" true
```

After rebuild + restart, `gm_bls_keyset` is re-sent automatically by the
coordinator on the first `ptx_roll` call.

---

## Next task: P2-BLS-02

**Objective:** End-to-end test of `ptx_roll` using the rebuilt Docker cluster.

**Steps:**
1. Push `feature/ptx-phase2-bls` to GitHub.
2. Rebuild Docker images: `docker compose build --no-cache && docker compose up -d`
3. Re-run `initgamemaster` loop; verify `startgamemaster all` → 11/11 ENABLED.
4. Call `ptx_roll` from `ptx-caller`:
   ```sh
   docker exec ptx-caller Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet \
     -rpcport=29910 ptx_roll 1 1 100 false '[]' mygame 00aabbcc
   ```
5. Verify in response: `quorum_sig` is 192-char hex (96 bytes), `quorum_sig_hash` is
   non-null, `beacon` is non-null, `results` has 1 value in [1,100].
6. Check `ptx_getroundstatus` → round state = RESOLVED (3), threshold_sig populated.

**Watch for:**
- GMs failing to sign (check gm logs for "BLS key not set" — means keyset failed)
- Threshold not met (< 6 of 11 responding)
- Signature verification failure (would surface as JSON-RPC error in ptx_roll)

---

## Open items

### Infrastructure / ops

| Item | Status | Notes |
|------|--------|-------|
| Docker images | **Stale** | Must rebuild to include P2-BLS-01 code |
| Grafana password | **Pending** | `docker exec ptx-grafana grafana-cli admin reset-admin-password <new>` |
| SPORKs | **Not activated** | Defaults (4070908800) acceptable for Phase 2 |
| `initgamemaster` persistence | **Manual** | Must re-run after every daemon/container restart |
| UPGRADE\_V6\_0 (DGM) | Disabled | Display bug only; legacy GM system in use |

### Code bugs

| Bug | Description |
|-----|-------------|
| BUG-005 | (carry-over from Phase 1) |
| BUG-006/007/008 | Fixed in `da55bb5` |
| BUG-009 | Open |
| BUG-011 | Open |
| T13 | Fix pending |

---

## Rebuild reference

```sh
# Push first so Dockerfile can clone the updated branch
git push origin feature/ptx-phase2-bls

cd /mnt/pve/Node14TB/hemis-ptx/docker
docker compose down
# Only wipe volumes if a full chain reset is needed:
# docker volume rm $(docker volume ls -q | grep ptx)
docker compose build --no-cache
docker compose up -d
```

**Important:** Use `docker compose build` (per-service images), NOT
`docker build -t hemisd-ptx:latest`.
