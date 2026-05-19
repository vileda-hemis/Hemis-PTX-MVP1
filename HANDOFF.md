# End-of-Session Handoff — P2-BLS-04 PASSED

**Branch**: `feature/ptx-phase2-bls`  
**Date**: 2026-05-19  
**Session result**: P2-BLS-04 COMPLETE — GM lottery settlement fired at block 575, tx confirmed

---

## What was done this session

### P2-BLS-04: GM lottery reward distribution

**Design ref**: KDD-030 — at each settlement window boundary the coordinator selects a winning GM, fetches a payout address via `getnewaddress`, and broadcasts a distribution tx paying the full accumulated pool balance.

**Implementation**:
- `src/ptx/ptx_lottery.cpp/h` — new module: `PTX_AddToPoolBalance`, `PTX_GetPoolBalance`, `PTX_SelectLotteryWinner` (sort by node_id, beacon-derived index), `PTX_SettleLotteryWindow` (fetch addr, FundTransaction, sign, AcceptToMemoryPool)
- `src/chainparams.cpp/h` — added `PTXSettlementWindow()` (`nPTXSettlementWindow=5` for testnet, `1440` for mainnet)
- `src/ptx/ptx_mempool.cpp` — call `PTX_AddToPoolBalance` after each successful relay
- `src/ptx/ptx_validation_interface.h` — trigger `PTX_SettleLotteryWindow` at `Params().PTXSettlementWindow()` boundary
- `src/rpc/ptx.cpp` — added `ptx_lottery_status` RPC

**Deadlock fix** (`2a204fd`): `TryATMP` posts to the CValidationInterface scheduler queue and waits on a promise — deadlocks when called from `BlockConnected` (same scheduler thread). Fixed by calling `AcceptToMemoryPool` directly in `ptx_lottery.cpp`.

### Commits this session
- `75ea59c` — ptx: GM lottery distribution at configurable settlement window (P2-BLS-04)
- `2a204fd` — ptx: fix lottery settle deadlock on scheduler thread

### Test result

```
ptx_roll called → pool funded (+100000000 sat)
Block 575 → lottery window triggered
  winner: gm05 (idx=4/11 candidates)
  addr:   yDcr8ZtCiB8xY4yw2v9U5dc1fnUkanSnCj
  payout: 0.9999698 HMS (100000000 sat minus miner fee)
  txid:   547c0954a0b7d5f1046deac79ec95c6b099549203e3e75231e9705f451de0674
  confirmations at check: 4
```

Log excerpt:
```
PTX: lottery pool +100000000 sat → 100000000 sat
PTX: lottery winner selected: gm05 (idx=4/11 candidates)
PTX: lottery window h=575: distributing 100000000 sat to gm05 (yDcr8ZtCiB8xY4yw2v9U5dc1fnUkanSnCj)
PTX: lottery settled h=575 winner=gm05 addr=yDcr8ZtCiB8xY4yw2v9U5dc1fnUkanSnCj pool=100000000 sat txid=547c0954...
```

---

## Cluster state

- Docker cluster: **UP** (all 14 containers)
- All 11 GMs: **ENABLED**
- Block height: ~579 at close of session
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
