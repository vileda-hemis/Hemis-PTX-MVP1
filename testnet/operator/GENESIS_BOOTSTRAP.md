# PTX testnet — genesis bootstrap runbook

**Audience:** the coordinator, on launch day. This is a sequence to execute, not a design
document. Every command is meant to be run as written and every check has a stated expected
result, because "it looked fine" is how four defects got as far as a shipped tag this week.

**Prerequisite:** the tag has been cut with the regenerated genesis. Everything below assumes
`Hemis-cli -version` reports that tag.

---

## ★★ Three things to know before you start

**Nothing auto-starts at `nTime`.** The genesis `nTime` is a timestamp compiled into block 0. It
is not a trigger, not a countdown, and no daemon does anything when wall-clock passes it. **The
chain begins when you mine block 1**, and not before. A node started before that time and a node
started a month after it behave identically: they load genesis and wait.

**Below 11 gamemasters, no quorum forms — and that is correct.** `ptx_formation.cpp:92-93`:

```cpp
// The pool >= 11 gate: under-threshold is a DETERMINISTIC SKIP (this
// cycle simply does not form), not an error.
if (pool.GetValidGMsCount() < 11)
    return false;
```

Every formation boundary passes with **no error, no log line, no partial state**. So from your
first GM until roughly operator three, the chain will mine, stake, sync and relay while producing
**zero rolls and zero quorums**. That is the system working. It is also indistinguishable from the
system being broken, which is why it is written here rather than only in the operator guide.

**Do not treat "no quorums" as a fault until `protx_list` shows 11 or more registered, enabled
gamemasters.**

**★★ You run the documented configuration, not a privileged variant.** Your three coordinator nodes
install from the same `install.sh`, on the same ports, with the same loopback-only RPC and the same
one-gamemaster-per-host rule as every operator. There is no coordinator-only path through
`OPERATOR_GUIDE.md`, no extra credential, and nothing in the guide assumes a reader who has one —
KDD-085 removed the last thing that would have made you different. ★ This is worth stating because
the opposite is the usual shape: a runbook that quietly relies on the coordinator having access the
guide does not describe is a runbook whose instructions have never actually been tested by the
people who have to follow them.

**★★ The per-operator gamemaster count is YOURS to set, and it is deliberately not in the operator
guide.** The guide states a shape — one wallet machine, one gamemaster per host, `100 HMS` per
gamemaster — and says the count comes from you. Set it per operator in §10, and **check the
network total, not any one operator's share**: 11 is the floor for a single quorum, and
`floor(total / 11)` is how many can be active at once (ODC-093). Under 22 total the network runs on
one quorum with nothing to carry it through a reform (ODC-094).

**★★ A SOLO NODE DOES NOT STAKE, SO ONE MACHINE CANNOT RUN THIS CHAIN.** The staking loop is
gated twice, and both gates need peers (`src/miner.cpp:144-146`):

```cpp
while ((g_connman && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && Params().MiningRequiresPeers())
        || pwallet->IsLocked() || !fStakeableCoins || gamemasterSync.NotCompleted()) {
    MilliSleep(5000);
}
```

* **Gate A — at least one peer.** `MiningRequiresPeers()` is `!IsRegTestNet()`
  (`src/chainparams.h:83`), so it is **true** here.
* **Gate B — at least two *distinct* peers.** `gamemasterSync.NotCompleted()`
  (`src/gamemaster-sync.cpp:30-37`) stays true until the sporks phase ends, and that needs
  GETSPORKS sent to `GAMEMASTER_SYNC_THRESHOLD = 2` different peers
  (`src/tiertwo/tiertwo_sync_state.h:22`, `src/gamemaster-sync.cpp:272-284` — the same peer is
  skipped by `HasFulfilledRequest` until its 1-hour expiry).

Measured on a real solo node, 2026-08-23 — a wallet with 7,719 stakeable coins and 73,492 HMS,
unlocked, staking enabled, that has **never attempted a stake**:

```
"staking_status": false,  "haveconnections": false,  "gmsync": false,
"staking_enabled": true,  "walletunlocked": true,    "stakeablecoins": 7719,
"lastattempt_tries": 0
```

