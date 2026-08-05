# Inherited chain parameters — stated vs coded vs measured

**Purpose.** The authoritative answer to "what are this chain's economics, and how do you
know". Written to be handed to an exchange, auditor, or integrator **as-is**. Every row
carries provenance: either a source `file:line` or a measurement with its instrument.

**Measurement provenance.** All measured figures come from a **mainnet observer node**
synced to **h1,293,515** on **2026-08-06**, running release **1.3.1**, isolated from all
PTX networks, read-only (no wallet, no staking, no key import). Measurements are
`getblockcount`, `gettxoutsetinfo`, and per-block net mint computed as
`Σ vout − Σ vin(prevout)` over sampled blocks with `-txindex=1`.

**Source provenance.** Line numbers are against the `feature/ptx-dkg` tree at the time of
writing. Where a constant is network-dependent, the **mainnet** value is given.

---

## 1. Headline figures

| Quantity | Value | Basis |
|---|---|---|
| Height at measurement | **1,293,515** | measured (`getblockcount`) |
| Circulating supply | **8,650,194.876 HMS** | measured (`gettxoutsetinfo.total_amount`) |
| Launch (block 1) | **2024-01-15 12:35 UTC** | measured (block timestamp) |
| Chain age | **2.56 years** (933 days) | derived from block 1 / tip timestamps |
| Nominal block reward | **5.35 HMS** | source, `validation.cpp:868` |
| **Effective block reward** | **4.815 nominal / 4.825542 measured avg** | measured, 1,200-block pooled sample |
| Effective annual emission | **~2.44M HMS/yr** | derived: 4.825542 × 506,821 blocks/yr |
| Mean block spacing | **62.27s** (target 60s) | measured over 1k/10k/100k windows |
| **Supply cap** | **NONE ENFORCED** | source — see §5 |

★ **The single most consequential line:** the coded block reward (5.35) and the actual
emission (4.8255) differ by **~9.8%**, silently, and have since **h703,414
(2025-06-06)**. Any figure quoting 5.35 as the block reward overstates emission.

---

## 2. Emission schedule — stated vs coded vs measured

`GetBlockValue` (`validation.cpp:849-871`) — note an **earlier identical copy is commented
out at `:828-847`**; the live function is the second one.

| Height range | Coded | Measured | Agreement |
|---|---|---|---|
| 0 (genesis) | — | **0.00000000** | ✅ genesis mints nothing |
| 1 – 500 | 3800 | **3800.00000000** | ✅ exact |
| 501 – 703,413 | 5.35 | **5.34999999** | ✅ (1 sat rounding) |
| **703,414 – tip** | **5.35** | **4.81500000** nominal | ❌ **−10%** |

The boundary at 500 is not a literal — it is
`vUpgrades[UPGRADE_V3_4].nActivationHeight` (`chainparams.cpp:305`), which is 500 on
mainnet. `UPGRADE_V5_5` = 505 (`:310`).

### 2.1 The h703,414 step

Located by bisection and confirmed on a tight window (h703,410–413 mint 5.34999999;
h703,414 onward mint 4.815). **4.815 = 5.35 × 0.90.**

Mechanism: `SubtractGmPaymentFromCoinstake` (`gamemaster-payments.cpp:353-364`):

```cpp
CAmount nSubsidy = GetBlockValue(nHeight);
nSubsidy *= 0.10;
txCoinstake.vout[1].nValue -= gamemasterPayment + nSubsidy;   // 10% removed, paid to NO ONE
```

The 10% is deducted from the staker's output and **not credited to any recipient** — it is
a **burn**. `GetBlockValue` is unchanged; the reduction is in the block assembler.

### 2.2 Why consensus does not reject it

`IsBlockValueValid` (`gamemaster-payments.cpp:199-226`) ends:

```cpp
return nMinted <= nExpectedValue;      // :226 — a CEILING, not equality
```

Under-minting is therefore **legal and silent**. The error text at `validation.cpp:1673`
("reward pays too much") only fires on *over*-mint. Nothing in consensus observes or
reports a block that mints less than schedule.

### 2.3 The deduction is conditional

The 10% fires only when `stakerOuts == 2`. A **split coinstake** (`stakerOuts > 2`) takes
the `else` branch, which deducts only the gamemaster payment — **no 10%** — so such blocks
mint the full 5.35. Measured incidence of full-reward blocks:

| window | @5.35 | @4.815 | other (fee burn) | window avg |
|---|---|---|---|---|
| h705,000 | 1.33% | 98.67% | — | 4.822133 |
| h950,000 | 1.33% | 98.67% | — | 4.822133 |
| h1,150,000 | 4.00% | 94.00% | 2.00% | 4.836398 |
| h1,250,000 | 4.67% | 92.67% | 2.67% | 4.839903 |
| **pooled (1,200 blocks)** | | | | **4.825542** |

The split-coinstake opt-out is **real, rarely used, and slowly rising** (1.33% → 4.67%).

### 2.4 Fee destruction — a third channel

`validation.cpp:1670` states it plainly: *"PoW phase redistributed fees to miner. PoS stage
destroys fees."* `nExpectedMint` adds `nFees` only for non-PoS blocks (`:1671-1672`).
Measured: blocks with transactions mint slightly **below** 4.815 (observed 4.8142–4.8149),
the shortfall being the destroyed fees. Source and measurement agree independently.

---

## 3. Budget / superblock minting

Superblocks mint **additional** coin on top of the schedule (`IsBlockValueValid` adds
`nBudgetAmt`, `gamemaster-payments.cpp:209/217`).

- `nBudgetCycleBlocks` = **43,200** (`chainparams.cpp:248`) — approx. 1 cycle / 30 days
- `SPORK_13_ENABLE_SUPERBLOCKS` = **1 (ACTIVE)** — measured via `spork show`
- Payout window: `nHeight % nBudgetCycleBlocks < 100` (`gamemaster-payments.cpp:205`)

★ **Payouts occur at offsets 0 AND 1, not only exact multiples.** A scan restricted to
exact multiples of 43,200 misses the largest payouts — in this dataset it missed
**84,113.67 of 143,230.02 (59%)**.

**Measured: 15 budget payouts totalling 143,230.02 HMS**, first at h734,400.

| height | budget minted |
|---|---|
| 734,400 | 15,000.535 |
| 777,600 / 820,800 / 864,000 / 907,200 / 950,400 / 993,600 / 1,036,800 | 4,000.535 each |
| 950,401 / 993,601 / 1,036,801 | 18,000.535 each |
| 1,123,200 | 8,000.535 |
| 1,123,201 | 15,111.535 |
| 1,252,800 | 8,111.535 |
| 1,252,801 | 15,000.535 |

Cycles at h1,080,000, h1,166,400 and h1,209,600 paid **nothing** — proposals do not pass
every cycle.

### 3.1 Burn vs budget — net position

```
withheld by the 10% burn   = 590,102 blocks × (5.34999999 − 4.825542)
                           = 590,102 × 0.52445799        =  309,483.4 HMS
minted by DAO superblocks                                =  143,230.0 HMS
                                                    net  = −166,253.4 HMS  (DEFLATIONARY)
budget utilisation of the burn pool                      =  46.3%
```

**At current proposal throughput the chain is net deflationary relative to its own coded
schedule.** The burn is not an accident to be reverted — it is the funding offset for DAO
minting. Roughly half of it is currently used.

---

## 4. Supply reconciliation (the audit that closes)

| component | amount |
|---|---|
| h1–500 @ 3800 | 1,900,000.000 |
| h501–703,413 @ 5.34999999 | 3,760,584.543 |
| h703,414–1,293,515 @ 4.825542 (measured avg) | 2,847,562.60 |
| budget mints (15 blocks) | 143,230.02 |
| **model total** | **8,651,377.16** |
| **measured supply** | **8,650,194.88** |
| **residual** | **+1,182.29 (0.0137%)** |

The residual lies inside the sampling error of the post-transition average (1,200 of
590,102 blocks sampled; ±0.002 on the mean = ±1,180 HMS). **The schedule, the burn, the
fee destruction and the budget minting together account for the measured supply.**

★ Naive arithmetic using the *coded* 5.35 gives 8,817,630.25 — an **overshoot of
167,435.37**. A negative residual cannot be explained by superblocks (which only add), so
anyone reconciling this chain with 5.35 will get a contradiction they cannot close.

---

## 5. There is no supply cap — the `nMaxMoneyOut` clarification

`consensus.nMaxMoneyOut = 30000000 * COIN` (`chainparams.cpp:253`) is **not a supply cap**
and is never compared against circulating supply.

Its only consumer is:

```cpp
bool MoneyRange(const CAmount& nValue) const
    { return (nValue >= 0 && nValue <= nMaxMoneyOut); }      // consensus/params.h:296
```

applied **per value** — to individual transaction output totals (`tx_verify.cpp:99`), fee
sanity checks (`validation.cpp:286/297`), and input sums (`:1102/1118`). It is a
per-transaction overflow guard inherited from Bitcoin's `MAX_MONEY`, nothing more.

★ **The ~30M figure is a legacy 10-year projection; no cutoff exists in time or supply.**
That reading is not a guess — it reproduces almost exactly from the nominal parameters:

```
h1–500 premine-era                          =  1,900,000
10 years × 525,960 blocks/yr (60s) × 5.35   = 28,138,860
                                              ----------
                                              30,038,860   ≈ 30,000,000  (0.13%)
```

So `nMaxMoneyOut` was almost certainly set to "supply after ten years at the nominal
reward and target spacing", then reused as a per-value overflow bound. Both of its
inputs have since drifted — the reward is 4.8255 not 5.35, and spacing is 62.27s not 60s
— which is why the real 30M date is ~11.3 years from launch (§5.1), and why the number
should never be quoted as a cap or as a date.

**Consequences, stated plainly:**

1. Emission does not stop at 30,000,000. No code path halts or reduces the reward at any
   supply level. `GetBlockValue` is a function of **height only**, with no terminal branch.
2. Any published "total supply: 30,000,000" is a **projection of the schedule**, not a
   consensus-enforced limit.
3. Crossing 10,000,000 or 30,000,000 is a **documentation event, not a chain event** —
   nothing observes it.

### 5.1 Projection to 30M (not a cap — a schedule projection)

```
remaining          = 30,000,000 − 8,650,194.876 = 21,349,805 HMS
blocks/year        = (86400 / 62.27) × 365.25   = 506,821
baseline           = 21,349,805 / 4.825542      = 4,424,333 blocks = 8.73 yr from now
                                                => ~2035, i.e. 11.3 years from launch
with budget minting (+~0.27/blk)                =  8.27 yr from now = ~10.8 yr from launch
```

**A "linear emission over 10 years" claim is measurably short** — the measured rate
reaches 30M in **10.8–11.3 years from launch (2024-01-15)**, not 10. The gap is caused by
the 10% burn (which slows emission) and by 62.27s actual spacing against a 60s target.

---

## 6. Other inherited parameters

| Parameter | Value | Source |
|---|---|---|
| `nTargetSpacing` | 60s (measured 62.27s) | `chainparams.cpp:263` |
| `nTimeSlotLength` | 15s | `chainparams.cpp:264` |
| `nNewGMBlockReward` | 2.674999995 HMS | `chainparams.cpp:256` |
| `nBudgetCycleBlocks` | 43,200 | `chainparams.cpp:248` |
| `UPGRADE_V3_4` | h500 (reward schedule boundary) | `chainparams.cpp:305` |
| `UPGRADE_V5_5` | h505 | `chainparams.cpp:310` |
| `DEFAULT_MAX_REORG_DEPTH` | 100 (runtime `-maxreorg`) | `consensus/consensus.h:35`, enforced `validation.cpp:2960` |
| Fee handling (PoS) | destroyed | `validation.cpp:1670` |
| Mint check | `nMinted <= nExpectedValue` (ceiling) | `gamemaster-payments.cpp:226` |

---

## 7. Known limits of this audit

Stated so a reader does not over-trust it:

1. **The post-transition average is sampled, not exhaustive** — 1,200 of 590,102 blocks.
   It carries ±0.002 (±~1,180 HMS on the total). Every figure derived from it inherits
   that. An exhaustive sum would remove it.
2. **The budget scan covers cycle windows only** (`offset 0–99`, 2,999 blocks), using a
   detector that flags coinstake outputs ≥ 500 HMS and then computes exact mint for
   candidates. A budget payout **below 500 HMS would be missed**. All 15 observed payouts
   were ≥ 4,000, so the threshold is comfortable but not proven safe.
3. **Historical fork/tie rate is not measurable from a synced node** and is deliberately
   absent from this document. A node that reaches tip by IBD downloads one chain and
   never observes competing blocks; `getchaintips` on this observer returns **1 tip, 0
   forks**. Measuring it requires multiple nodes observing live over weeks.
4. **Figures are a snapshot at h1,293,515.** Supply, height and the projections move.
