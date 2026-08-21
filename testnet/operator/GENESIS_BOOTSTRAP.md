# PTX testnet — genesis bootstrap runbook

**Audience:** the coordinator, on launch day. This is a sequence to execute, not a design
document. Every command is meant to be run as written and every check has a stated expected
result, because "it looked fine" is how four defects got as far as a shipped tag this week.

**Prerequisite:** the tag has been cut with the regenerated genesis. Everything below assumes
`Hemis-cli -version` reports that tag.

---

## ★★ Two things to know before you start

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
wallet and a public address is optional for it. Your four GMs are separate machines.

★ **The mining node's config differs from a GM's in exactly one respect: it never gets
`gamemaster=1` or `gamemasterblsprivkey`.** Everything else — network selection, ports, rpcbind,
rpcallowip — is identical. There is no mining-specific setting: the internal miner is driven
entirely by RPC (step 4), not by config.

Start it:

```bash
sudo systemctl enable --now hemis-ptx     # the unit install.sh wrote
```

---

## 2. Confirm you are on the new chain — by observation

The daemon's `Using config file` line is not evidence; it prints the path it *intends* to open
whether or not the file is there, and that is exactly what hid the lowercase-`hemis.conf` bug.
Check outcomes instead.

```bash
Hemis-cli getblockhash 0          # must equal the genesis hash in chainparams.cpp
Hemis-cli getblockchaininfo | grep '"chain"'      # must be "ptxtestnet"
ls ~/.Hemis/ptxtestnet/           # chain data must be HERE
ls ~/.Hemis/blocks 2>/dev/null    # must NOT exist — that is the mainnet layout
```

| check | expected | if wrong |
|---|---|---|
| `getblockhash 0` | the new genesis hash | you are on the old chain or the wrong binary |
| `"chain"` | `ptxtestnet` | the config was not read — **stop** |
| `~/.Hemis/ptxtestnet/` exists | yes | as above |
| `~/.Hemis/blocks` exists | **no** | you are on **mainnet** — stop, delete, re-check the config |

★ The last row is not hypothetical. It happened on `ptx01` on 2026-08-21 (BUG-047): 17 MB of
mainnet blocks and `Bound to [::]:49165` while a correct config sat unread in another directory.

Magic bytes are not exposed by any RPC. They are verified by the fact that you connect only to
peers on this chain and by `getblockhash 0` matching — a magic mismatch produces a clean peer
drop, so a wrong-magic node simply finds nobody.

---

## 3. A receiving address

```bash
Hemis-cli getwalletinfo | grep walletname     # which wallet you are talking to
ADDR=$(Hemis-cli getnewaddress "genesis-float")
echo "$ADDR"
```

Keep `$ADDR`. Everything mined in step 4 pays to it, and you will need it again in step 6.

---

## 4. Mine blocks 1–51

★★ **`generate` does NOT work here.** `rpc/mining.cpp:99-100` refuses it off regtest:

```cpp
if (!Params().IsRegTestNet())
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "This method can only be used on regtest");
```

**Use `generatetoaddress`.** It is not regtest-gated, it takes an explicit destination, it is
synchronous, and it returns the block hashes it produced (`rpc/mining.cpp:151`; the coinbase script
comes from your address at `:165`, and it selects PoW vs PoS from `UPGRADE_POS` at `:174`).

```bash
# a few first, to measure
time Hemis-cli generatetoaddress 5 "$ADDR"

# then the rest, in batches so you can watch the difficulty move
Hemis-cli generatetoaddress 20 "$ADDR"
Hemis-cli generatetoaddress 24 "$ADDR"     # stop at height 49

Hemis-cli getblockcount                    # must read 49
```

**How long.** At the floor difficulty `nBits = 0x1e00ffff` the target is ≈ 2^231, so a block is
expected in ≈ **2^24 ≈ 16.8 million hashes** — seconds on any modern CPU. The genesis mine's own
evidence agrees: ptxbea's winning thread found its nonce after ~136,000 iterations, ~4.4M across
32 threads.

★ **But the first blocks will be the fastest ones.** Difficulty retargets toward
`nTargetSpacing = 60` (`chainparams.cpp`), so as you mine faster than one per minute the work per
block climbs. **Expect the first handful to be near-instant and the rate to converge toward roughly
one per minute.** Budget about an hour for 51 blocks and watch rather than assume:

```bash
watch -n5 'Hemis-cli getblockcount; Hemis-cli getmininginfo | grep -E "difficulty|blocks"'
```

★ `CheckWork` applies a **±50 % tolerance** to PoW difficulty below height 68589
(`validation.cpp:2949`) — a mainnet-history carve-out that applies to every non-regtest chain. It
means early difficulty is loosely enforced; it does not need action, but do not be surprised if
`difficulty` moves in coarse steps.