**This is why step 2 exists.** `generatetoaddress` is not affected — it bypasses both gates — so a
solo host can mine blocks 1–51 by hand and then the chain simply stops. Earlier notes claiming the
chain "self-extends with no further intervention" were measured on a **twelve-peer fleet** and do
not hold for one host.

---

## 1. Install, with the wallet

The wallet is **enabled by default** — the shipped config template writes no `disablewallet` and
`DEFAULT_DISABLE_WALLET = false` (`src/wallet/wallet.h:87`, read at `src/wallet/init.cpp:55`).
There is nothing to turn on.

```bash
git clone -b <the tag> https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
./install.sh
```

Run it on **the mining host**. This host is not a gamemaster and does not need to be — it needs a
wallet, and it **does** need to be publicly reachable on P2P 29994, because it is the first
bootstrap peer the whole network dials.

★ **Provisioning consequence, before you build anything: this is seven machines, not five.** One
mining/caller host, **two** further coordinator nodes (step 2), and your four gamemaster hosts. All
three coordinator nodes need public reachability on 29994 — they are the `addnode` seeds every
operator is given.

★ **The mining node's config differs from a GM's in exactly one respect: it never gets
`gamemaster=1` or `gmoperatorprivatekey`.** Everything else — network selection, ports, rpcbind,
rpcallowip — is identical. There is no mining-specific setting: the internal miner is driven
entirely by RPC (step 5), not by config.

Start it:

```bash
sudo systemctl enable --now hemis-ptx     # the unit install.sh wrote

# ★ Verify by OUTCOME, not by the command returning. Both must answer:
systemctl is-active hemis-ptx                                  # -> active
Hemis-cli getblockcount        # -> a number
```

★ **`systemctl is-active` alone is not enough on an old unit.** The unit `install.sh` writes is
`Type=simple` with an RPC round-trip in `ExecStartPost`, so `active` does mean the daemon answered.
If you are on a unit written before 2026-08-23 it is `Type=forking` with `-daemon`, which reports
`active` for a daemon that already died — `Hemisd -daemon` forks and exits 0 before it validates
anything. The `getblockcount` is what settles it either way.

★★ **No command below passes `-datadir`, and that is the fix for BUG-047 rather than an
abbreviation.** `install.sh` now installs into the daemon's **own default** datadir,
`$HOME/.Hemis` (`GetDefaultDataDir()` in `util/system.cpp`), and writes `ptxtestnet=1` at the top of
`$HOME/.Hemis/Hemis.conf` — the config file the daemon reads from the *base* of that directory
(`Hemis_CONF_FILENAME`, `util/system.cpp:81`; `GetConfigFile` resolves it with `net_specific=false`).

So a bare `Hemisd` finds a real config, selects ptxtestnet (`util/system.cpp:865`), and puts its
chain data in `$HOME/.Hemis/ptxtestnet/`. Previously the installer used a *different* directory, so
a bare `Hemisd` read a config that was not there, took every default and came up on **mainnet** —
silently, looking healthy. Verified 2026-08-23 with no `-datadir`, `-conf` or `-ptxtestnet`:
`"chain": "ptxtestnet"`, `Using data directory .../.Hemis/ptxtestnet`, and no `blocks/` at the top.

★ **The consequence, stated rather than left implicit: on this host the default network is now PTX
testnet.** Running Hemis **mainnet** on the same machine requires an explicit
`-datadir=<somewhere else>`. That is the correct trade for a machine deployed as a PTX gamemaster,
and it is the whole point — the dangerous default was the one that pointed at mainnet.

---

## 2. Stand up the other two coordinator nodes

**Do this before you mine, not after.** See the third note at the top: below two peers the staking
thread never runs, so a chain mined by a solo host stops at whatever height you mined by hand.

Two more machines, same `install.sh`, no gamemaster role, no collateral, nothing to register:

```bash
# on each of the two extra coordinator hosts
git clone -b <the tag> https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
PTX_SEEDS="<mining-host-address>" ./install.sh
sudo systemctl enable --now hemis-ptx

# ★ CHECK THIS ON EACH OF THE TWO NODES BEFORE MOVING ON.
# These two are not spectators -- section 2's whole point is that the mining host
# cannot extend the chain without them, so a node that silently failed to start
# looks exactly like the staking bug you are trying to avoid.
systemctl is-active hemis-ptx                                  # -> active
Hemis-cli getblockcount        # -> a number
Hemis-cli getconnectioncount   # -> >= 1
```

Then point the mining host at both of them and restart it:

```bash
printf 'addnode=%s\naddnode=%s\n' "<node-2>" "<node-3>" >> $HOME/.Hemis/Hemis.conf
sudo systemctl restart hemis-ptx
```

★ **`addnode` must land under the `[ptxtestnet]` header.** It is a network-only setting
(`util/system.cpp:329`); above the header it is dropped with nothing but a startup warning.
`>>` appends to the end of the file, which is inside the section, so the command above is correct
as written.

**Check both gates before you go on:**

```bash
Hemis-cli getconnectioncount     # must be >= 2
Hemis-cli getstakingstatus | grep -E 'haveconnections|gmsync'
```

| field | required | if false |
|---|---|---|
| `haveconnections` | `true` | zero peers — the `addnode` lines are missing, above the header, or the port is blocked |
| `gmsync` | `true` | fewer than two peers have answered GETSPORKS; with exactly one peer this clears only after ~1 hour |

★ These three nodes are also the `PTX_SEEDS` value every operator gets. Record all three addresses
now — they go in the onboarding message (`ONBOARDING.md`).

---

## 3. Confirm you are on the new chain — by observation

The daemon's `Using config file` line is not evidence; it prints the path it *intends* to open
whether or not the file is there, and that is exactly what hid the lowercase-`hemis.conf` bug.
Check outcomes instead.

```bash
Hemis-cli getblockhash 0
Hemis-cli getblockchaininfo | grep '"chain"'
ls $HOME/.Hemis/ptxtestnet/           # chain data must be HERE
ls $HOME/.Hemis/blocks 2>/dev/null    # must NOT exist — that is the mainnet layout
```

| check | expected | if wrong |
|---|---|---|
| `getblockhash 0` | the new genesis hash in `chainparams.cpp` | you are on the old chain or the wrong binary |
| `"chain"` | `ptxtestnet` | the config was not read — **stop** |
| `$HOME/.Hemis/ptxtestnet/` exists | yes | as above |
| `$HOME/.Hemis/blocks` exists | **no** | you are on **mainnet** — stop, delete, re-check the config |

