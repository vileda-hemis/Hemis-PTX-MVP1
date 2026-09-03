# PTX public testnet — genesis and seed configuration

**Status:** pre-cut proposal, 2026-08-19. Every value below is **stated with a reason**, not
expressed as a diff against `ptxbea`. That is deliberate: `ptxFormation` is a **positional aggregate
initialiser**, so a field-name grep finds only `consensus/params.h` and the tests — never the
chainparams call sites. A diff-shaped spec is exactly how "nothing sets these" became wrong once
already. Read every value here against the field order in `Consensus::PTXFormationParams`.

---

## §0 — TWO BLOCKERS THAT PRECEDE CUTTING GENESIS

### ★★ 0.1 Five operators cannot form a quorum — RESOLVED 2026-08-19: **Option A**

`ptx_formation.cpp:92` — `if (pool.GetValidGMsCount() < 11) return false;` — a **deterministic
skip**. Quorum size **11** and threshold **t = 6** are **hardcoded literals**, not parameters
(`specialtx_validation.cpp` rejects `member_node_ids.size() > 11` and `premit_commitments.size() < t`,
both DoS 100).

**Five operators running one node each is five GMs. Formation never fires. The testnet produces zero
quorums, zero rolls, and zero settlements** — it would look healthy and do nothing, which is the
worst possible failure shape for a launch.

Three ways out, and the choice changes the operator documents:

| option | requirement | cost |
|---|---|---|
| **A — multiple GMs per operator** *(recommended)* | ≥3 GMs each (5×3 = 15 ≥ 11, leaves 4 spare) | operator guide becomes multi-node; collateral ×3 per operator |
| **B — parameterise quorum size** | consensus change to 11 and t | ★ see 0.2 — this is the exact un-gated class |
| **C — recruit operators** | ≥11 GMs total by any split | schedule, not code |

> ## ★★ SUPERSEDED AGAIN 2026-09-02 — the NUMBER leaves the OPERATOR GUIDE entirely (ODC-094)
>
> **Both banners below record a per-operator number. The operator-facing documents no longer carry
> one at all.** `OPERATOR_GUIDE.md`, `GM_QUICKSTART.md`, `ONBOARDING.md`, `vps-install.sh` and
> `install.sh`'s completion output now state a **shape** — one wallet machine, one gamemaster per
> host, `100 HMS` collateral *per gamemaster*, `(N × 100) + 500 HMS` funding — and say that **N is
> agreed with the coordinator**. `GENESIS_BOOTSTRAP.md` is where a number is now chosen, per
> operator, by the coordinator.
>
> ★ **Why this is not merely tidying.** "4 per operator, 20 total" is the exact configuration
> ODC-094 identifies as fragile: `floor(20/11) = 1`, so the network runs on **one** active quorum,
> and during an idle-reform window — retirement is *not* atomic, unlike rotation, which supersedes
> its predecessor in the same block — there is no second quorum to carry it. A guide that hard-codes
> 20 prescribes that case to every reader who follows it literally. The floor to fix it is a
> **network total**, not a per-operator share, so a per-operator constant is the wrong place to
> express it: five operators at 4 and four operators at 5 are the same network, and only one of
> those numbers survives an operator joining or leaving.
>
> ★ **What is NOT superseded:** Option A over B and C; one GM per host; `nGMCollateralAmt = 100`;
> and `nSupportedQuorums = 1` (see §4 — it is a *declared provisioning* value read only by
> `PTX_Formation_CheckParams`, not the active-quorum ceiling, which is `floor(pool/11)` — ODC-093).
>
> ★ **The fan-out rationale in the 2026-08-21 banner below is now FALSE as stated.**
> `src/ptx/ptx_fanout.cpp` was **deleted** by KDD-085; signing arrives over P2P at each GM's own
> registered address and port. One GM per host is retained because co-hosting is **untested**, not
> because it is impossible. Kept below unedited, as a record of what was believed on that date.

