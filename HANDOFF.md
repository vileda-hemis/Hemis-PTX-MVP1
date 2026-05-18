# End-of-Session Handoff — P2-BLS-03 PASSED

**Branch**: `feature/ptx-phase2-bls`  
**Date**: 2026-05-19  
**Session result**: P2-BLS-03 COMPLETE — PTXSESS tx mined on gm01

---

## What was done this session

### P2-BLS-03: PTXSESS transaction broadcast fix

**Problem**: `ptx_roll` returned `tx_id: "pending"` because the PTX transaction had no fee, was rejected by relay nodes with "insufficient fee: 0 < 4940".

**Fix**: Implemented wallet-funded PTXSESS transaction per design doc §9 / KDD-023.

### Commits this session
- `2d55d32` — allow PTX broadcast to mempool (exempted from vin-empty check, V6.0 gate)
- `2c6fe54` — fund PTXSESS from caller wallet with real UTXOs (P2-BLS-03)

### Files changed
| File | Change |
|------|--------|
| `src/primitives/transaction.h` | Added `IsProbabilisticTx()` helper |
| `src/consensus/tx_verify.cpp` | Exempted PTX from vin-empty check |
| `src/evo/specialtx_validation.cpp` | Exempted PTX from UPGRADE_V6_0 gate |
| `src/chainparams.h` | Added `PTXLotteryPoolAddress()` + `PTXServiceFee()` getters + protected fields |
| `src/chainparams.cpp` | Set lottery pool address + 1 COIN fee in CPTXTestNetParams |
| `src/ptx/ptx_mempool.cpp` | Full rewrite: FundTransaction + ProduceSignature + TryATMP + RelayTx |

### Test result

```
ptx_roll 1 1 100 false [] testgame deadbeef
→ tx_id: 4b51ccc1b4ed83a013fec3b246f3a7246c865c18548d3ee4ee39904248cab117
```

Block 513 on gm01, 6 confirmations:
- vout[0]: OP_RETURN(round_seed)
- vout[1]: 998.9999289 HMS change → caller wallet
- vout[2]: 1.00 HMS → lottery pool `y9ameyKwSUpyX8EY1L8FfniMfSSfYHahhB` ✓
- fee: 7110 sat (miner_fee via FundTransaction CFeeRate(0))

---

## Cluster state

- Docker cluster: **UP** (all 14 containers)
- All 11 GMs: **ENABLED**
- Block height: ~520
- `initgamemaster` syntax in new build: `initgamemaster <privkey> <ip:port>` (not txhash/outputidx)
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

---

## Next milestone: P2-BLS-04

Per the phase 2 plan, P2-BLS-04 is the **lottery reward distribution** milestone.

Candidate tasks:
1. Implement `ptx_claim` RPC — allows a player to claim lottery winnings from the pool address
2. Block-level sweep of the lottery pool (triggered at fixed intervals)
3. Audit that the lottery pool address balance accumulates correctly across multiple `ptx_roll` calls

Check `hemis_kdd_register_v1_3.html` for KDD-030 (GM lottery reward structure) before starting.