★ **These paths are now `~/.Hemis`, the daemon's own default** (that is BUG-047's fix — see
section 1). The network subdirectory is `ptxtestnet`
(`chainparamsbase.cpp:68`); `blocks/` sitting at the *top* of a datadir is the mainnet layout
(mainnet's subdirectory is `""`) and is the one wrong-network case this check catches on its own.
Every other wrong network — `testnet5`, `regtest`, `ptxbea` — is caught by the `"chain"` row.

★ The last row is not hypothetical, and it has now happened three times. On `ptx01` on
2026-08-21 (BUG-047): 17 MB of mainnet blocks and `Bound to [::]:49165` while a correct config sat
unread in another directory. Then twice more on 2026-08-23, on a clean host, caused by our **own
tooling** — `install.sh` executing a backticked `Hemisd -daemon` out of an unquoted heredoc (18 MB
of mainnet), and a bare `Hemis-cli`, which creates the default datadir tree merely by being invoked.
Installing into the default directory is what removes the trap; keep the check anyway.

Magic bytes are not exposed by any RPC. They are verified by the fact that you connect only to
peers on this chain and by `getblockhash 0` matching — a magic mismatch produces a clean peer
drop, so a wrong-magic node simply finds nobody.

---

## 4. A receiving address

```bash
Hemis-cli getwalletinfo | grep walletname
ADDR=$(Hemis-cli getnewaddress "genesis-float")
echo "$ADDR"
```

Keep `$ADDR`. Everything mined in step 4 pays to it, and you will need it again in step 7.

---

## 5. Mine blocks 1–51

★★ **`generate` does NOT work here.** `rpc/mining.cpp:99-100` refuses it off regtest:

```cpp
if (!Params().IsRegTestNet())
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "This method can only be used on regtest");
```

**Use `generatetoaddress`.** It is not regtest-gated, it takes an explicit destination, it is
synchronous, and it returns the block hashes it produced (`rpc/mining.cpp:151`; the coinbase script
comes from your address at `:165`, and it selects PoW vs PoS from `UPGRADE_POS` at `:174`).

```bash
time Hemis-cli generatetoaddress 49 "$ADDR"
Hemis-cli getblockcount        # must read 49
```

**How long: seconds. Not an hour.** ★★ An earlier version of this runbook said "budget about an
hour" and told you to mine in batches so you could watch the difficulty move. Both were wrong, and
the reason is worth knowing because it also tells you what a *correct* run looks like.

* **Blocks 1–24 do not retarget at all.** `pow.cpp:38-40` returns `powLimit` outright while
  `pindexLast->nHeight < PastBlocksMin (24)`. The genesis `nBits = 0x1e00ffff` is irrelevant to
  every block after block 0.
* **`powLimit` is far lower than it looks.** The literal at `chainparams.cpp:748` is
  **66** hex digits, and `base_blob::SetHex` fills from the *end* (`uint256.cpp:44-55`), so the two
  leading digits are dropped and the stored value is ≈ 2^252 — about **16 expected hashes** per
  block, not 16.8 million. (Same literal on mainnet at `:245`; inherited, not a ptxtestnet defect.)
* **From block 25 DGW is clamped.** `nActualTimespan` is floored at `_nTargetTimespan / 3`
  (`pow.cpp:106-109`), so the target can fall by at most a factor of three per block against a
  24-block average that lags — nowhere near enough to bite inside 51 blocks.

**Measured on ptxbea**, which has the identical `powLimit` and activation heights:

| height | nBits | difficulty | block time |
|---|---|---|---|
| 1 | `200fffff` | 3.7e-09 | 1786788690 |
| 24 | `200fffff` | 3.7e-09 | 1786788695 |
| 25 | `1f246857` | 4.2e-07 | 1786788695 |
| 49 | `1f06ffb4` | 2.2e-06 | 1786788699 |

**Blocks 1→49 took nine seconds.** If yours takes an hour, something is wrong — that is the alarm,
not the expectation.

★ `CheckWork` applies a **±50 % tolerance** to PoW difficulty below height 68589
(`validation.cpp:2949`) — a mainnet-history carve-out that applies to every non-regtest chain. It
means early difficulty is loosely enforced; it does not need action, but do not be surprised if
`difficulty` moves in coarse steps.

---

## 6. The PoS flip at height 50

`UPGRADE_POS` and `POS_V2` activate at **50**, `UPGRADE_V3_4` at **51**
(`chainparams.cpp`, the ptxtestnet block).

★★ **`generatetoaddress` KEEPS WORKING PAST 50, AND THAT IS CORRECT.** An earlier version of this
runbook said to stop and report it if it did. That instruction would have raised an alarm on a
healthy chain. The throw it quoted —

```cpp
if (fGenerate && NetworkUpgradeActive(nHeight, UPGRADE_POS))
    throw JSONRPCError(RPC_INVALID_REQUEST, "Proof of Work phase has already ended");
```

— is at `rpc/mining.cpp:320-321` and lives in **`setgenerate`**, a different RPC.
`generatetoaddress` has no such gate: `generateBlocks` computes `fPoS` from `UPGRADE_POS` and
flips it mid-loop (`rpc/mining.cpp:70`), so one call simply switches from mining PoW blocks to
producing PoS blocks. If it has no stakeable coins it says
`No available coins to stake` (`:46`) — that is the only refusal you should see.

**What you should see:** blocks 1–49 are PoW with a 3800 HMS coinbase; block 50 onward are PoS.

```bash
Hemis-cli generatetoaddress 2 "$ADDR"    # blocks 50 and 51, PoS
Hemis-cli getblockcount                  # 51
Hemis-cli getstakingstatus
```

★ **Now the two gates from the top of this document matter.** From here the chain is supposed to
extend itself, and it will only do so if `haveconnections` and `gmsync` are both `true`. If you
skipped step 2, they are not, and the chain stops at 51 until you fix it.

---

## 7. Balance, and when it is actually spendable

`nCoinbaseMaturity = 10` on ptxtestnet, so **each mined block's reward is spendable 10 blocks
after the block that produced it** — not 10 blocks after you finish mining.

```bash
Hemis-cli getwalletinfo | grep -E '"balance"|immature_balance'
```

* **`balance`** — spendable now.
* **`immature_balance`** — mined but not yet 10 deep (`rpcwallet.cpp:4336`).

**The total.** `GetBlockValue` (`validation.cpp:852-876`) branches only on `IsRegTestNet()` and
`IsTestnet()`, and `IsTestnet()` is `NetworkIDString() == "test"` (`chainparams.h:98`) — ptxtestnet
is neither, so it takes the mainnet schedule: `nHeight > V3_4(51) ? 5.35 : 3800 COIN`. Blocks 1–51
therefore mint **3800 HMS each ≈ 193,800 HMS**, and from 52 onward 5.35.

★ **But it does not all arrive the same way, and only 49 of those blocks have a coinbase.**
Blocks 1–49 are PoW and pay 3800 in the **coinbase**. Blocks 50 and 51 are PoS: their coinbase is
**0.00** and the 3800 rides the **coinstake** instead. Measured on ptxbea — h1 and h49 coinbase
`3800.00`, h50 and h51 coinbase `0.00`, and h50's coinstake spends a 3800 input to 15 outputs
totalling 7600 (stake returned + 3800 reward). You still receive it; it is simply not a coinbase,
so do not go looking for one and do not expect `immature_balance` to move in 51 equal steps.

★ **This is the operator float and it is a deliberate decision, not an accident of inheritance.**

**When it is all usable:** the last PoW block is 49, so its reward matures at height 59. Since
staking continues producing blocks at roughly one per minute, that is about ten minutes after the
flip. Check rather than count:

```bash
watch -n30 'Hemis-cli getwalletinfo | grep -E "\"balance\"|immature_balance"'
# done when immature_balance reaches 0.00000000
```

★ **This loop only terminates if the chain is still producing blocks** — i.e. if step 2 was done
and both staking gates are true. On a solo host it waits forever, and the wait looks exactly like
patience.

---

## 8. ★★ BACK UP `wallet.dat` — before anything else happens

**This wallet holds the entire supply of the network.** It is on hardware with a poor reliability
record. Do this before you send a single coin anywhere.

```bash
mkdir -p ~/ptx-backups
Hemis-cli backupwallet ~/ptx-backups/wallet-genesis-$(date -u +%Y%m%dT%H%M%SZ).dat

# VERIFY it — a backup you have not checked is not a backup
ls -la ~/ptx-backups/
sha256sum ~/ptx-backups/wallet-genesis-*.dat
```

Then **copy it off this machine** — a second host, removable media, anywhere that does not share a
disk, a PSU or a room with the original.

★ `backupwallet` writes a consistent snapshot through the wallet's own database layer — it waits
for the file to be idle, closes the handle and **checkpoints the BDB log into the `.dat`** before
copying (`wallet/db.cpp:777-820`, via `CWallet::BackupWallet` at `wallet/wallet.cpp:4424`). Copying
`wallet.dat` out from under a running daemon skips the checkpoint and can produce a file that opens
and is subtly wrong. Use the RPC.

★ **Repeat the backup after the distribution in step 10**, because the change addresses created by
those sends are new keys that the first backup does not contain.

---

## 9. Confirm staking has taken over, and that the chain is stable

Staking is on by default: `-staking` defaults to `!IsRegTestNet() && DEFAULT_STAKING`
(`init.cpp:1928`), so a wallet-enabled node with mature coins stakes without configuration —
**provided it has peers.** Check the two gates first, because nothing below is meaningful until
they pass:

```bash
Hemis-cli getstakingstatus
```

| field | required | meaning |
|---|---|---|
| `haveconnections` | `true` | Gate A — `miner.cpp:144`, at least one peer |
| `gmsync` | `true` | Gate B — `miner.cpp:145`, sporks answered by two distinct peers |
| `walletunlocked` | `true` | |
| `stakeablecoins` | `> 0` | mature and `nStakeMinDepth = 20` deep |
| `staking_status` | `true` | the staker is actually running |
| `lastattempt_tries` | `> 0` | it has actually tried — a `0` here with everything else green is the solo-node signature |

**The chain is stable when, over 60 consecutive blocks:**

1. **Cadence** — median inter-block time 45–90 s (target 60), no gap over 300 s.
2. **Staking** — every field in the table above is green, and **at least two distinct stakers**
   produce blocks once operators are on. ★ **Correction:** an earlier version said "during your
   solo period one staker is expected and correct". A solo host produces **no** stakers at all —
   see the third note at the top. With step 2 done you have three coordinator nodes, and only the
   mining host holds coins, so **one staker is what you should see until operators arrive**; zero
   means the gates are not passing.
3. **Reorgs** — none deeper than 1. Check `debug.log` for `InvalidChainFound` and disconnect
   events, not just the tip height.
4. **Registration** — every GM in `protx_list` with `PoSePenalty: 0` and `service` equal to the
   address you meant.
5. **Mesh** — every node has ≥ 2 peers; none isolated.
6. **Restart survival** — stop one node, start it, it reaches the same tip hash as its peers.

★ **Not in the condition: quorums, rolls, settlements.** They cannot be exercised below 11 GMs and
their absence is not instability. See the note at the top.

---

## 10. Distribution to operators

★★ **The ordering is not negotiable: install → create the wallet → receive coins.**

The build uses `--with-incompatible-bdb` (`install.sh` section 3b), so a `wallet.dat` created by
this build uses the **system** Berkeley DB, not the 4.8 that stock release binaries expect. Every
machine on this testnet runs the same build, so nothing bites while that holds — but a wallet
created under a mismatched BDB is **unreadable**, and the failure message does not say so. **A
funded wallet that later will not open is a bad way to learn this; an empty one is free to throw
away.**

So each operator must:

1. run `install.sh`,
2. start the node **and restart it once** — the second start is what proves the wallet file opens,
3. send you an address,
4. **only then** receive coins.

**How much each operator gets.**

★ **The per-operator gamemaster count is yours to set, per operator, and it is not in the operator
guide** — see ODC-094. Write **N** for what you agreed with a given operator.

| item | amount |
|---|---|
| collateral, N GMs × 100 HMS | N × 100 |
| registration fees and change | ~5 |
| margin for a fumbled registration, a re-send, a mistyped address | ~95 |
| **recommended per operator** | **(N × 100) + 100 HMS** |

Against a float of ~193,800 the margin costs nothing, and the alternative is a second round of
transfers during the week you least want one. Send it as a single payment per operator and let them
split it — they need **N separate exact 100 HMS outputs**, and `protx_register_fund` can create
those for them.

★★ **Sum the N you actually assigned, and check it against 11 before you send anything.** The
network total is what has to clear a quorum, with spare: at exactly 11 the next GM lost stops
formation silently. ODC-094 has what a given total buys — briefly, `floor(total / 11)` is the
active-quorum ceiling, so a total under 22 means the network runs on one quorum with no successor
pool during a reform.

★ **The collateral is 100 HMS per GM, exactly.** `nGMCollateralAmt = 100 * COIN`
(`chainparams.cpp`, ptxtestnet block). 1000 is mainnet and the old Hemis testnet, and it is the
number anyone with prior Hemis experience will reach for. The check is exact equality
(`specialtx_validation.cpp:119`, `rpc/rpcevo.cpp:569`) and **neither rejection message states the
amount that was required.**

---

## 11. After distribution

* Repeat the `wallet.dat` backup (step 8) — the sends created change addresses.
* Add each operator's node addresses to the `addnode` lines in the config template so later
  operators bootstrap from more than one host. **Do this once three operators are up**, so no
  single machine is load-bearing.
* Expect zero quorums until the eleventh gamemaster registers. Re-read the note at the top of this
  document before reporting it as a fault.