> ## ★★ SUPERSEDED 2026-08-21 — Option A stands, the NUMBER is now **4 per operator, 20 total**
>
> **≥3 / 15 as written below is superseded. The decision is 4 GMs per operator on 4 separate
> hosts, 20 total, nine spare.** Option A itself is unchanged — the reasoning below for choosing it
> over B and C still holds and is why this is a number change, not a re-decision.
>
> **Why the number moved.** At 3 each the margin is four. Losing one operator entirely (3 GMs) plus
> any *single* other GM puts the pool at exactly **11** — `ptx_formation.cpp:92-93`'s floor, where
> the next GM lost stops formation silently and every boundary thereafter is a deterministic skip.
> At 4 each there are nine spare: it survives two whole operators plus stragglers. Host count is
> not a constraint for the five operators.
>
> **Why nothing else moves.** `L_max = floor(pool/11)` is **1** at both 15 and 20, so
> `nSupportedQuorums = 1` in §4 is unchanged and no chainparams value changes with it.
>
> ★ **And a second change that is not about margin at all: ONE GM PER HOST, one internet-routable
> address per GM.** The PTX signing fan-out dials every member on the *same* port number —
> `PTX_FanoutRpcPort()` (`src/ptx/ptx_fanout.cpp:117-120`) takes no per-member argument — so two
> GMs sharing a host at different RPC ports cannot both be reached: one registers, is selected,
> shows `ENABLED`, and silently never signs. Four GMs therefore means **four hosts and four
> addresses**, not four datadirs on one machine. The operator guides and `vps-install.sh` were
> rewritten accordingly on the same date.

★ **DECIDED: Option A — ≥3 GMs per operator (5 × 3 = 15, four spare).**
Reasoning recorded, not just the value:
* **What it does not touch.** Option B changes a consensus constant in the un-gated retroactive class,
  which means it must ship with an activation height **in the same commit, under launch pressure, with
  no time to soak** — the same category V11 was just gated for. Doing a second one in the same week is
  the wrong risk. Option C makes launch depend on people who are not confirmed.
* **It keeps ODC-075 theoretical**, which matters: week one of a 15-node network is not when to be
  exercising quorum-size edge cases.
* **It models mainnet better.** Operators exercise the arm-and-self-check flow **three times rather
  than once**, so we find out immediately whether the guide scales past one instance — and one wallet
  registering several nodes is the realistic pattern (nobody runs a separate wallet per GM).
* **Collateral: 3× per operator.** See the guide for the two funding routes, including whether
  centrally-seeded collateral works (it does, two ways, with different handoff costs).

### ★★ 0.2 The second un-gated retroactive rule is 11 / t=6 — and 0.1 creates pressure to change it

V11 is now activation-gated (`nBoundaryEnforceHeight`, 2026-08-19). The sweep for siblings found that
`PTX_Formation_IsBoundary` is the only **consensus** reader of `ptxFormation` (the other four call
sites are startup validation, ceremony deadlines and the store driver) — but it also found this:

**Quorum size 11 and threshold t=6 are consensus-enforced, un-gated, and retroactive.** Change either
on a chain with history and every historical PTXDKG re-validates differently — a node syncing from
genesis rejects blocks the network accepted. **Split-on-resync, the h385 shape, identical to what V11
was just gated for.** They are *more* exposed than the cadence, because 0.1 supplies an active reason
to change them.

**If option B is ever chosen, it must ship with an activation height in the same commit**, mirroring
`nBoundaryEnforceHeight`. Registering as an owed decision rather than doing it now: on a fresh genesis
the values are correct as they stand, and gating them is only required once history exists.

---

## §1 Network identity

| param | value | reason |
|---|---|---|
| `strNetworkID` | `"ptxtestnet"` | distinct datadir and `-conf` section; never collides with the internal `ptxbea` fleet |
| `pchMessageStart` | `0x50 0x54 0x58 0x54` (`"PTXT"`) | **differs from all four existing magics** — main `a014dd99`, Hemis testnet `f5e6d5ca`, regtest `a1cf7eac`, ptxbea `50545833`. Without this, nodes dial the existing Hemis testnet, fail the handshake and get **banned** |
| `nDefaultPort` (P2P) | **29994** | as specified; hardcoded seeds are published on this port |
| RPC port | **29995** | as specified. ★ Must be reachable **at the registered address**, not merely listening — see the self-check in the operator guide |
| `fRequireStandard` | `false` | matches ptxbea; PTX special transactions are non-standard by shape |

**Address prefixes — reuse the testnet set, change the HRPs:**