---

## 5. The PoS flip at height 50

`UPGRADE_POS` and `POS_V2` activate at **50**, `UPGRADE_V3_4` at **51**
(`chainparams.cpp`, the ptxtestnet block). The flip is enforced in the mining RPC itself
(`rpc/mining.cpp:301-302`):

```cpp
if (fGenerate && NetworkUpgradeActive(nHeight, UPGRADE_POS))
    throw JSONRPCError(RPC_INVALID_REQUEST, "Proof of Work phase has already ended");
```

**What you should see:** at height 49, one more `generatetoaddress` produces block 50 and then PoW
mining stops being possible. Blocks 50 onward are **proof of stake**, produced by the wallet's
staking thread rather than by you. The Phase D-bootstrap run is the precedent: 49 PoW blocks, flip
at 50.

```bash
Hemis-cli getblockcount           # 50
Hemis-cli getstakingstatus        # the wallet's staking view
```

★ If `generatetoaddress` keeps working past 50, the activation heights are wrong — **stop and
report it**, because it means the chain is not the one that was cut.

---

## 6. Balance, and when it is actually spendable

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
therefore mint **3800 HMS each ≈ 193,800 HMS**. Confirmed on ptxbea, which has the same shape:
h1, h2, h25 and h49 each paid 3800.00.

★ **This is the operator float and it is a deliberate decision, not an accident of inheritance.**

**When it is all usable:** the last PoW block is 49, so its reward matures at height 59. Since
staking continues producing blocks at roughly one per minute, that is about ten minutes after the
flip. Check rather than count:

```bash
watch -n30 'Hemis-cli getwalletinfo | grep -E "\"balance\"|immature_balance"'
# done when immature_balance reaches 0.00000000
```

---

## 7. ★★ BACK UP `wallet.dat` — before anything else happens

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

★ `backupwallet` writes a consistent snapshot through the wallet's own database layer. Copying
`wallet.dat` out from under a running daemon does not, and can produce a file that opens and is
subtly wrong. Use the RPC.

★ **Repeat the backup after the distribution in step 9**, because the change addresses created by
those sends are new keys that the first backup does not contain.

---

## 8. Confirm staking has taken over, and that the chain is stable

Staking is on by default: `-staking` defaults to `!IsRegTestNet() && DEFAULT_STAKING`
(`init.cpp:1928`), so a wallet-enabled node with mature coins stakes without configuration.

**The chain is stable when, over 60 consecutive blocks:**

1. **Cadence** — median inter-block time 45–90 s (target 60), no gap over 300 s.
2. **Staking** — `getstakingstatus` healthy, and **at least two distinct stakers** producing
   blocks once operators are on. During your solo period one is expected and correct.
3. **Reorgs** — none deeper than 1. Check `debug.log` for `InvalidChainFound` and disconnect
   events, not just the tip height.
4. **Registration** — every GM in `protx_list` with `PoSePenalty: 0` and `service` equal to the
   address you meant.
5. **Mesh** — every node has ≥ 2 peers; none isolated.
6. **Restart survival** — stop one node, start it, it reaches the same tip hash as its peers.

★ **Not in the condition: quorums, rolls, settlements.** They cannot be exercised below 11 GMs and
their absence is not instability. See the note at the top.

---

## 9. Distribution to operators

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

| item | amount |
|---|---|
| collateral, 4 GMs × 100 HMS | 400 |
| registration fees and change | ~5 |
| margin for a fumbled registration, a re-send, a mistyped address | ~95 |
| **recommended per operator** | **500 HMS** |

Five operators × 500 = **2500 HMS**, against a float of ~193,800. The margin costs nothing and the
alternative is a second round of transfers during the week you least want one. Send it as a single
payment per operator and let them split it — they need **four separate exact 100 HMS outputs**, and
`protx_register_fund` can create those for them.

★ **The collateral is 100 HMS per GM, exactly.** `nGMCollateralAmt = 100 * COIN`
(`chainparams.cpp`, ptxtestnet block). 1000 is mainnet and the old Hemis testnet, and it is the
number anyone with prior Hemis experience will reach for. The check is exact equality
(`specialtx_validation.cpp:119`, `rpc/rpcevo.cpp:569`) and **neither rejection message states the
amount that was required.**

---

## 10. After distribution

* Repeat the `wallet.dat` backup (step 7) — the sends created change addresses.
* Add each operator's node addresses to the `addnode` lines in the config template so later
  operators bootstrap from more than one host. **Do this once three operators are up**, so no
  single machine is load-bearing.
* Expect zero quorums until the eleventh gamemaster registers. Re-read the note at the top of this
  document before reporting it as a fault.
