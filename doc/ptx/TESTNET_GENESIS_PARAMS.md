# Public PTX testnet — genesis and seed configuration

**Status:** SPEC, not yet implemented. **Date:** 2026-08-18.
**Scope:** every parameter a fresh public testnet chain needs, with a stated value and the reason
for it. This is deliberately *not* a diff against `ptxbea` — a diff hides the fields nobody set,
and on `PTXFormationParams` the unset fields are invisible to a name grep because the struct is
initialised positionally. Every field is listed here whether or not it changes.

Ground truth for all line references: `src/chainparams.cpp`, `src/consensus/params.h`,
`src/chainparams.h`, branch `feature/ptx-dkg` @ 822577b.

---

## 0. The first decision: a NEW network, not a promoted `ptxbea`

**Recommendation: cut a new network id. Do not ship `ptxbea` as the public testnet.**

`ptxbea` carries four dev-only carve-outs that are correct for a local disposable fleet and wrong
for five external operators. They cannot simply be deleted, because the local fleet still needs them.

| carve-out | site | why it must not ship |
|---|---|---|
| `IsPTXBeaFleetAddr` routability bypass for `172.31/16`, `172.32/16`, `fd00:31::/64`, `fd00:32::/64` | `src/net.cpp:219-244` | lets unroutable private addresses be gossiped as GM service addresses |
| `IsRegTestNet() \|\| IsPTXBeaTestNet()` bypasses `addr.IsRoutable()` | `src/evo/specialtx_validation.cpp:40` | same, at the consensus-validation layer |
| `ptx_debug_ptxdkgpopulate` — **open on ptxbea** | `src/rpc/ptx.cpp:874` | writes DKG state feeding **consensus-facing block assembly** |
| `ptx_debug_selectquorum` — **open on ptxbea** | `src/rpc/ptx.cpp:1684` | exposes selection internals |

Both debug RPCs are written to **fail closed for any network that is neither regtest nor ptxbea**
(`rpc/ptx.cpp:869-872` says so explicitly). **A new network id therefore closes them for free** —
which is the single strongest argument for this decision. Promoting `ptxbea` ships them open.

> **Independent hardening item, unrelated to the choice above:** `ptx_debug_setnodefailmode`
> (`src/rpc/ptx.cpp:606`) has **no network gate at all** — it is callable on *every* network
> including mainnet. It perturbs ceremony messaging only, and RPC requires auth, so an operator can
> only degrade their own node's ceremony participation (and their own PoSe score). It is not a
> launch blocker, but it must be gated to regtest+dev before mainnet. **Owed.**

Everything below assumes a new class `CPTXNetParams`, `strNetworkID = "ptxnet"`, selected by
`-ptxnet`, registered in `src/chainparamsbase.cpp:14-18` and the factory at
`src/chainparams.cpp:1061-1074`. **The name `ptxnet` is a placeholder for the operator to confirm.**

---

## 1. Chain identity

| param | value | reason |
|---|---|---|
| `strNetworkID` | `"ptxnet"` | distinct from all four existing ids (`main`, `test`, `regtest`, `ptxtestnet`, `ptxbea`); becomes the datadir subdirectory |
| `pchMessageStart` | **`0x50 0x54 0x58 0x34`** ("PTX4") | must differ from mainnet `a0 14 dd 99` **and** Hemis testnet `f5 e6 d5 ca`, and from `regtest a1 cf 7e ac`, `ptxtestnet "PTX2"`, `ptxbea "PTX3"`. Continues the readable PTX-n convention. Magic mismatch causes a clean peer drop, so this — with genesis — is what actually keeps the chains apart |
| `nDefaultPort` (P2P) | **29994** | as specified. ⚠ **This is also `ptxbea`'s P2P port.** Harmless while the dev fleet is local-only and the testnet is on migrated hardware, but the two must never share a host. Pick 29996 if they ever might |
| RPC port | **29995** | `src/chainparamsbase.cpp` — P2P+1, the established convention. Same host-collision caveat |
| datadir | `ptxnet` | follows `strNetworkID` |
| `fRequireStandard` | `false` | testnet convention; keeps non-standard PTX special-tx shapes relayable |
| `IsTestChain()` | must include `ptxnet` | `src/chainparams.h:81` — gates test-only relaxations |
| `CGamemaster::IsValidNetAddr` | do **NOT** add `ptxnet` | `src/gamemaster.cpp:197-198` permits any address on regtest/ptxtestnet. `ptxbea` is correctly absent; `ptxnet` must be too, so GM service addresses are required to be routable |