| prefix | value | reason |
|---|---|---|
| `PUBKEY_ADDRESS` | 139 | standard testnet prefix — maximum tooling/explorer compatibility |
| `SCRIPT_ADDRESS` | 19 | as ptxbea |
| `STAKING_ADDRESS` | 73 | as ptxbea |
| `SECRET_KEY` | 239 | Bitcoin testnet default; every WIF tool already handles it |
| `EXT_PUBLIC_KEY` / `EXT_SECRET_KEY` | `3a 80 61 a0` / `3a 80 58 37` | as ptxbea |
| `EXT_COIN_TYPE` | `80 00 00 01` | BIP-44 testnet coin type 1 |
| bech32 HRPs | `ptxtest`, `ptxtestview`, `ptxtestivk`, `bls-sk-ptxtest`, `bls-pk-ptxtest`, … | **network-distinct** so a human-readable address cannot be mistaken for a mainnet or ptxbea one |

★ **Stated tradeoff:** reusing prefix 139 means a PTX-testnet address is visually indistinguishable
from a Hemis-testnet address. Chains are not cross-spendable so this is user error, not a protocol
risk, and the tooling compatibility is worth more. **The bech32 HRPs carry the distinction.**

**Seeds:**

| param | value | reason |
|---|---|---|
| `vSeeds` (DNS) | **empty** — `vSeeds.clear()` | five known operators do not need a seeder; a DNS seeder is infrastructure to run, secure and keep alive for no benefit at this size |
| `vFixedSeeds` | **the five operator IPs, hardcoded, port 29994** | populated at cut time from the addresses operators register. Must be their **externally reachable** addresses |

---

## §2 Genesis block — REGENERATE, do not reuse

**Decision: regenerate.** Reusing ptxbea's genesis would carry its hash
(`000000dccda161...`) onto a different network. The magic already separates them at the wire, but a
distinct genesis hash makes chain identity unambiguous in every log line, explorer and checkpoint,
and costs one mining run.

```cpp
genesis = CreateGenesisBlock(<nTime>, <nNonce>, 0x1e00ffff, 1, 0 * COIN);
consensus.hashGenesisBlock = genesis.GetHash();
assert(consensus.hashGenesisBlock == uint256S("0x<new>"));
assert(genesis.hashMerkleRoot   == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));
```

* **Recipe already exists** — `chainparams.cpp:219`, the commented-out `findGenesisPTXBea()`: threads
  over `findGenesisBlock(nTime, offset, 0x1e00ffff, 1, 0)`. Uncomment, set `nTime`, run once, record.
* `nTime`: the intended launch date at 00:00:00 UTC. **State it; do not inherit ptxbea's 1779926400.**
* `nBits` `0x1e00ffff`: same starting difficulty as every non-main network here.
* ★ **The merkle root will be UNCHANGED** (`93ad7b45…`, identical on main and ptxbea) because it
  derives from the shared genesis coinbase message. **This is expected, not a mistake** — only change
  it if the `pszTimestamp` is deliberately changed, which would require a new merkle assert too.
* Add a `mapCheckpoints` entry for the genesis block and nothing else — checkpoints on a chain with no
  history are noise.

---

## §3 Activation heights — ALL genesis-active, each stated deliberately

> ### ★★ CORRECTION 2026-08-21 — `UPGRADE_POS` / `POS_V2` / `V3_4` are the exception
>
> The table below states **1** for `UPGRADE_POS` and `POS_V2` and *"State 1 and mean it."*
> **That value produces a chain permanently stuck at height 0** and must not be used.
>
> `rpc/mining.cpp:321-322` refuses `generate` once PoS is active at the next height, and at
> genesis `nHeight = Height() + 1 = 1`, so an activation height of 1 makes
> `NetworkUpgradeActive(1, UPGRADE_POS)` true (`upgrades.cpp:92`) and the **first** `generate`
> call throws *"Proof of Work phase has already ended"*. No PoW block can be mined, so no coins
> exist, so nothing can stake.
>
> **Blocks 1..49 must be PoW to mint the initial supply before anything can stake.** ptxbea proves
> the shape: `POS = POS_V2 = 50`, `V3_4 = 51`, PoW blocks 1–49 at 3800 HMS each, PoS from h50 —
> verified live 2026-08-21. **The values now in `chainparams.cpp` are 50 / 50 / 51 and the reasoning
> is repeated in a source comment beside them**, because §3 as written will otherwise persuade the
> next reader to "fix" it back.
>
> The §3 *principle* — genesis-active for anything whose purpose is grandfathering — is correct and
> unchanged. These three are **bootstrap** heights, not grandfathering gates.