---

## 2. Genesis block — REGENERATE, do not reuse

**Decision: regenerate.** Reusing `ptxbea`'s genesis would give the new chain an identical genesis
hash, defeating the separation the new magic buys and making the two chains indistinguishable to
any tool that keys on genesis.

| param | value | reason |
|---|---|---|
| `pszTimestamp` | unchanged — the Takosha Churu memorial string (`src/chainparams.cpp:67`) | a global constant shared by all five networks; it is the project's memorial line and must not be forked per-network |
| `genesisOutputScript` | unchanged (`chainparams.cpp:68`) | ditto — global |
| merkle root | **`93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113`** | *derived, not chosen*: identical on all five networks because the timestamp string and output script are global. Assert it like the others |
| `nTime` | **set at cut time to the launch date, 00:00:00 UTC** — e.g. 2026-08-24 = `1787529600`, 2026-08-25 = `1787616000` | a meaningful launch stamp; must be fixed *before* mining the nonce |
| `nNonce` | **must be mined** — unknown until `nTime` is fixed | brute-forced against `nBits` |
| `nBits` | `0x1e00ffff` | identical on all five networks; no reason to differ |
| `nVersion` | `1` | as all networks |
| genesis reward | `0 * COIN` | as all networks — genesis coinbase unspendable by value as well as by DB |

**Mining tool.** There is none in `contrib/`, `share/`, or `util/`. The only miner is **commented
out inside chainparams.cpp**: `findGenesisBlock()` at `:202-218` and `findGenesisPTXBea()` at
`:219-228` (spawns `hardware_concurrency()` threads, each starting at `i*(UINT32_MAX/N)`, prints
nonce/hash/merkle, then `std::exit(0)`). **Owed work:** copy `findGenesisPTXBea` to a
`findGenesisPTXNet` with the new `nTime`, build, run once, record the nonce, re-comment. Budget an
hour; the `ptxbea` nonce was 2,550,273,078, so a full-range search is realistic.

**Checkpoint at height 0 must be the new genesis hash.** `MapCheckpoints` gets exactly one entry,
`{0, <new genesis hash>}`. ⚠ Do not repeat regtest's bug: its height-0 checkpoint is `0x001`
(`chainparams.cpp:195`), which is not its genesis hash. `CCheckpointData` = `{nTime, 0, 0}` following
`ptxbea` (`:726-731`) — zero transactions, zero tx/day, on a chain with no history.

---

## 3. Seeds

| param | value | reason |
|---|---|---|
| `vSeeds` (DNS) | **empty** — `vSeeds.clear()` | five known operators exchange addresses out of band; a DNS seeder is infrastructure with nothing to do, and an unmaintained seed host is worse than none |
| `vFixedSeeds` | **the five operator endpoints, port 29994** | hardcoded so a fresh node bootstraps with no seeder |

`vFixedSeeds` takes BIP155-serialised tuples from `src/chainparamsseeds.h`, generated by
`contrib/seeds/generate-seeds.py`. Today that header holds only `chainparams_seed_main[]` (2 entries)
and `chainparams_seed_test[]` (1). **Owed work:** add a `chainparams_seed_ptxnet[]` array — feed the
generator a plain-text list of `host:29994` lines. Note the port is encoded *into* each tuple
(mainnet's entries end `0xc0,0x0d` = 49165), so the array is only valid for port 29994.

Operator addresses are not in this document by design — they are deployment data, and they are not
known until the boxes are ordered.

---

## 4. Address prefixes

**Decision: keep the shared non-mainnet prefix set. Give the network its own bech32 HRPs.**

`test`, `regtest`, `ptxtestnet` and `ptxbea` already share **byte-identical** base58 prefixes.
Chain separation is done by magic bytes and genesis hash, not by prefixes; diverging here would
break every wallet/tool that already handles Hemis test addresses, for no security gain.

| entry | value | reason |
|---|---|---|
| `PUBKEY_ADDRESS` | `139` | shared non-mainnet value |
| `SCRIPT_ADDRESS` | `19` | shared |
| `STAKING_ADDRESS` | `73` | shared |
| `SECRET_KEY` | `239` | shared |
| `EXT_PUBLIC_KEY` | `3a 80 61 a0` | shared |
| `EXT_SECRET_KEY` | `3a 80 58 37` | shared |
| `EXT_COIN_TYPE` | `80 00 00 01` (BIP44 coin type 1) | the registered "testnet (all coins)" type |

bech32 HRPs **do** get their own set, following `ptxbea`'s precedent — a Sapling address that
decodes on the wrong chain is a real footgun, and the HRP is the only thing that prevents it:
`ptxnet` / `ptxnetview` / `ptxnetivk` / `p-secret-spending-key-ptxnet` / `ptxnetxview` /
`bls-sk-ptxnet` / `bls-pk-ptxnet`.

---

## 5. Activation heights — everything genesis-active

**Principle: on a fresh chain there is no pre-activation history to protect, so every gate opens at
genesis.** A non-zero activation height on a new chain buys nothing and creates a window in which
the chain behaves like a legacy chain it never was.

| upgrade | value | reason |
|---|---|---|
| `BASE_NETWORK` | `0` | as every network |
| `UPGRADE_POS` | `0` | ⚠ **see the caveat below — this one is not free** |
| `UPGRADE_POS_V2` | `0` | with POS |
| `UPGRADE_BIP65` | `0` | `regtest`/`ptxtestnet`/`ptxbea` already use 0 |
| `UPGRADE_V3_4` | `0` | ⚠ **drives the block-reward schedule — see §7** |
| `UPGRADE_V4_0` | `0` | DIP3-era special transactions available from genesis |
| `UPGRADE_V5_0/_V5_2/_V5_3/_V5_5` | `0` | no legacy period to stage through |
| **`UPGRADE_V6_0`** | **`0` (ALWAYS_ACTIVE)** | **the master PTX gate.** `IsDIP3Enforced()` = `NetworkUpgradeActive(h, UPGRADE_V6_0)` (`src/evo/deterministicgms.cpp:943-946`), and `PTX_Formation` returns early unless true (`src/ptx/ptx_formation.cpp:731-732`). Only `ptxbea` sets it today; on `main`/`test`/`regtest`/`ptxtestnet` it is `-1` and **the entire PTX/DGM subsystem is dead**. Getting this wrong produces a chain that syncs and stakes and silently never forms a quorum |
| `UPGRADE_ZC`, `ZC_V2`, `ZC_PUBLIC` | `100000000` | zerocoin is dead; park it far out as `ptxtestnet`/`ptxbea` do |
| `UPGRADE_TESTDUMMY` | `-1` | no activation |
| `UPGRADE_GM_ENABLE` | *see trap below* | |
| `hashActivationBlock` | **none** | set only on `main`/`test`; meaningless on a chain whose activations are all at genesis |

> ⚠ **`UPGRADE_POS` cannot be 0 in practice.** Blocks 1..N must be PoW to mint the initial coin
> supply before anything can stake. `ptxbea`/`ptxtestnet` use `50`, and `UPGRADE_V3_4 = 51`.
> **Recommended: keep `POS = POS_V2 = 50` and `V3_4 = 51`.** These are *bootstrap* heights, not
> legacy-compatibility gates — the §5 principle does not apply to them. Everything else goes to 0.

> ⚠ **`NetworkUpgradeInfo` indices 13/14 are swapped relative to the enum.**
> `src/consensus/upgrades.cpp:15-77` orders `… 13 "v6_evo", 14 "GM_Enable" …` while the enum
> (`params.h:26-45`) has `13 = UPGRADE_GM_ENABLE`, `14 = UPGRADE_V6_0`. So `-nuparams=v6_evo:H`
> writes the **dead** `UPGRADE_GM_ENABLE`, and `-nuparams=GM_Enable:H` moves the real gate; the log
> and RPC strings for the two are likewise swapped. `-nuparams` is regtest-only
> (`src/init.cpp:934-936`) so it cannot corrupt the testnet, but **anyone debugging via nuparams
> will be lied to.** `UPGRADE_GM_ENABLE` is assigned by no network and read nowhere — its
> `nActivationHeight` is indeterminate. **Fix the table order before it matters. Owed.**

### The one activation gate that is genuinely missing

`src/evo/specialtx_validation.cpp:715-730` — the V11 **anchor-on-boundary** consensus rule carries
an explicit in-source note that it has **deliberately no activation-height gate**, as a
"resettable-fleet simplification", and that mainnet and any public testnet **require** one before
shipping, or historical off-boundary PTXDKGs become retroactively invalid.

On a fresh genesis this is **inert by construction** — there is no history to invalidate, and every
PTXDKG from block 1 will be on-boundary. **It is safe to launch the testnet without it.** But it
is exactly the h385 shape (a validation change applied without a height gate, retroactively
invalidating an older block), and the testnet is not resettable in the way the dev fleet is. It
joins the E1 mainnet checklist and should be discharged before the chain accumulates history worth
keeping. **Owed, not blocking.**

---

## 6. PTX formation params — all nine, positional

`Consensus::PTXFormationParams`, `src/consensus/params.h:178-257`. **This struct is initialised by
brace aggregate-init with bare positional values at all five existing sites, so a grep for any
field name finds nothing.** The header carries its own warning at `:229-237`. Field order is
load-bearing; the table below is the authority.

Existing sites for reference: `main :375 {"main",1440,1440,1440,1}` · `test :544` (same) ·
`regtest :691 {"regtest",80,80,80,1,200,1,40}` · `ptxtestnet :847 {"ptxtest",80,80,80,1}` ·
`ptxbea :1023 {"ptxbea",30,1440,80,8,200,1,40,900}`.

**Proposed for `ptxnet` — write all nine explicitly, never rely on a default:**

```cpp
consensus.ptxFormation = {"ptxnet", 30, 1440, 80, 8, 200, 1, 40, 0};
```

| # | field | value | reason |
|---|---|---|---|
| 1 | `name` | `"ptxnet"` | ⚠ match `strNetworkID` exactly. `ptxtestnet` sets `"ptxtest"` against a network id of `"ptxtestnet"` — a live mismatch; do not copy it |
| 2 | `nBoundaryInterval` | **30** | formation boundary every 30 blocks ≈ 30 min at 60 s spacing. `ptxbea`'s value, exercised continuously for weeks |
| 3 | `nRotationInterval` | **1440** | ≈ 1 day. The KDD-045 security ceiling; handover-at-accept means rotation is never an availability cost |
| 4 | `nCeremonyBudget` | **80** | ODC-050 stall-out span. Ceremony needs 26 blocks of deadlines (`ptx_ceremony_driver.h:41`), so 80 is ~3× headroom |
| 5 | `nSupportedQuorums` | **8** | ⚠ **constrained, not free — see the two hard gates below** |
| 6 | `nRetireWindow` | **200** | KDD-074 idle-retire arm; `0` disables retirement entirely |
| 7 | `nReformGrace` | **1** | ⚠ **must be > 0 whenever `nSupportedQuorums > 1` or the daemon aborts at startup** |
| 8 | `nReformRateWindow` | **40** | reform pacing; with B=30 gives `stride = 60` |
| 9 | `nReformStatelessHeight` | **0** | **the field this spec was opened on — see §6.1** |

### The startup gate — `PTX_Formation_CheckParams`

`src/ptx/ptx_formation.cpp:106-161`, called unconditionally from `AppInitSanityChecks`
(`src/init.cpp:741`), **aborts the daemon on failure**:

1. `:109-113` — all four of fields 2–5 must be `> 0`.
2. `:117-128` — **hard reject** if `capacity < nSupportedQuorums`, where
   `capacity = nRotationInterval / nBoundaryInterval`. Here `1440/30 = 48 ≥ 8`. ✔
3. `:140-149` — **hard reject** if `nSupportedQuorums > 1 && nReformGrace <= 0` (ODC-054).
   This is why field 7 must be 1, and it is invisible if you copy `main`'s five-value form. ✔
4. `:152-159` — advisory log warning if `capacity < nSupportedQuorums * 2`. Here 48 ≥ 16, quiet. ✔

### 6.1 `nReformStatelessHeight` = 0 — the reason in full

Read at exactly two sites, both in `CPTXQuorumStore::MaybeReformAtBoundary`
(`src/ptx/ptx_quorum_store.cpp:576-752`):

* **`:646`** — `if (pindex->nHeight < params.nReformStatelessHeight)` selects the **legacy
  stored-stamp pacing** path: walk the record snapshot for the maximum `reformed_height` stamp and
  pace from it. Above the boundary, pacing is **stateless**: `stride = ceil(rateW / B) * B` (= 60
  here) and reform fires when `nHeight % stride == 0` — a pure height predicate with no stored state.
* **`:677`** — `if (prevB > 0 && prevB >= params.nReformStatelessHeight)` arms the **BUG-036
  self-heal**, which re-derives the previous stride boundary's selection and re-stamps a
  lost or clobbered record.

`ptxbea` uses **900** for one reason only: its chain was already running when Layer 2 landed, and
900 sat above the deployed height so that blocks below 900 would **replay byte-identically** under
the old stored-stamp rules. That is a migration device for an existing chain.

**On a fresh genesis it is worse than useless.** With 900 you would get 900 blocks (~15 hours) of
legacy stored-stamp pacing on a chain that has no legacy stamps to be compatible with — 900 blocks
governed by the exact mechanism whose clobber-by-`VerifyDB` **partitioned the fleet at h5487**, and
900 blocks during which `:677` holds the self-heal **disarmed**, because `prevB >= 900` is false.
The launch window would run with the known-fragile path active and its repair mechanism switched off.

**`0` gives the correct behaviour on both reads:** `nHeight < 0` is never true, so stateless pacing
applies from block 1; and `prevB >= 0` is always true, so the self-heal is armed at every boundary
from the start. `0` is also the struct's in-class default (`params.h:256`) — but **write it
explicitly**, because a nine-field positional literal that stops at eight is precisely the kind of
silence this document exists to prevent.

`1` would also work and is indistinguishable in behaviour (no boundary occurs at height 0 anyway —
`PTX_Formation_IsBoundary` excludes genesis via its `nHeight > 0` term, `ptx_formation.cpp:701-704`).
**Prefer `0`**: it is the field's own default and reads as "this mechanism was never needed here",
which is the true statement.

---

## 7. The five `CChainParams` PTX fields — **not** in `Consensus::Params`

Declared at `src/chainparams.h:144-148` with in-class defaults, accessors at `:106-122`. These live
on `CChainParams`, so they are missed by anyone auditing `Consensus::Params`. **Their defaults are
consensus-live** — a network that omits them silently gets fee 0, miner-fee 0, and a 1440 window.

| field | value | reason |
|---|---|---|
| `strPTXLotteryPoolAddress` | **`""`** | as `ptxbea` (`:1028`). The accumulator uses the derived burn script, not a named address; `ptxtestnet`'s populated address is legacy |
| `nPTXServiceFee` | **`1 * COIN`** | enforced at `specialtx_validation.cpp:945`, `:1074`. Non-zero is what makes a roll cost something; `0` reopens the free-preview class |
| `nPTXSettlementWindow` | **`60`** | payout every 60 blocks ≈ 1 hour. `ptxbea`'s proven value; enforced at `:1294`, `:1491` and `src/blockassembler.cpp:343` |
| `nPTXSeedHeightWindow` | **`60`** | max staleness of a commitment's seed height before `ptxcommit-seedheight-stale` (`:984-987`). **`0` disables the check entirely** — do not inherit the default |
| `nPTXPayoutMinerFee` | **`10000`** (0.0001 HMS) | enforced at `:1495`, `:1528`, `ptx_payout.cpp:23` |

---

## 8. Consensus scalars

Follow `ptxbea` except where noted. Reasons given for the ones that are a choice rather than a copy.

| param | value | reason |
|---|---|---|
| `nTargetSpacing` | 60 | 1-minute blocks, as every network |
| `nTargetTimespan` / `V2` | 1200 / 1200 | as main |
| `nTimeSlotLength` | **15** | ⚠ also sets P2P clock-skew tolerance: `abs64(nTimeOffset) < 2 * nTimeSlotLength` → disconnect (`src/net_processing.cpp:1507-1513`). 15 gives ±30 s. **Do not copy `ptxtestnet`'s `1`** — a ±2 s window across five internet-separated operators would cause constant disconnects |
| `nCoinbaseMaturity` | 10 | fast iteration; testnet convention |
| `nStakeMinAge` | 0 | testnet convention |
| `nStakeMinDepth` | 20 | as `ptxbea`; keep in mind for funding runbooks — freshly-sent coins are unstakeable for 20 blocks |
| `nGMCollateralAmt` | **100 · COIN** | testnet value. With five operators the collateral must be cheap to source |
| `nGMCollateralMinConf` | 1 | testnet convention |
| `nGMBlockReward` | 3 · COIN | as `ptxbea` |
| `nNewGMBlockReward` | 2.674999995 · COIN | as `ptxbea` (`:906`) — note `ptxbea` deliberately takes the **mainnet** value here while `regtest`/`ptxtestnet` use 6 |
| `nBudgetCycleBlocks` | **720** | as `ptxbea`. ⚠ puts ~14 % of blocks in the superblock window (`nHeight % cycle < 100`, `gamemaster-payments.cpp:205`) vs mainnet's 0.23 % — intentional, exercises the path |
| `nMaxMoneyOut` | 30000000 · COIN | a **per-value** cap, not a supply cap |
| `powLimit` / `posLimitV1` / `posLimitV2` | main's values | as `ptxbea` |
| `fPowAllowMinDifficultyBlocks` | **false** | ⚠ `true` only on regtest. Keep false — min-difficulty blocks would let a single operator race the chain |
| `fPowNoRetargeting` | **false** | same |
| `nProposalEstablishmentTime` | 300 | testnet convention |
| `strSporkPubKey` | **a NEW key** | ⚠ **must not reuse any existing key.** `test` and `ptxtestnet` currently **share** one (`:440` = `:770`); regtest's private half is published in-source at `:599-603`. Generate a fresh pair, keep the private half in the deployment secret store — never in the repo, following `ptxbea`'s `$PTXBEA_SPORK_KEY` precedent |

> ⚠ **Block reward — decide this consciously.** `GetBlockValue` (`src/validation.cpp:852-876`)
> branches on `IsRegTestNet()` and `IsTestnet()` only; **every other network, including a new
> `ptxnet`, falls through to the mainnet schedule**: `nHeight > vUpgrades[UPGRADE_V3_4] ? 5.35 COIN
> : 3800 COIN`. With `V3_4 = 51`, blocks 1–51 mint **3800 HMS each ≈ 193,800 HMS**, then 5.35 HMS
> thereafter. That is the de-facto genesis allocation and it lands in whichever wallet mines the
> first 51 blocks. Either accept it deliberately as the operator float, or add a `ptxnet` branch.
> `GetTotalBudget` derives from it (`budgetmanager.cpp:854-862`), so changing it moves budget too.

### Fields read but never written on ANY network

`Consensus::Params` (`params.h:262-376`) has no in-class initialisers on these, and `CChainParams`
default-initialises the struct (`chainparams.h:130`, ctor `:126`). They are **indeterminate at
runtime on every network today** — pre-existing, not introduced here, but a new network is the
natural moment to set them:

| field | decl | read at |
|---|---|---|
| `strSporkPubKeyOld` | `params.h:290` | `src/spork.cpp:321` |
| `nTime_EnforceNewSporkKey` | `:291` | `src/spork.cpp:149` |
| `nTime_RejectOldSporkKey` | `:292` | `src/spork.cpp:153`, `:275` |
| `nHemisBadBlockTime` | `:300` | `src/validation.cpp:2962` |
| `nHemisBadBlockBits` | `:301` | `src/validation.cpp:2963` |

**Recommendation:** set `strSporkPubKeyOld = strSporkPubKey`, both spork-key times to `0`, and both
bad-block fields to `0` — explicit no-ops. **Owed.**

---

## 9. LLMQ

Register **`LLMQ_TEST` only**, `llmqChainLocks = LLMQ_TEST` — as `regtest`, `ptxtestnet` and
`ptxbea` all do (`:1008-1010`). `llmq50_60` needs 50 gamemasters and `llmq400_*` need 400; a
five-operator testnet cannot form either, and registering an unformable LLMQ yields silent
chainlock failure. `LLMQ_TEST` is size 3 / minSize 2 / threshold 2.

⚠ `LLMQ_TYPE_PTX_CEREMONY = 200` (`params.h:105`) is a TierTwoConnMan map key only and **must never
enter `consensus.llmqs`**.

---

## 10. The gamemaster-count floor — a deployment constraint, not a parameter

`src/ptx/ptx_formation.cpp:92`: fewer than **11** valid gamemasters ⇒ **silent deterministic skip**.
No error, no log line, no quorum. `n = 11` and `t = 6` are open-coded literals with no named
constant and no per-network override — `n` at `ptx_dkg.cpp:125`, `:325`, `ptx_formation.cpp:92`,
`:97`, `specialtx_validation.cpp:666`, `:820`; `t` in **six** copies at `ptx_dkg.cpp:370, 495, 713,
1110, 1365, 1391`, and independently *derived* as `formed_size/2 + 1` at `ptx_quorum_store.cpp:967`
— two mechanisms that agree only by coincidence at n=11.

**Consequence for launch: five operators must run at least 11 gamemasters between them** (e.g. 2–3
each) or the chain will run, stake, and never form a single quorum, with nothing in the log to say
why. This is the single most likely way a correctly-configured testnet still looks broken.
`nSupportedQuorums = 8` additionally wants a pool comfortably above 11 to keep eight quorums
distinct — **budget ~24+ gamemasters** for the eight-quorum configuration to be meaningful, or
lower `nSupportedQuorums` to match the real GM count.

---

## 11. Owed source work before the chain can be cut

1. Register the network — `chainparamsbase.cpp:14-18`, factory `chainparams.cpp:1061-1074`,
   CLI `-ptxnet` in `src/util/system.cpp:865-877`, and `IsTestChain()` in `chainparams.h:81`.
2. Add `CPTXNetParams` with every value in §1–§9 written explicitly.
3. Mine genesis: adapt `findGenesisPTXBea` (`chainparams.cpp:219-228`), run, record nonce, re-comment.
4. Generate `chainparams_seed_ptxnet[]` via `contrib/seeds/generate-seeds.py` at port 29994.
5. Generate a fresh spork keypair; private half to the secret store only.
6. Fix the `NetworkUpgradeInfo` 13/14 ordering (`upgrades.cpp:15-77`).
7. Gate `ptx_debug_setnodefailmode` (`rpc/ptx.cpp:606`) to regtest+dev.
8. Set the five never-written `Consensus::Params` fields.
9. Decide the block-reward question in §8.
10. **Then** re-run the fresh-node cold-sync deploy gate — the standing rule from h385: every
    consensus change is proven by a fresh node syncing the chain from empty, not by "reindex clean".

Items 1–5 are required to launch. 6–9 are correctness debt that a new network is the cheapest
moment to pay. Item 10 is not optional.

---

## 12. Explicitly unchanged

`pszTimestamp`, `genesisOutputScript`, merkle root, `nBits`, `ZC_Modulus`, all zerocoin scalars, the
`PTX_*` hardcoded constants (quorum n=11/t=6, ceremony deadlines, PoSe weights, DKG transport sizing,
fan-out budget), `PROTOCOL_VERSION = 70928`, and the special-tx type numbers (PTX=6, PTXCOALESCE=9,
PTXPAYOUT=10, PTXDKG=11, PTXROLLCOMMIT=12). None of these are per-network today and none should
become so for this launch.

⚠ Noted, not actioned: `LOTTERY_ACCUM_SCRIPT` (`ptx_accum_script.cpp:16-33`) and the
winner-selection domain `"PTX-LOTTERY-PAYOUT-"` (`ptx_winner_selection.cpp:20`) carry **no chain
tag**, so the identical accumulator burn address exists on every network. Harmless while chains are
separated by magic and genesis, but it means a cross-chain replay of a payout-domain hash is not
domain-separated. Registered for the mainnet checklist.