**Principle:** on a fresh genesis there is no pre-activation history to protect, so every gate whose
purpose is grandfathering must be **open from block 0**. Inheriting a non-zero height from ptxbea
imports pacing that protects history this chain does not have.

| upgrade | value | reason |
|---|---|---|
| `UPGRADE_POS`, `POS_V2` | **1** | PoS from the first block after genesis |
| `UPGRADE_V3_4`, `V4_0`, `V5_0`, `V5_2`, `V5_3`, `V5_5` | **1** | all legacy staging; nothing to stage on a new chain |
| `UPGRADE_BIP65`, `UPGRADE_V6_0` | `ALWAYS_ACTIVE` | as every network |
| `UPGRADE_ZC`, `ZC_V2`, `ZC_PUBLIC` | `100000000` | zerocoin stays **off**, as on ptxbea — effectively never |
| `UPGRADE_TESTDUMMY` | `NO_ACTIVATION_HEIGHT` | test-only |

★ ptxbea uses 50/51/265 for these. Those are **artefacts of its own bring-up**, not requirements.
State 1 and mean it.

---

## §4 `consensus.ptxFormation` — all ten fields, in positional order

```cpp
// {name, B, R, budget, L, retire, grace, rate, statelessH, boundaryEnforceH}
consensus.ptxFormation = {"ptxtestnet", 60, 1440, 80, 1, 200, 1, 40, 0, 0};
```

| # | field | value | reason |
|---|---|---|---|
| 1 | `name` | `"ptxtestnet"` | logging only, not consensus |
| 2 | `nBoundaryInterval` (B) | ★ **60** | formation cadence — **decided 2026-08-19, widened from ptxbea's 30**. One boundary per hour at 60s spacing. Full margin derivation below |
| 3 | `nRotationInterval` (R) | **1440** | KDD-045 key-compromise window ≈ 1 day at 60s spacing. Bounded by **this**, not by B |
| 4 | `nCeremonyBudget` | **80** | ODC-050 stall-out. ★ Must **not** track B — a ~27-block ceremony under a 30-block cadence lives on exactly this separation |
| 5 | `nSupportedQuorums` (L) | **1** | Guard 1 declares what the network is provisioned for. Five operators support **one** quorum (§0.1); declaring 8 like ptxbea would be a lie the guard cannot catch |
| 6 | `nRetireWindow` | **200** | KDD-074 idle arm live, as ptxbea |
| 7 | `nReformGrace` | **1** | ODC-054: L>1 hard-requires grace>0; harmless at L=1 and correct if L rises |
| 8 | `nReformRateWindow` | **40** | KDD-074 limiter, as ptxbea |
| 9 | `nReformStatelessHeight` | ★ **0** | **NOT 900.** ptxbea's 900 exists solely so its pre-BUG-036 history replays byte-identically. A fresh genesis has no such history: 900 blocks of legacy stored-stamp pacing on a new chain is a trap with no purpose. **0 = stateless from genesis** |
| 10 | `nBoundaryEnforceHeight` | ★ **0** | new 2026-08-19 gate. 0 = **V11 enforced from genesis** — correct for a fresh chain, and it means every PTXDKG this network ever accepts is on-boundary, so the gate never needs to move |

★ **DECIDED — B = 60, and the margin is stated in the unit that actually moves.**

**M is BLOCK-COUNT-BASED, and `nTimeSlotLength` does not enter it.** M is defined as

> **M = S + 6·pb + W_mine** — S = setup (0..pb) in blocks, pb = per-phase blocks, W_mine = mining
> window in blocks (11, the LLMQ `dkgMiningWindowStart/End` analog, itself a block count).

Every term is a block count; the familiar "M ≈ 47 min" is a *derived* wall-clock figure obtained by
multiplying by `nTargetSpacing = 60s`, which is 60 on all five network definitions. `nTimeSlotLength`
appears nowhere in M, and `chainparams.cpp:915` records that it does not move block cadence either
(`nTargetSpacing` stays 60). **So B and `nTimeSlotLength` were never coupled — they only looked it.**
`nTimeSlotLength = 15` is inherited from mainnet/ptxbea unchanged.

**Why 60, expressed as the propagation it tolerates.** pb is **propagation-bound**, and it enters M
with **coefficient 6** — so M is highly sensitive to per-phase propagation, and B's real meaning is
"how slow may a phase get before a boundary can fire mid-ceremony":

| B | tolerated ceremony | implied max `pb` (S=0, W_mine=11) | against the mainnet-grade baseline `pb = 6` |
|---|---|---|---|
| **30** (ptxbea) | 30 blocks | `6·pb + 11 ≤ 30` → **pb ≤ 3** | ★ **half the baseline** — only works at drill RTT |
| **60** (chosen) | 60 blocks | `6·pb + 11 ≤ 60` → **pb ≤ 8** | **33% headroom above baseline** |

This is precisely why 30 fit the drill and will not fit a public network: the drill measured a
27-block ceremony **on a single host at near-zero RTT**, i.e. pb ≈ 2–3 — inside 30 by three blocks,
but that margin was a property of the measurement environment, not of the protocol. A public network of five
operators across real links has **worse** propagation, not better, and a ceremony that overruns its
boundary is the anchor-split problem's neighbour.

**B = 60 tolerates M = 60 blocks**: 1.28× the provisional mainnet-grade floor of 47, 2.2× the observed
drill ceremony, and pb degrading from 6 to 8 before the floor is reached.

★ **And the cost is nil.** B is a **security ceiling only** — handover-at-accept (KDD-063) keeps
rotation available across boundaries, so a wider B never makes the quorum unavailable. Key-rotation
cadence is governed by **R = 1440**, not by B. The only thing a wider B delays is how soon the *first*
quorum forms (h60 rather than h30 — one hour on a network with no throughput pressure).

---

## §5 PTX economic and timing parameters

| param | value | reason |
|---|---|---|
| `strPTXLotteryPoolAddress` | `""` | accumulation via `LOTTERY_ACCUM_SCRIPT` (ODC-022), same as ptxbea — no pool address exists |
| `nPTXServiceFee` | **1 × COIN** | 1 HMS per roll, KDD-043; spork-adjustable |
| `nPTXSettlementWindow` | **60** | ~60 blocks ≈ 1 hour at 60s spacing |
| `nPTXSeedHeightWindow` | **60** | ODC-073 Step 1. Bracketed by two real quantities: **floor** = commit-to-mine lag (~12 blocks incl. congestion and retry), **ceiling** = `nRetireWindow` 200. 60 sits strictly inside and equals the settlement horizon |
| `nPTXPayoutMinerFee` | **10000** (0.0001 HMS) | miner incentive inside PTXPAYOUT |
| `nTimeSlotLength` | ★ **15** or **60 — decide** | ptxbea uses 15 deliberately. This interacts with §4's M floor. **Set together with B** |
| `nLLMQConnectionRetryTimeout` | **10** | as ptxbea |
| `consensus.llmqs[LLMQ_TEST]`, `llmqChainLocks` | `llmq_test` | small-network LLMQ shape |

---

## §6 A1 / A2 / excludes — NOT PRESENT

**Recorded as a state, not as an open item.** A1/A2/excludes appear in **neither the source nor
`doc/ptx/`**. They therefore **cannot be genesis-active, because they do not exist** — there is no
value to state and nothing to inherit. Genesis is **not blocked** on them.

If they are built later, they arrive on a chain that already has history and so **require an
activation height from the start**, exactly like `nBoundaryEnforceHeight` (KDD-092) — that is the
standing rule for anything added after this cut, not a special case for these.

---

## §7 Pre-cut checklist

1. ☑ **§0.1 resolved — Option A**, ≥3 GMs per operator (15 total, 4 spare).
2. ☑ **§4 decided — B = 60**; `nTimeSlotLength = 15` inherited. M is block-count-based, so the two were independent.
3. ☑ A1/A2/excludes — **not present**; genesis not blocked (§6).
4. ☐ `findGenesisPTXBea()` re-run with the launch `nTime`; hash and nonce recorded; both asserts updated.
5. ☐ Five operator addresses collected and written into `vFixedSeeds` on port 29994.
6. ☐ Magic `PTXT` confirmed absent from every other Hemis network.
7. ☐ `nReformStatelessHeight = 0` and `nBoundaryEnforceHeight = 0` — **verified by position**, not by
   field-name grep, against the ten-field order in §4.
