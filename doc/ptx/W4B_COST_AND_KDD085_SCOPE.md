# W4-b cost, and KDD-085 sign-over-P2P scope

**Date:** 2026-08-24 · §9 recon 2026-08-25 · landing record §9.13, 2026-08-31
**Gate:** RECON → PLAN. ★ **One increment has since landed from §9** — the
`PTX_RollCommitmentPresent` index (`bfea163`, built green on px1). Everything else in this
document remains unbuilt. **§9.13 is the authority on what exists.**
**Measurement host:** px1, AMD Ryzen 5 2600 (12 threads), `-O2`, blst assembly path
**Chain data:** live ptx-w2r fleet, read-only, tip 11,747

---

## 0. ★★ The premise correction, up front

The brief states the gap as: *"`PTX_MapBeacon` appears in `rpc/ptx.cpp` only (`:406`, `:1273`),
never in `validation.cpp`; `quorum_sig` appears in `validation.cpp` zero times. So any staker can
stamp an arbitrary `quorum_hash` and arbitrary results into a PTXSESS and it validates."*

**Both greps are true and the conclusion drawn from them is not.** PTX special-transaction
validation does not live in `validation.cpp`; it lives in `evo/specialtx_validation.cpp`, reached
via `ProcessSpecialTxsInBlock` → `CheckSpecialTx`. And there:

> `src/evo/specialtx_validation.cpp:1090-1114` — **the W4-b signature verify is already built and
> already shipping.**

```cpp
if (pindexPrev != nullptr) {
    CPTXQuorumRecord qrec;
    if (ptxQuorumStore == nullptr ||
        !ptxQuorumStore->GetQuorumRecord(payload.quorum_hash, qrec)) {
        return state.Invalid(false, REJECT_INVALID, "ptx-unknown-quorum");
    }
    if (payload.quorum_sig.size() != (size_t)PTX_SIG_BYTES ||
        qrec.group_pk_bytes.size() != 48 ||
        !PTX_BLS_Verify(qrec.group_pk_bytes.data(), payload.round_seed,
                        payload.quorum_sig.data())) {
        return state.Invalid(false, REJECT_INVALID, "ptx-bad-quorum-sig");
    }
}
```

Landed `81fcf26` (2026-07-27), *"W2.4 W4-b: consensus verification of the roll threshold signature
(the trust half of the a+b pair)"*. It is an ancestor of `HEAD` **and of the released commit
`b637751`** (`git merge-base --is-ancestor` → YES for both). **The pairing is in the binary
operators are running today.**

So the question "should we add a BLS pairing to consensus?" is not open. It was answered four
weeks ago, and the cost the brief asks us to establish before building is a cost the chain is
**already paying**. That reframes every number below from *projected* to *measured-in-place*.

### 0.1 What IS still open — and it is not a pairing

Three derivations remain unchecked by consensus, verified by grep at `HEAD`:

| Property | Checked by consensus? | Evidence |
|---|---|---|
| `quorum_sig` verifies under the named quorum's `group_pk` | **YES** | `specialtx_validation.cpp:1108-1111` |
| `quorum_hash` names a real, ACTIVE-at-`nSeedHeight` record | **YES** | `:1099-1103`, `:975-980` |
| `beacon == SHA256(quorum_sig)` | **NO** | `PTX_BLS_SigToBeacon` has exactly one non-test caller: `rpc/ptx.cpp:382` |
| `results == PTX_MapBeacon(beacon, …)` | **NO** | `PTX_MapBeacon` count in `src/evo/` + `validation.cpp` = **0** |
| `round_seed` re-derives from the payload's own inputs | **NO** | and see BUG-051 — the documented formula does not match the code |

**The remaining gap is therefore SHA-256 and an integer mapping loop, not a pairing.** The
randomness is already proven authentic; what is not proven is that the *published outcome is the
one that randomness produces*. A staker cannot forge the beacon's provenance, but it can publish
`results: [7]` next to a signature that maps to `[26]`, and every node accepts it.

That is still worth closing — it is the whole reason an independent verifier exists — but it is
**cheap**: one SHA-256 over 96 bytes plus one `PTX_MapBeacon` call, both already written and both
already exercised by `testnet/explorer/ptx_verify.py`. It needs a costing paragraph, not a costing
exercise.

### 0.2 The documentation defect that produced the wrong premise

Two shipped artefacts assert the gap is open:

- `src/primitives/transaction.h:540-542` — *"NOT a trustworthy trigger input until the consensus
  `quorum_sig` verify lands (W4-b): until then any staker can stamp an arbitrary quorum_hash."*
- `testnet/explorer/ptx_verify.py:3-11` — the module docstring, which cites the same two greps and
  concludes *"until W4-b, a node will happily connect a block whose PTXSESS carries results that do
  not follow from its beacon, or a beacon that does not follow from its signature."*

The explorer's sentence is **half right and half stale**: the beacon/results half is accurate, the
`quorum_sig` half is not. `transaction.h`'s is simply out of date — `81fcf26` added the check and
did not update the comment that predicted it. Registered as **ODC-081**.

---

## 1. Verification cost — measured, not quoted

`bench_verify.cpp` replicates `PTX_BLS_Verify` (`ptx/ptx_bls.cpp:788-807`) exactly: same
`blst_core_verify_pk_in_g1`, same DST `BLS_SIG_HEMIS_PTX_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_`,
same 48-byte G1 group key and 96-byte G2 signature.

★ **Anti-vacuity:** the inputs are not synthetic. They are a real on-chain triple — the PTXSESS at
height 10,496 (`3fcf1293…`), its quorum record `141d9bdf…` (`formed_size 11`), and that record's
`group_pk 816d345d…` — and the benchmark **returns true** on them. It is measuring the consensus
path, not a lookalike of it.

| Case | Cost | Notes |
|---|---:|---|
| verify **OK** (real chain triple) | **2,395.6 µs** | the steady-state cost |
| verify FAIL — wrong message, real signature point | 2,385.4 µs | full pairing, then false |
| verify FAIL — **forged but well-formed G2 point** | **2,490.5 µs** | full pairing, then false |
| verify FAIL — random garbage bytes | 27.1 µs | fails `blst_p2_uncompress` |
| `blst_p1_uncompress` + `blst_p2_uncompress` only | 82.1 µs | parse, no pairing |

**~2.4 ms, not ~1–2 ms.** The general BLS12-381 figure understates this build by roughly 2×. Two
contributing facts, both source-confirmed:

- The build is **not** the pure-C path. `src/Makefile.am:113-116` compiles `blst/build/assembly.S`
  and explicitly forbids `__BLST_NO_ASM__` (*"pure-C path overflows RPC thread stack"*). So 2.4 ms
  is already the fast path, not a portable fallback.
- The measurement host is a 2018-era Ryzen 5 2600. A modern VPS core will do better; a small one
  will not. **Operators are the relevant population and they are not on this box** — treat 2.4 ms
  as a mid-range figure, not a ceiling.

### 1.1 ★ Flat in quorum size — confirmed, and structurally so

Confirmed in this implementation. `PTX_BLS_Verify` takes `const uint8_t group_pk_bytes[48]` and
`const uint8_t sig[96]` — one aggregate G1 point and one G2 point. `specialtx_validation.cpp:1109`
enforces `qrec.group_pk_bytes.size() != 48` as a hard reject, so the record can only ever hold a
single group key regardless of `formed_size`. An 11-member and a 30-member quorum present
byte-identical inputs to the same one-pairing call.

The signing threshold `t` affects **recovery** on the coordinator, not verification on the
validator. Quorum growth is free at the consensus layer. That is the good news in this document.

---

## 2. Worst case per block

### 2.1 ★ What actually caps PTXSESS per block: nothing counts them

Searched and confirmed: there is **no `MAX_PTX_COUNT`-style per-block bound on PTXSESS**.
`CheckPTXCoalesceBlockRules` caps PTXCOALESCE at one per block and `CheckPTXDKGBlockRules` caps
PTXDKG at one per block; `CheckPTXRollCommitSettlePairing`
(`evo/specialtx_validation.cpp:1407-1438`) iterates every PTXSESS and bounds **pairing**, not
count. `MAX_SPECIALTX_EXTRAPAYLOAD = 10000` (`specialtx_validation.h:23`) is a *per-transaction*
payload bound, not a block bound.

**The only ceiling is block space:**

- `MAX_BLOCK_SIZE_CURRENT = 2000000` — consensus (`consensus/consensus.h:14`)
- `DEFAULT_BLOCK_MAX_SIZE = 750000` — the assembler's default policy (`policy/policy.h:21`),
  operator-overridable with `-blockmaxsize`, and **not** a bound on what a validator must accept

Measured transaction sizes, from 9,987 real PTXSESS across the whole chain:

- PTXSESS mean **826.8 bytes**
- PTXROLLCOMMIT mean **558.4 bytes** (min 516, max 2,896)
- a same-block commit+settle pair therefore ≈ **1,385 bytes**

Since BUG-034 Phase 2, a settle may pair with a **confirmed** commitment at any depth, not only a
same-block sibling. So the adversarial shape is a block of *settles alone*:

| Shape | PTXSESS/block | Pairings | Serial cost | % of the 60 s block interval |
|---|---:|---:|---:|---:|
| Observed maximum, real chain | 68 | 68 | **163 ms** | 0.27 % |
| Policy default (750 kB), settles only | 907 | 907 | **2.17 s** | 3.6 % |
| Consensus max (2 MB), same-block pairs | 1,443 | 1,443 | **3.46 s** | 5.8 % |
| **Consensus max (2 MB), settles only** | **2,419** | **2,419** | **5.80 s** | **9.7 %** |

`nTargetSpacing = 60` on every network (`chainparams.cpp:263/435/595/801/1069`).

**5.8 seconds of pairing inside `ConnectBlock`, holding `cs_main`, during which the node does
nothing else.** The brief's "~2 s at 1,000 PTXSESS" was the right order of magnitude and slightly
optimistic: 1,000 PTXSESS is 2.40 s at the measured rate.

The economic bound is real but weaker than it looks: 2,419 settles need 2,419 commitments, each
paying `nPTXServiceFee` (1 HMS on ptxbea). That prices a *sustained* attack but not a burst, and it
is a caller-side cost, not a validator-side one — **the validator pays the 5.8 s whether the
attacker paid or not**, because the fee is only known after the block is assembled.

---

## 3. IBD cost

**Chain census** (read-only, full chain, 6 parallel shards over 6 callers):

- blocks: **11,747**
- PTXSESS: **9,987**
- PTXROLLCOMMIT: **10,533**
- max PTXSESS in one block: **68**

**9,987 × 2.3956 ms = 23.9 seconds of BLS pairing to sync this chain from block 0.**

Against the harness's reference of 10,503 blocks in 128 s: pro-rata that window holds ~8,929
PTXSESS = **21.4 s, or ~17 % of total IBD wall-clock**, already, today, on a chain 11.7 k blocks
old and at a roll density (0.85 PTXSESS/block lifetime average) far below what a busy chain would
carry.

★ **This figure is computed from two measured quantities — the per-verify cost and the on-chain
count — not from an end-to-end A/B of two binaries.** It is stated that way deliberately; an
instrumented IBD comparison is the honest way to confirm it and is owed before anyone quotes 17 %
as a benchmark result.

### 3.1 ★★ The pairing is NOT skipped below checkpoints, and script checks ARE

`validation.cpp:1606`:

```cpp
bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();
```

`fScriptChecks` gates `CCheckQueueControl` (`:1614`) and every `CheckInputs` call. But
`ProcessSpecialTxsInBlock` is passed **`fJustCheck`, not `fScriptChecks`** (`validation.cpp:1779`),
and `CheckSpecialTx` has no checkpoint arm at all.

**So on the stretch of IBD where Bitcoin-derived nodes skip signature verification entirely, this
node still performs one BLS pairing per PTXSESS.** As roll volume and chain length grow, the
pairing becomes a *larger* fraction of IBD over time, not a smaller one — the opposite of the
script-check curve. This is the single most important structural finding in this section.

---

## 4. The three standard mitigations — availability, checked in the tree

### A. Validation cache — **AVAILABLE, and it is the one that matters**

Today the pairing runs **twice** per PTXSESS in the steady state, and the source says so:

`evo/specialtx_validation.cpp:1004-1006` documents its own three call shapes —
- `pindexPrev = nullptr` → `CheckBlock` → `CheckSpecialTxNoContext` (**pairing skipped**)
- `pindexPrev = chainActive.Tip()` → `AcceptToMemoryPoolWorker` (`validation.cpp:509`) → **pairing**
- `pindexPrev = pindex->pprev` → `ConnectBlock` → `ProcessSpecialTxsInBlock` (`:1639`) → **pairing**

A transaction accepted into the mempool and then mined is verified at acceptance and verified again
at connect. There is no cache between them.

**The infrastructure to fix this is already in the tree and is the exact precedent named in the
brief:** `src/script/sigcache.{h,cpp}` — `CSignatureCache`, `InitSignatureCache()` (called at
`init.cpp:1238`), and `CachingTransactionSignatureChecker`, which is how ECDSA signatures avoid
exactly this double-verify. A BLS analogue keyed on `SHA256(quorum_sig ‖ round_seed ‖ group_pk)` is
a direct port of a pattern this codebase already runs, not new machinery.

**Effect:** steady-state cost → ~0 for anything that transited the mempool. IBD is untouched (no
mempool to have cached from), so §3 remains the residual.

### B. Batch verification — **AVAILABLE in blst, with one structural caveat**

`src/blst/bindings/blst.h:363-437` exposes the full pairing-accumulator API:
`blst_pairing_init`, `blst_pairing_chk_n_mul_n_aggr_pk_in_g1` (the randomised batch form),
`blst_pairing_commit`, `blst_pairing_merge`, `blst_pairing_finalverify`.

**The caveat is where it fits, not whether it exists.** A batch yields one all-or-nothing verdict:

- **`ConnectBlock` — fits.** If any signature in the block is bad, the *block* is invalid; the
  validator never needs to name which transaction did it. A whole block's PTXSESS can go into one
  accumulator. This is where the 5.8 s of §2 lives, and batching should take it to well under 1 s.
- **`AcceptToMemoryPoolWorker` — does not fit.** ATMP judges one transaction at a time and must
  return a per-transaction verdict. No batching is possible there. This is exactly the site
  mitigation A covers, so A and B are complements, not alternatives.

### C. Parallel verification — **AVAILABLE**

`CCheckQueue` (`src/checkqueue.h`), `scriptcheckqueue` (`validation.cpp:1508`), the worker threads
(`init.cpp:1241-1242`) and `-par`/`nScriptCheckThreads` all exist and are already used for script
checks. A `CBLSCheck` alongside `CScriptCheck` on the same queue is a small change.

**But note the interaction:** parallel verification of a batch is not additive with B, and B is
cheaper. If B lands, C buys much less. Recommended order is **A, then B, then C only if measurement
still demands it.**

---

## 5. ★★ The DoS question — and the answer is worse than "ordering"

**Is an invalid `quorum_sig` verified before it is rejected? Yes — and there is a route to it that
costs the attacker essentially nothing.**

### 5.1 The intended check order, as built

Within `CheckSpecialTx`'s `PTX` arm the cheap checks *do* come first, and correctly so
(`specialtx_validation.cpp:1056-1114`):

1. `GetTxPayload` deserialize → `ptx-bad-payload`
2. `low > high` → `ptx-bad-range`
3. `count == 0` → `ptx-zero-count`
4. `results.size() != count` → `ptx-result-count-mismatch`
5. every result within `[low, high]` → `ptx-result-out-of-range`
6. `quorum_sig_hash.IsNull()` → `ptx-missing-sig`
7. `nSeedHeight == 0` → `ptx-bad-height`
8. redundant-accum-fee scan over `tx.vout` → `ptxsess-redundant-fee`
9. **store lookup** on `quorum_hash` → `ptx-unknown-quorum`
10. length checks on `quorum_sig` (96) and `group_pk` (48)
11. **the pairing** → `ptx-bad-quorum-sig`

Step 9 is the load-bearing one: a *random* `quorum_hash` never reaches the pairing. And a garbage
96-byte blob costs 27 µs, not 2.4 ms, because it fails decompression. **On those two axes the
ordering is right and should be left alone.**

### 5.2 Where it breaks

An attacker who names a **real, ACTIVE quorum_hash** — public information, `ptx_quorum_list`
returns it — and supplies a **well-formed but wrong G2 point** pays the full **2,490 µs**
(measured, row 3 of §1). Producing such a point is one hash-to-curve: microseconds.

Now the part that makes it exploitable rather than merely asymmetric. At mempool acceptance
(`validation.cpp:509`) `CheckSpecialTx` runs:

- **after** the duplicate check and *"do all inputs exist?"* (`:492-503`)
- **before** `view.GetValueIn` and the fee checks (`:561-567`)
- **before** `CheckInputs` — i.e. **before any input script is verified** (`:599`, `:615`)

**So the transaction's inputs must merely *exist*; they need not be spendable by the attacker.** A
PTXSESS referencing any known UTXO with a junk `scriptSig` reaches the pairing and is rejected at
step 11 — before the node ever discovers the attacker could not spend that coin and paid no fee.

Three multipliers:

1. **`AssertLockHeld(cs_main)`** (`validation.cpp:648`) — ATMP runs under `cs_main`, and
   `LOCK(pool.cs)` is taken at `:476`, *before* the `CheckSpecialTx` call at `:509`. **The pairing
   is performed holding both `cs_main` and `mempool.cs`.** This is not merely CPU burn; it is
   validation-thread head-of-line blocking.
2. **No misbehaviour score.** `state.Invalid(...)` is `DoS(0, ...)` by definition
   (`consensus/validation.h:55-61`). `ptx-bad-quorum-sig` therefore carries **DoS score 0** — the
   peer is never penalised and never banned, however many it sends. Compare the neighbouring PTX
   rules, which use `state.DoS(100, …)`.
3. **`recentRejects` does not help.** It is a rolling bloom over rejected *txids*
   (`net_processing.cpp:112`, `:828`); flipping one payload byte yields a new txid.

**Arithmetic:** ~2.49 ms per rejected transaction means one validation thread absorbs ~400/s. At
~830 bytes each that is **~2.7 Mbit/s of upstream to saturate a node's `cs_main`-holding validation
path**, from an unauthenticated peer, with no ban and no cost.

**This is a present-day exposure in shipping code, not a property of a change under
consideration.** Registered as **BUG-052**. It is derived from source and from the measured
2,490 µs; it has **not** been demonstrated against a live node, and that demonstration is owed
before the severity is treated as settled.

### 5.3 The recommended check order

The fix is not a re-ordering of §5.1 — that order is already correct. It is to move the pairing
**behind the checks that make the sender pay**:

1. Keep steps 1–10 exactly as they are.
2. **At ATMP only:** defer the pairing until after `CheckInputs` and the fee checks, so a
   transaction must be genuinely fundable and correctly signed at the script layer before it can
   buy 2.4 ms of pairing. (`CheckSpecialTx` would grow an explicit "structural vs contextual vs
   expensive" split, or ATMP would call the expensive arm separately after `:615`.)
3. Give `ptx-bad-quorum-sig` a **non-zero DoS score**. A well-formed PTXSESS naming a live quorum
   with a signature that does not verify is not an honest mistake; it cannot be produced by a
   correct implementation.
4. Then mitigation A (cache), so the deferred pairing is not repeated at connect.

★ Note what step 2 must **not** do: it must not weaken `ConnectBlock`. There the pairing has to
happen regardless of ordering, because a block's transactions are not individually rejectable.
§4.B (batch) is the answer there, not ordering.

---

## 5.4 ★★ BUG-052 DEMONSTRATED LIVE (2026-08-25) — the ordering, and the cost

The §5.2 argument was source- and arithmetic-derived. It is now demonstrated on a node that can be
saturated and restored: a copy of the fleet's append-only `blocks/` reindexed on px1 in isolation
(`-connect=0 -listen=0`), release build, RPC + `sendrawtransaction`.

**Method.** Lift a real confirmed PTXSESS's raw bytes; repoint its single input at a currently
**unspent** coin (so *"do all inputs exist?"* passes) — which invalidates its `scriptSig`, giving
exactly the junk-scriptSig the attack needs; keep every PTX field byte-valid. Three variants, each
submitted with a per-copy `scriptSig` byte flipped so the txid differs (which also defeats
`recentRejects`):

| variant | quorum_hash | quorum_sig | where it must reject |
|---|---|---|---|
| `badqh` | random (no record) | a valid G2 point | store lookup — **before any pairing** |
| `garbage` | real, ACTIVE | 96 bytes `0xAB` | inside `PTX_BLS_Verify`, at `blst_p2_uncompress` |
| `forge` | real, ACTIVE | a valid G2 point lifted from a **different** roll | the **full pairing** |

**Result 1 — the ordering, proven by the reject reason (this is the decisive part, not the timing).**
All three inputs are unspendable by the sender (junk scriptSig). `forge` and `garbage` both come back:

```
error code: -26
ptx-bad-quorum-sig
```

`ptx-bad-quorum-sig` is the pairing's own rejection (`specialtx_validation.cpp:1111`). **A transaction
whose script was never going to verify is rejected by the BLS pairing — which can only happen if the
pairing runs before `CheckInputs`.** If the script check ran first, the reject would be a script
error. The reason code alone establishes the ordering the finding claims; `badqh` returns
`ptx-unknown-quorum`, confirming the cheap store lookup still (correctly) precedes the pairing.

**Result 2 — the cost, isolated by interleaved paired deltas** (forge and badqh alternating, N=120,
which cancels host drift):

```
forge - badqh : median +1.74 ms   trimmed-mean +1.76 ms   (badqh = reject with NO pairing)
forge - forge : median +0.01 ms   trimmed-mean -0.01 ms   (noise floor / control)
```

The pairing adds **~1.75 ms per rejected transaction inside ATMP**, resolved cleanly above a ~0.01 ms
control floor. It is lower than the §1 microbench's 2.40 ms because `badqh` already pays the cheap
checks and a store-lookup miss; the *marginal* cost of reaching the pairing is what an attacker adds
per transaction, and it is unambiguously non-zero. (The ~52 ms absolute per-call figure is
docker-exec + RPC overhead, not in-daemon cost — which is why the paired delta, not the absolute, is
the measurement.)

**Saturation, with the measured in-situ figure:** ~1.75 ms of `cs_main`+`mempool.cs`-holding work per
attack transaction ⇒ **~570 tx/s saturates one validation thread**; at ~827 bytes/tx that is
**~3.8 Mbit/s** from an unauthenticated, unbanned peer. Same order as the §5.2 estimate.

**What is NOT shown here:** the ban behaviour, because the isolated node has no peers. That
`ptx-bad-quorum-sig` carries DoS score 0 is definitional (`state.Invalid` ⇒ `DoS(0,…)`,
`consensus/validation.h:55-61`), not a measurement, and stands on source.

## 5.5 The minimal fix, its trade-offs, and whether it rides the recut

**The fix is two changes that compose, and the composition is the point.**

1. **Defer the pairing past `CheckInputs`, at ATMP only.** Today `CheckSpecialTx` runs at
   `validation.cpp:510` and does the pairing inline; `CheckInputs` (script verification) is at `:599`.
   Split the PTX arm's *expensive* check (the pairing) out of `CheckSpecialTx`'s *structural +
   cheap-contextual* checks, and run it after `:599`. Then a junk-scriptSig transaction rejects at
   `CheckInputs` — before it can buy a pairing — and the whole cheap-attack surface closes.
   - ★ **Constraint that makes this plan-then-build, not a one-liner:** `CheckSpecialTx` has THREE
     callers (`specialtx_validation.cpp:1004-1006`) — ATMP (tip), `ConnectBlock` (pprev), and
     `CheckBlock`→`CheckSpecialTxNoContext` (null). **The deferral must be ATMP-only.** `ConnectBlock`
     must keep verifying every PTXSESS regardless of ordering, because a block's transactions are not
     individually rejectable — that is where §4.B (batch) belongs, not deferral. So the change is an
     interface split with a per-caller policy, on the exact path (mempool acceptance) that produced
     the last two surprises.

2. **Raise `ptx-bad-quorum-sig`'s DoS score above 0.** A settle produced by a real quorum *always*
   verifies; a well-formed signature that does not verify against the named group key cannot come from
   a correct implementation, so it is unambiguously malicious.
   - ★★ **Deferral is a PRECONDITION for the DoS score being safe, and this answers the operator's
     concern directly.** The worry — "a legitimate node relaying a settle whose inputs are momentarily
     unspendable should not be banned" — is real *only without deferral*. **With** deferral, reaching
     the pairing (and thus the bannable `ptx-bad-quorum-sig`) means `CheckInputs` already passed, so
     the inputs are spendable and the script is valid; a bad signature at that point is a forged
     signature, not a transient relay condition. A momentarily-unspendable settle rejects earlier, at
     `CheckInputs`, with an ordinary non-PTX score, and is never banned. **The two fixes are safe only
     together: order first, then score.**

3. **Then the validation cache (§4.A)**, so the deferred pairing is not re-paid at `ConnectBlock` for
   anything that transited the mempool.

**Rate-limiting per peer** is a weaker third option: it bounds the burst but not the steady rate, and
an attacker rotating source addresses evades it. Prefer (1)+(2)+(3); reach for rate-limiting only if
measurement after those still shows exposure.

**Does it ride the recut?**

- **The DoS-score change alone is a safe one-liner but insufficient alone** — it bans repeat senders
  but does nothing about the first wave from each peer, or from rotating peers, and without deferral
  it is *unsafe* per point 2. So it must not ship without the deferral.
- **The deferral is NOT a blind land.** It restructures `CheckSpecialTx`'s interface and touches the
  mempool-acceptance ordering — the same path as BUG-048 and the funding-view surprise. **It needs
  the same plan-then-build treatment as W4-b's remaining half, not a spot fix on this pass.**

★ **Recommendation: BUG-052 does NOT ride the recut as a code change.** It is now demonstrated,
scoped, and its fix is specified; it should land as its own reviewed change (deferral + score + cache,
in that dependency order) before the operator set opens — it is a KDD-103 member, cheap to exploit and
present in `b637751` today, but the fix is on the path where "cheap-looking" has twice been wrong.

## 6. What W4-b's remaining half should cost — recommendation

| Item | Verdict |
|---|---|
| Pairing in consensus | **already shipped** (`81fcf26`), ~2.4 ms, flat in quorum size |
| `beacon == SHA256(quorum_sig)` | ~1 µs. Land it. |
| `results == PTX_MapBeacon(beacon, …)` | microseconds; reuses shipped code. Land it, with the `unique`-path pool-size bound checked against `MAX_BLOCK_SIZE` reasoning first (`PTX_MapBeacon` builds a pool of `high-low+1` entries and can `throw`). |
| BUG-052 (deferred pairing + DoS score) | **land before, or with, any of the above** |
| Mitigation A (BLS verify cache) | high value, in-tree precedent |
| Mitigation B (batch at connect) | high value at the 5.8 s worst case |
| Mitigation C (parallel) | defer; measure after A and B |

**The timing argument in the brief survives the correction and gets stronger.** A new consensus
rule is cheapest on a chain with no blocks — and the *remaining* rule is a hash comparison and a
mapping check, which is about as cheap as a consensus rule gets. What should not ride a recut
unexamined is BUG-052.

---

## 7. KDD-085 — sign-over-P2P: scope

### 7.1 The exposure, confirmed at source

`ptx_fanout.cpp:612-616`:

```cpp
std::string rpcpass = gArgs.GetArg("-rpcpassword", "");
if (!rpcpass.empty()) {
    std::string creds = gArgs.GetArg("-rpcuser", "") + ":" + rpcpass;
    evhttp_add_header(hdrs, "Authorization",
                      (std::string("Basic ") + EncodeBase64(creds)).c_str());
}
```

The **dialling node's own** `-rpcuser`/`-rpcpassword`, as HTTP Basic, to all eleven members of
every roll. The caller does not choose its quorum (selection is by tip hash), so it hands that
credential to whoever the chain picks.

Confirmed on the live fleet, read-only: every node's config carries `rpcuser=ptxw2rpc` and
`rpcallowip=0.0.0.0/0`, and caller1's argv lists 153 `-ptxnode=<id>:<suffix>@<ip>:29995` targets.
`-rpcwhitelist` does not exist in this fork (grep: 0 hits), so an authenticated caller reaches the
**entire** RPC surface — `stop`, `setban`, `invalidateblock`, the wallet.

★ **That pair is bounded, and the boundary is the point.** These RPC ports are bound inside a
private Docker network on a single host and the coins are testnet, so the credential guards nothing
an attacker could not reach by already being on the box — the qualifier ODC-079 carries, restated
here because `rpcuser=ptxw2rpc` alongside `rpcallowip=0.0.0.0/0` reads as a live misconfiguration
without it. ★ It is cited as evidence *because* the fan-out breaks exactly that boundary: it hands
the dialling node's credential to eleven hosts the operator does not control and did not choose.
The config is safe where it sits and unsafe the moment KDD-085 ships it off the host.

★ Note this is *additional to* the reason KDD-085 was originally registered (2026-08-12), which was
address/port correctness: the PTX-RPC endpoint is not on-chain — DGM advertises only the P2P port —
so the fan-out embeds *"GMs expose PTX-RPC at DGM-IP:convention-port"*, an assumption permissionless
operators break. The credential argument is the stronger one and it is new.

### 7.2 ★★ How large is it really — larger than "a message type plus a handler"

The brief's read is that identity, addressing and plumbing are built and only signing is left. The
plumbing is built; **the identity is built for the wrong set.**

- The DKG transport relays to peers whose `verifiedProRegTxHash` matches a session member
  (`ptx_dkg_net.cpp:32-45`). That field is set only by `CGMAuth::ProcessMessage`
  (`evo/gmauth.cpp:180`).
- `CGMAuth::PushGMAUTH` returns immediately unless `fGameMaster && activeGamemasterManager` with a
  non-null `proTxHash` (`gmauth.cpp:23-28`).
- The challenge that starts the exchange is only sent on connections flagged
  `m_gamemaster_connection` (`net_processing.cpp:300-305`), and `net.h:775` says so outright:
  *"Challenge sent in VERSION to be answered with GMAUTH (**only happens between GMs**)"*.

**A caller is not a gamemaster.** (Confirmed on the fleet: caller1's argv has `-staking=1` and no
`-gamemaster`.) It therefore cannot become a GMAUTH-verified peer, and cannot use the relay path
the DKG uses.

There is a second shape mismatch. The DKG messages are **gossip**: `PushInventory` → `inv` →
`getdata` → store-and-relay, five pending-message queues drained by a background thread
(`ptx_dkg_net.cpp:419-427`, `PendingForPhase`). Signing is a **directed request with a correlated
reply** — eleven of them, with a stop-at-threshold rule. That is a different messaging pattern on
the same socket layer, not an extra case in an existing switch.

### 7.3 ★ The finding that makes it tractable: the caller needs no identity at all

`gm_bls_sign`'s authorisation is **not** the RPC credential. It is BUG-032's gate:
`PTX_SignRoundIfCommitted` → `PTX_RollCommitmentPresent` (`ptx/ptx_mempool.cpp:413-421`,
`:327-339`) — *is there a funded, mempool-accepted PTXROLLCOMMIT for this exact
(`round_seed`, `quorum_hash`)?* KDD-088 made this explicit: the predicate was always *local mempool
presence*, never *who asked*.

**So an authenticated caller with no commitment gets nothing, and an unauthenticated one with a
commitment should get a signature.** The Basic-auth header is not protecting the signing
operation; it is an artefact of the courier being JSON-RPC. A P2P sign request can be
**unauthenticated** and lose no security property — the payment gate is the authorisation, and it
is already the only one that was ever doing work.

That removes the largest item anyone would budget for.

### 7.4 Replace, or coexist behind a flag?

**Replace.** A flag means the RPC path ships and stays enable-able, and the whole point is that
`-rpcuser`/`-rpcpassword` must stop leaving the machine. A permissionless operator who enables the
legacy path re-creates the exposure for *every other operator's* callers, not just their own — the
failure is not contained to whoever flips the switch.

The compatible transition is a **capability**, not a flag: a member advertises P2P sign support
(service bit or protocol version), the caller uses P2P where advertised and RPC only where not,
and the RPC arm is deleted at the next tag once the fleet is uniform. KDD-088 already established
the mixed-fleet precedent — `gm_bls_sign` guards with `params.size() < 2`, not `!= 2`
(`rpc/ptx.cpp:552` — `request.params.size() < 2`), so old members ignore extra arguments.

### 7.5 What the caller needs

`ptx_roll` runs in-process on the node it is called on, so the caller *is* a full node with a P2P
stack, a connection manager and (via `PTX_ResolveMemberAddr`) live DGM address resolution. It needs:

1. **A connection to each member's advertised P2P address.** Already resolvable from the DGM list;
   the port is on-chain, which is the original KDD-085 argument. Most members will already be
   connected for block relay.
2. **A request/reply correlation** — `round_seed ‖ quorum_hash` is a natural key, already unique
   per round.
3. **No new identity, per §7.3.**
4. **A DoS bound on the responder side.** This is the one genuinely new surface: today the RPC
   port is (nominally) credentialed; a P2P sign request is reachable by any peer. The gate is
   `PTX_RollCommitmentPresent`, which is a **full O(mempool) scan under `mempool.cs`** — see §7.7.

### 7.6 ★ Latency and throughput — likely faster, and the measurement already exists

`doc/ptx/FANOUT_BUDGET_ANALYSIS.md` fits, across ten netem rungs:

```
p50 ≈ 1.47 s + 0.052 s × (one-way ms)   →  ~26 sequential one-way traversals per roll
```

decomposed there as *"a ~22-traversal sequential first pass (commitment INV trickle + the
sequential sign dials) plus a partial second pass"*, with the explicit note that this coefficient
**is the parallelisation target, not a network property**.

Since that analysis, two things landed that already claim most of the win: the **parallel dialer**
plus `FANOUT_WALL_MS = 30000` (`ptx_fanout.cpp:358-360`), and **KDD-088 direct-attach**, which
removes the commitment-propagation wait by carrying the commitment in the request itself.

So the honest estimate for KDD-085 is:

- **Against the pre-parallel baseline:** large. ~26 traversals → ~2.
- **Against `HEAD`:** **modest.** Direct-attach already removed the propagation term (79–91 % of
  roll latency by the FANOUT §11 accounting) and the parallel dialer already removed the
  serialisation. What remains for P2P to win is **connection setup**: `ptx_fanout.cpp:169` /
  `:599` set 3 s and 5 s timeouts on a *fresh* `evhttp_connection` per member per pass, with
  `Connection: close` (`:614`) — so every pass pays TCP setup ×11. P2P reuses sockets that already
  exist for block relay: **saves ~1 RTT per member per pass**, plus the HTTP framing.
- **Against that, P2P adds:** message-queue latency (the DKG transport drains on a background
  thread, not inline), and the caller's own `SendMessages` cadence.

**Net: expect a win in the low hundreds of milliseconds at fleet-local RTT, and a larger, more
useful win in the tail** — the stall class documented in FANOUT §4 (two rolls at 131.7 s) is a
property of per-pass connection cost, which P2P removes structurally rather than bounding with a
wall-clock.

**Recommendation: do not justify KDD-085 on latency.** Justify it on the credential and the port
assumption. The latency case was real before direct-attach and is now a secondary benefit; claiming
it as the driver invites a measurement that will underwhelm.

### 7.7 Effect on the roll ceiling

The ceiling today is caller-side and economic — *confirmed non-dust coins held* (365:365 per the brief; not re-measured here)
— and **KDD-085 does not move it.** Each roll still needs a funded commitment, still spends a coin
and returns change, still pays the service fee.

What KDD-085 *does* change is a member-side cost that is currently invisible. `gm_bls_sign`
performs **two full `O(mapTx)` scans under `mempool.cs`** on the common path — once inside
`PTX_AcceptAttachedCommitment` (`ptx_mempool.cpp:352`) and once inside `PTX_SignRoundIfCommitted`
(`:419`) — and three when an attachment is actually accepted (`:402`). Moving signing to P2P makes
that path reachable by any peer rather than by a credentialed one, so **the scan becomes the DoS
surface** and should be indexed (a `(round_seed, quorum_hash)` map maintained on mempool
add/remove) **in the same change**, not after it.

★ This is the same lock that L1 touches, and it is the reason L1's throughput question matters
beyond L1.

### 7.8 Admission control — and does P2P change it?

`rpc/ptx.cpp:292-318` broadcasts the commitment **before** `PTX_FanOutSign`. If the quorum then
fails to reach threshold, the caller has paid for nothing.

★ The mitigating measurement stands, and it should be stated whenever this item is raised:
threshold is `formed_size/2 + 1` (`ptx/ptx_quorum_store.cpp:967`) = **6 of 11**, so five members can be entirely absent and rolls
still complete. A single withholding actor achieves nothing; it takes six colluding members of one
quorum, and selection is by tip hash, so an attacker cannot choose which quorum they are in.
**The fee loss is the exposure; the freeze is not.**

**Does sign-over-P2P make the two fixes one? Partly — and less than it first appears.**

- P2P gives a natural place to observe acceptance before committing: a `PTXSIGNREQ` could be
  answered with a cheap *"I hold a CURRENT share for this quorum and I am willing"* before the
  commitment exists. Six such acknowledgements would let the caller defer the broadcast.
- **But BUG-032 forbids the useful version of that.** The gate exists precisely so that no
  signature is produced before payment is irrevocable. A pre-commitment acknowledgement can
  therefore only ever attest *availability*, not *commitment to sign* — a member may still fail
  afterwards, and the caller has no recourse.
- And KDD-088 pushes the other way: the commitment now rides the request, so the request cannot
  precede the commitment's existence — only its *broadcast*, which direct-attach has already made
  less load-bearing.

**Conclusion: they are adjacent, not identical.** Deferring the broadcast until six availability
acknowledgements arrive is a small, useful change worth ~the fee on failed rolls, and it is
*easier* under P2P than under RPC, but it does not fall out for free and it must not be sold as
part of KDD-085's core.

### 7.9 Size

**A week, not days** — for the replacement done properly. Breakdown:

| Piece | Size |
|---|---|
| `PTXSIGNREQ` / `PTXSIGNRESP` message types, serialization, protocol version bump | ~1 day |
| Directed request/reply with correlation + stop-at-threshold, replacing the libevent dialer | ~2 days |
| Responder-side DoS bound: index the commitment lookup, rate-limit per peer, size caps | ~1 day |
| Capability advertisement + mixed-fleet transition, RPC arm retained but unused | ~1 day |
| Fleet verification: latency ladder re-run, mixed-fleet run, adversarial responder tests | ~2 days |

The **two-days** answer is only available if the scope is "a flag, coexisting" — which §7.4 argues
against, because it leaves the credential path shippable.

**Prerequisite ordering:** KDD-085 should land *after* the fan-out has stabilised on direct-attach
(it has) and *after* the `PTX_RollCommitmentPresent` index exists, since P2P makes that scan
adversary-reachable.

---

## 8. Summary of what this document registers

- **ODC-081** — a shipped comment and a shipped verifier both assert W4-b is unbuilt after its
  signature half landed; the assertion was believed and became a task premise.
- **BUG-052** — the pairing precedes fee and script verification at mempool acceptance, under
  `cs_main` + `mempool.cs`, with DoS score 0.
- **KDD-103** — the class: *what breaks when the operator set opens*.
- **KDD-105** — a credential used where the question is not about identity is a design error,
  not a configuration one; reducing its blast radius does not fix it. (§9)
- **KDD-106** — a measurement taken on the fleet is not automatically conservative. (§9.4)
- **ODC-083** — the shared `ptxcaller` credential, accepted with a written expiry. (§9.10)
- **ODC-084** — Sybil / pool-share accumulation: previously unregistered. (§9.12)

---

## 9. KDD-085 sign-over-P2P — the build plan

Recon 2026-08-25 against `9ff584a`, read-only. §7 is the scope; this is the plan. Where the two
disagree this section is later and cites source.

### 9.0 ★★ The frame: no identity anywhere

KDD-085 is not a hardening item. The current design has a **credential answering a question that
was never about identity**, and the cryptography already answers it correctly.

The right question at a GM is *"is there a funded commitment for this?"* — which every GM can
answer **independently, from data it already holds, about a caller it has never heard of.** So the
target model is:

- the caller does not authenticate — nothing about the caller matters;
- the GM trusts neither the caller, nor the requester (which may not be the caller), nor the other
  ten members;
- each member checks and signs, or does not.

Eleven mutually-distrusting strangers each reaching the same verdict from public data is the only
shape that works permissionlessly, and it is what threshold BLS exists to make possible.
**"Replace, not flag" follows from this rather than being a preference:** a flag leaves a path in
which identity still decides, and one operator enabling it re-creates the assumption for everyone.

★ **Why per-operator credentials were rejected** (and this generalises — see KDD-105, §9.10): in
the real network eleven members may be eleven operators, none of whom knows who the caller is or
should trust each other. Per-operator credentials assume a GM can *enumerate the callers it
trusts*. It cannot. Five secrets instead of one reduces blast radius and leaves the defect intact.

### 9.1 Two corrections to §7

**(a) §7.3 overstates and §7.7 contradicts it.** §7.3: a P2P sign request "can be unauthenticated
and lose no security property." §7.7: the mempool scan "becomes the DoS surface." Same claim,
negated; neither cites the other. ★ Precisely: **the credential is not load-bearing as
authorisation and IS load-bearing as a DoS bound.** Both must be replaced. BUG-053's shape — two
passages in one document, each locally true.

**(b) The shared credential is `ptxcaller`, and it has a second factor.** `install.sh:865-890`
installs two coordinator-supplied lines on every GM — `rpcauth=ptxcaller:<salt>$<hmac>` and
`rpcallowip=<caller>` — both, per `:866`, "**unchanged when another operator joins**". Each host
gets its own random `rpcpassword` (`:1108`). So the exposure is not copies at rest: **the caller
transmits the plaintext `ptxcaller` password to eleven members per roll** as HTTP Basic
(`ptx_fanout.cpp:612-616`), and any member reads it off its own wire. `rpcauth` protects the config
at rest and nothing in transit. ★ `rpcallowip=<caller>` is a real second factor — a harvester must
connect *from the caller's address*. **Realistic severity: "one hostile operator plus one widened
config", not "one hostile operator."** Lower than assumed, still unacceptable, because nothing
detects the widening and the fleet already runs `0.0.0.0/0`.

### 9.2 ★★ S1 — what a GM can verify alone, from chain data, about a stranger

| # | check | site | input | trust required? |
|---|---|---|---|---|
| 1 | a funded commitment exists for this exact (`round_seed`,`quorum_hash`) | `ptx_mempool.cpp:327-339` | **local mempool** | ★ none — but see below |
| 2 | commitment well-formed; pays exactly one service-fee output to the accumulator | `specialtx_validation.cpp:955-962` | tx bytes + chainparams | none |
| 3 | quorum is **canonical**: a real record ACTIVE at `nSeedHeight` (BUG-033) | `:977-981` | `ptxQuorumStore`, chain-derived | none |
| 4 | seed height not stale — anchor lag ≤ 60 (ODC-073) | `:984-987` | `pindexPrev` + payload | none |
| 5 | this GM is a member — holds a CURRENT share for `quorum_hash` | `PTX_BLS_GetCurrentShare` | local keystore | none |
| 6 | inputs exist and are unspent | `TryATMP` → `CheckInputs` | local UTXO set | none |

★ Checked and worth recording: `nPTXSeedHeightWindow` is **60 on ptxtestnet**
(`chainparams.cpp:1011`) — an in-source note records that ptxtestnet was previously taking the
`0`-means-disabled default. The launch chain has the bound.

★ **Nothing requires believing the requester.** Every check reads either the GM's own chain state
or caller-supplied bytes that are *self-validating* (signatures, funding). That is the whole
argument, and it holds.

★★ **But check 1 is the exception that shapes the design: the mempool is not consensus state.** It
is node-local, varies by propagation, policy and eviction, and GMs legitimately disagree about it —
which is exactly why the retryable "commitment not seen" path exists (`ptx_mempool.cpp:430`). So
"eleven strangers reach the same verdict from public data" is **not true of the mempool**.

**The fix is that the request must carry its own evidence** — KDD-088's direct-attach, made
mandatory. Then the GM derives its verdict from caller-supplied self-validating bytes checked
against its own chain state (2,3,4,6), and check 1 becomes a local cache rather than a dependency.
★ **The mandatory-commitment decision is therefore load-bearing twice, for two independent
reasons** — it is what makes the model correct (here) *and* what makes it survivable (§9.4).

★ **The current model stated in the same terms, for the comparison:** today a GM signs because
someone presented a shared secret. That is a proxy for authorisation which the network cannot
verify, which every GM must hold *identically*, and which any holder can replay against any other
GM. It is strictly weaker than the check it stands in for, and it is the only part of the system
that requires operators to trust each other's operational hygiene.

**One honest caveat, pre-existing and unchanged by KDD-085:** "funded" means *fundable in my
current UTXO view*, not *irrevocably paid* — an unconfirmed commitment can in principle be
replaced. That is BUG-032's existing design point, not something this change introduces, but the
reframe should not claim more than it delivers.

### 9.3 S1.3 — the trade, stated explicitly

Under the new model anyone may ask any GM to sign anything satisfying the gate. What that buys:
a partial signature over a round **they funded**. A hostile requester who satisfies the gate is a
paying customer.

**More acceptable than today? Yes, decisively.** Today a credential-holder gets the *entire* RPC
surface — `stop`, `setban`, `invalidateblock`, the wallet — because this fork has no
`-rpcwhitelist` (grep: 0 hits).

★ **But it is not strictly better, and must not be sold as such.** Severity falls; *reachability*
rises. Today the endpoint needs a secret **and** an IP on the allow-list. Tomorrow it needs a TCP
handshake. **Severity down, exposed surface up** — sound only if §9.4 holds.

### 9.4 ★★ S1.4 — the security model, quantified

With no credential, **the bound on work-before-rejection is the entire defence.**

**Today's cheapest bogus request.** `PTX_RollCommitmentPresent` (`ptx_mempool.cpp:327-339`) is a
full linear pass over `mapTx` that copies a `shared_ptr` per entry, under `LOCK(mempool.cs)` — and
it is the **first statement** of both entry points (`:352`, `:419`). Attacker cost: 88 bytes
(24-byte P2P header + two `uint256`). Victim cost: the whole scan, holding the global mempool lock.

★ **The lock is the amplifier, not the CPU.** `mempool.cs` is contended by ATMP, block assembly and
the settle path (lock order corrected only in `063d5d3`). This is a **liveness attack on block
production**, not a CPU burn.

At ~50 ns/entry (iteration + atomic refcount dominate; `IsPTXRollCommitTx` is two field compares,
`transaction.h:370`):

| mempool | scan | req/s to hold the lock continuously | attacker bitrate |
|---|---|---|---|
| 1,000 tx | 0.05 ms | 20,000 | 14 Mbit/s |
| 10,000 tx | 0.5 ms | 2,000 | 1.4 Mbit/s |
| 100,000 tx | 5 ms | 200 | **141 kbit/s** |
| 300,000 tx | 15 ms | 67 | **47 kbit/s** |

★★ **At the default `maxmempool=300` (`policy.h:25`), roughly 47 kbit/s — a dial-up trickle — holds
a GM's mempool lock continuously.** And the mempool is partly attacker-controlled: filling it is a
one-off fee cost, after which every subsequent 88-byte request is amplified.

★ **The fleet understates this by 6×** — it runs `maxmempool=50`, and `install.sh` does **not** set
`maxmempool`, so every real operator gets the 300 default. A fleet measurement of this is not
conservative; it is wrong in the reassuring direction.

**The fix, in order of strength:**

1. ★★ **Mandatory commitment in the request.** The responder then runs size cap → hex → decode →
   `IsPTXRollCommitTx` → payload names the requested round — **five cheap checks, none touching
   `mempool.cs`** — before any mempool access. `PTX_AcceptAttachedCommitment` (`:345-380`) already
   has exactly this ordering; it needs promoting from optional to required. An attacker must
   produce a well-formed PTXROLLCOMMIT for their own round before a victim takes any lock, and
   then `TryATMP` rejects it for the same cost as relaying any junk transaction.
   **This is BUG-052's lesson applied before the fact:** cheap discriminator first, expensive work
   unreachable without passing it.
2. **Index the lookup** — `(round_seed, quorum_hash) -> txid`, maintained on mempool add/remove.
   O(1), no `shared_ptr` churn. **Prerequisite; a standalone win** (the happy path pays two scans
   per request, and `ptx_sign_tick` re-dials every pending member every 150 ms up to 200 attempts,
   `ptx_fanout.cpp:516-545`).
   ★ **LANDED `bfea163` — but NOT in this shape.** What exists is a generation-keyed *derived*
   cache, not a hook-maintained index. The substitution is deliberate (BUG-036 REGISTER 2) and the
   defence stated here is unaffected, because it never depended on O(1). **§9.13(a) is the
   authority; this paragraph is the plan, not the build.**
3. **Per-peer token bucket.** ★ Do not design one — **copy `m_addr_token_bucket`** (`net.h:737`,
   used at `net_processing.cpp:1645-1649` for ADDR). Score misbehaviour via the existing
   `Misbehaving()` (`net_processing.cpp:643`) and **only after a cheap check fails**, never after a
   lock-taking one.

**After:** cheapest rejection is a decode failure at ~1–2 µs with **no lock taken**; at 563 bytes
on the wire that is **2.2–4.5 Gbit/s** to saturate one core.

★★ **~47 kbit/s → multi-Gbit/s: five orders of magnitude, and a qualitative change — the
bottleneck moves from an exclusive lock that stalls block production to NIC bandwidth that only
costs the attacker.**

**Can a request be rejected without touching chain state at all?** Yes, and that is the design
target: everything through step 1 above is pure byte inspection. Only a request that is
*syntactically a plausible paid round* earns a lock.

### 9.5 S2 — message design

- **`ptxsignreq`** { `round_seed`, `quorum_hash`, `commit_raw` **(mandatory)** } — mandatory per
  §9.2 *and* §9.4.
- **`ptxsignresp`** { `round_seed`, `quorum_hash`, `sig[96]` } **or** typed refusal
  { `retryable | terminal`, reason }. The distinction already exists
  (`RPC_PTX_COMMITMENT_NOT_SEEN`) and must survive the transport change.
- **Correlation:** `(round_seed, quorum_hash)` — already unique per round; no new nonce.
- **Reachability:** the member's DGM-advertised P2P address — on-chain, KDD-085's original
  argument. Most members are already connected for block relay.
- ★ **GMAUTH does not transfer and could not be used.** `ptx_dkg_net.cpp:32-45` keys the relay on
  `pnode->verifiedProRegTxHash` matched against session members: GM-to-GM **by construction**. A
  caller is not a gamemaster. The new message must be accepted from an unverified peer *by
  design* — which is why §9.4 is the whole risk and the whole plan.
- ★ **The caller does not become anything new.** `ptx_roll` is an in-process RPC on a full node
  today and remains one. Operator story and SDK story unchanged — worth stating, because "move to
  P2P" invites the assumption that callers must now run a node. They already did.
- **Timeouts:** keep the 30 s wall (`FANOUT_WALL_MS`), already the single authority by its own
  comment (`:350-360`). The 3 s/5 s per-dial timeouts (`:169`, `:599`) **disappear** — they are
  `evhttp_connection` setup and there is no connection to set up. A block-denominated deadline is
  *not* available: the only height bound is the consensus same-block mandate
  `nExpiryHeight == nSeedHeight`. (Grepped `ptx_dkg.cpp`; none found — low-confidence negative,
  confirm during build.)

### 9.6 S3 — the operator-story delta: there is no secret to distribute at all

★★ **`ONBOARDING.md` calls them "The five values". KDD-085 deletes two of the five** —
`rpcallowip=<caller>` and `rpcauth=ptxcaller:<salt>$<hmac>`. The remaining three are the `addnode`
seeds, the spork **public** key and the genesis `nTime`/nonce — **all public**.

**So after KDD-085 the coordinator mints only public values. There is no secret to distribute, and
no operator has to be trusted to keep one.** That is the strongest form of the argument and it is
the one to lead with: not "a simpler operator story" but *the confidentiality requirement is gone*.

★ **The deletion surface.** `PTX_FanOutSign` has **exactly one** production caller
(`rpc/ptx.cpp:325`). `PTX_FanOutCommit` / `PTX_FanOutReveal` have **zero** — declared, defined,
referenced nowhere, tests included. **~200 of the file's 711 lines are already dead.**
`ptx_fanout.cpp` goes entirely **except** `PTX_ResolveMemberAddr`, which **moves** (DGM host
resolution is the permanent half of fix A).

| removed | count | today |
|---|---|---|
| coordinator-minted **secrets** | **2 → 0** | the only two values requiring confidentiality |
| GM config lines | 2 | `rpcauth`, `rpcallowip` |
| daemon options | 1 | `-ptxfanoutport` (`init.cpp:552`) |
| inbound-reachability requirement | 1 | 29995 open to the caller → **loopback-only for everyone** |
| operator failure modes | ≥4 | 401 (no `rpcauth`), 403 (no `rpcallowip`), wrong fan-out port, firewall/NAT/IP-family unreachable |
| `self-check.sh` | most of it | its stated purpose is *"IS MY RPC REACHABLE AT THE ADDRESS I REGISTERED ON CHAIN?"* — a question that ceases to exist |

★ Each removed failure mode shares one signature, which is why documentation cannot fix them: the
GM **registers, syncs, shows ENABLED and silently refuses every signing request**
(`install.sh:891-896`). The node looks healthy and is not.

### 9.7 S4 — admission control

§7.8 stands: **adjacent, not identical.** "Will you sign?" is forbidden by BUG-032 — a member
cannot commit before payment is irrevocable, which is the property the gate exists to hold.

★ A weaker probe works and is nearly free: *"do you hold a CURRENT share for `quorum_hash`?"* —
gate check 5 in isolation. No signature, so BUG-032 is untouched; no leak, since membership is
already on-chain. Six affirmatives → broadcast. Converts a **certain** fee loss into a **risked**
one; it does not eliminate it.

Keep the mitigating measurement attached: threshold is `formed_size/2 + 1` = **6 of 11**
(`ptx_quorum_store.cpp:967`); five members can be absent and rolls still complete, and selection is
by tip hash so an attacker cannot choose their quorum. **The fee is the exposure; the freeze is
not.** Separate increment, ~0.5 d. Do not let it grow the core.

### 9.8 S5 — what actually demonstrates this

★★ **The fleet cannot, and the reason is structural: 161 containers on one host sharing one
credential is precisely the configuration this change makes unnecessary.** A green fleet run proves
the transport works and says nothing about the property. It also understates the DoS surface 6×
(§9.4).

**The minimum that demonstrates it — two properties, both cheap:**

1. **A GM signs for a caller it has no credential relationship with.** Venue: two hosts, no shared
   `rpcauth`, no `rpcallowip` entry for the caller. Under today's build this *must* fail with 401;
   under KDD-085 it must succeed. **The RED leg is the current binary** — which makes it a real
   discriminator rather than a green that proves nothing.
2. **A GM rejects a request lacking a funded commitment** — and rejects it *before* taking
   `mempool.cs`, which is the §9.4 claim and must be asserted directly, not inferred from timing.

**Venue:** ptx001/002/003 + ptx01, once they exist — real hosts, distinct operators-in-principle,
no shared secret between them. Add one deliberately instrumented node logging what it receives:
today it harvests a working `ptxcaller` credential for the others; under KDD-085 it receives a sign
request and nothing else. **Two nodes, and it is the only experiment that proves the point.**

Everything else — protocol correctness, threshold behaviour, mixed-fleet transition — the fleet
tests fine.

### 9.9 Risk, sequencing, size

**Blast radius: total — failure mode benign.** Every roll depends on this path. ★ A BLS signature
is self-verifying, so a broken transport yields a *missing* result, never a wrong one, failing loud
into the existing forfeit/abandon machinery. No block format, validation rule or consensus change:
node-local transport only.

| component | est | note |
|---|---|---|
| ~~Index `PTX_RollCommitmentPresent`~~ — **LANDED `bfea163`** | ~~0.5 d~~ **done** | ★ green on px1, 488/488 `--enable-debug`; built as a derived cache, not the planned hooked index — §9.13(a) |
| `ptxsignreq`/`ptxsignresp` + serialization + protocol bump | 1 d | |
| Directed request/reply, correlation, stop-at-threshold | 2 d | replaces the libevent dialer |
| Mandatory-commitment ordering + token bucket (copy `m_addr_token_bucket`) | 0.5 d | cheap: ordering exists, limiter is in-tree |
| Capability advertisement + mixed-fleet transition | 1 d | |
| Delete `ptx_fanout.cpp`, move `PTX_ResolveMemberAddr`, drop `-ptxfanoutport`, rewrite `self-check.sh`, ONBOARDING five-values → three | 1 d | ★ the payoff, and real work |
| Fleet run + the two-host adversarial test (§9.8) | 2 d | |
| **total** | **~8 d** — **~7.5 d remaining** | availability probe (§9.7) +0.5 d, separate |

### 9.10 ★ Timing — recommendation

Per-operator credentials are off the table (§9.0), so the options reduce to two.

**Recommend option 2, with a recorded expiry.**

**Accept the shared credential across ptx001–003 now; land KDD-085 before any *external* operator
receives it.**

Why:

1. ★ **The trust assumption is real rather than assumed.** ptx001–003 are Vileda's own hosts. The
   defect is that the model *requires trusting parties you cannot enumerate* — on three hosts one
   person controls, there is nothing being assumed that is not true. That is precisely the
   distinction per-operator credentials failed: they pretended to fix the model; this does not
   pretend, it accepts a bounded exposure while the model is fixed.
2. **Option 1 delays the chain ~8 days** to remove an exposure that, for those three hosts, does
   not exist.
3. ★ **The expiry must be written down, not left to drift** — *"the shared `ptxcaller` credential
   does not leave Vileda's own hosts; KDD-085 lands before operator #2 is invited."* An acceptance
   without an expiry is how the fleet arrived at `rpcallowip=0.0.0.0/0` and ODC-079.
4. **The index (0.5 d) is unconditional and rides now** — not blocked on the message design, a
   strict improvement to a live per-roll cost, and it removes the §9.4 exposure's worst term
   independently of when the rest lands.

**Sequence:** index this week alongside the chain → cold-sync green → chain proven on ptx001–003 →
KDD-085 (~7.5 d) → *then* invite external operators.

★ And a sharper statement of the boundary than "distribution": **the line is the moment the
credential reaches a host its holder did not choose.** Cloning onto Vileda's own three does not
cross it. Inviting operator #2 does.

### 9.11 Register — KDD-105

> **★★ KDD-105 (adopted 2026-08-25) — A CREDENTIAL USED WHERE THE QUESTION IS NOT ABOUT IDENTITY IS
> A DESIGN ERROR, NOT A CONFIGURATION ONE; REDUCING ITS BLAST RADIUS DOES NOT FIX IT.** Raised when
> per-operator caller credentials were proposed as a mitigation for KDD-085 and rejected. ★ **The
> test that decides it:** ask what question the credential answers. `gm_bls_sign` asks *"should I
> sign this?"*, and its actual authorisation is the BUG-032 payment gate — *"is there a funded
> commitment?"* — which every GM answers independently from data it already holds. The credential
> answers *"do you know a secret?"*, which is a **proxy the network cannot verify**, must be held
> identically by every member, and can be replayed by any holder against any other. ★ **Why the
> mitigation is not a fix:** issuing five credentials instead of one narrows blast radius and
> leaves the model intact — it still requires each GM to enumerate the callers it trusts, which in
> a quorum of eleven mutually-distrusting operators is not something a GM can do. **A mitigation
> that scales the damage without changing the question is a delay, not a remedy.** ★
> **Generalisation:** when a secret is used to authorise, ask whether the thing being authorised is
> checkable from public state. If it is, the secret is standing in for a check that the system can
> already perform, and it is strictly weaker than that check — it can be stolen, must be
> distributed, must be rotated, and forces mutual trust between parties who otherwise need none.
> ★ Same family as KDD-097/098/099 — *a mechanism asserting something it does not deliver* — with
> the failure one level up: not a setting that misstates what it binds, but **a whole mechanism
> answering the wrong question, correctly.**

### 9.12 ★★ The honest boundary — what KDD-085 does NOT fix

**KDD-085 makes the network credential-free. It does not make it trustless.** Stated here so
nobody reads more into it than it claims, with register coverage checked rather than assumed.

| | status | where |
|---|---|---|
| **Collusion** | ★ **COVERED, and the source is ahead of the prose.** 6 of 11 still produces a signature; threshold BLS gives *"no individual can influence the output"*, never *"no coalition can"*. `ptx_quorum_store.cpp:963-967` already carries the **ODC-036 addendum**: `t == n/2+1` is an explicit **STOPGAP**, with **KDD-048** pre-documenting a `t=7-at-n=11` upgrade — and names the hazard, that the derivation *"would silently return 6 for a ceremony that baked 7"*. **The durable fix is persisting `t` in the record rather than deriving it.** | ODC-036, KDD-048 |
| **Liveness / withholding** | **COVERED.** Five absent is survivable, six stalls. Per-round participation is not on chain, so a withholding member is indistinguishable from an unreachable one — which is the same indistinguishability KDD-077 §5 owns. | KDD-077 §5, ptxbea-known-limitations |
| **Beacon correctness** | **COVERED as a known gap.** W4-b's remaining half: `beacon == SHA256(quorum_sig)` and `results == PTX_MapBeacon(…)` are still not consensus-checked, so a staker can stamp arbitrary results into a PTXSESS. Auditability is on-chain; enforcement is absent. | 2c-iii, ODC-081, §0 above |
| **Sybil** | ★★ **UNCOVERED — zero occurrences of "sybil" in any document in this repository.** Anyone may run a GM, so anyone may run many. Selection by tip hash stops quorum-*shopping* but not **pool-share accumulation**: an actor holding a large fraction of registered GMs gets a proportionate fraction of seats in every quorum, and at `t = 6 of 11` needs only a majority of one quorum. The barrier is collateral, and on ptxtestnet collateral is **100 HMS** (`chainparams.cpp:757`). **Registered now as ODC-084.** | **ODC-084 (new)** |

★ **The point of this table is that KDD-085 sits underneath all four and touches none of them.**
Removing the credential removes a mechanism that was answering the wrong question; it does not
answer any of the questions above. Three were already registered. The fourth was not, and the
reason it was missed is instructive: *"permissionless"* was discussed throughout as an
**operational** property (who may join, who must be trusted) and never as an **economic** one (how
much does controlling six seats cost).

### 9.13 ★★ What landed, and where it diverges from this plan

Written 2026-08-31 against `bfea163`. §9.0–§9.12 are the plan as recon left it on 2026-08-25; this
section is the **landing record**, and by this document's own convention (§9 preamble) it is later
than all of them and cites source. **One of the seven §9.9 rows has landed; the other six are
unstarted** — `ptxsignreq`/`ptxsignresp` return zero grep hits anywhere in the tree, the attachment
is still optional (`rpc/ptx.cpp:592` guards on `params.size() > 2`), `ptx_fanout.cpp` is still 711
lines, and `-ptxfanoutport` is still at `init.cpp:552`.

**(a) ★★ The index was built in a different shape than §9.4 step 2 specifies — deliberately.**

| | §9.4 step 2, as planned | `bfea163`, as built |
|---|---|---|
| structure | `(round_seed, quorum_hash) -> txid` | `std::set<std::pair<uint256,uint256>>` — no txid; the predicate only ever answered *present?* |
| maintenance | hooks on mempool add / remove | rebuilt when `mempool.GetTransactionsUpdated()` changes |
| cost per request | O(1) | O(log n) lookup — the O(n) scan is paid **per mempool mutation**, not per request |
| files touched | would have meant `txmempool.cpp` — this fork has **no** entry add/remove signals to hook | `ptx_mempool.cpp` only |

★ **Why the substitution, which is the more important half of this row.** A hook-maintained index
is *stored-and-trusted*: correct only while every add/remove/clear site remembers to update it, and
a single missed site makes this predicate **lie silently** — refusing legitimate rolls, or admitting
a commitment that is gone. That is **BUG-036 REGISTER 2** (derive-don't-store; stored copies drift)
landing on a predicate that gates *payment*. The generation-keyed cache is **derived** and
recomputed whenever `mapTx` changes, so **staleness is unrepresentable rather than merely
unlikely**, and no future mempool change has a new invariant to violate.

★ **The §9.4 defence survives the shape change intact, because it never depended on O(1).** The
load-bearing property is that **a flood of requests does not mutate the mempool** — the rebuild is
gated on *change*, not on *being asked*, and an attacker controls how often they ask, not how often
`mapTx` turns over. The ~47 kbit/s lock-hold (§9.4) is removed either way.

★ **Correctness rests on the counter being complete, which was checked rather than assumed:**
`nTransactionsUpdated` is incremented in `addUnchecked` (`txmempool.cpp:471`), `removeUnchecked`
(`:568`) and `clear()` (`:884`). `GetTransactionsUpdated()` takes `cs`, a `RecursiveMutex`
(`txmempool.h:471`), so calling it under our own `LOCK(mempool.cs)` is safe. A validity **flag** is
used rather than a sentinel generation, so unsigned wrap-around cannot alias *"never built"* onto a
live counter value. Full rationale in-source at `ptx_mempool.cpp:322-365`; the case is
`Kdd085_CommitmentIndex_TracksMempoolChange`.

**(b) The "UNCOMPILED" caveat on `bfea163` is DISCHARGED — and the count needs its flags.**

`bfea163`'s commit message opens *"★★ UNCOMPILED AND UNTESTED … it must be built on px1 and the
suite run before this commit is trusted or any tag is cut from it"*, and predicts 488. **It was
built on px1 on 2026-08-25 and the prediction held: 488/488 under `--enable-debug`, with
`Kdd085_CommitmentIndex_TracksMempoolChange` passing.** HEAD is safe to tag from. (`v0.1.2-testnet`
sits at `9ff584a` and does **not** contain this; the released binaries are unaffected either way.)

★★ **Quote the build flags with the count, always.** `src/ptx/test/ptx_lockorder_tests.cpp` declares
four cases that are **mutually exclusive by build config**: `--enable-debug` compiles the three real
BUG-048 lockorder cases and drops the announcer → **488**; the canonical non-debug recipe compiles
the announcer — which prints *"SKIPPED: needs DEBUG_LOCKORDER + ENABLE_WALLET — this suite is
inert"* — and drops the three real cases → **486**. ★ **So the canonical invocation does not
exercise the BUG-048 fix at all**, and a bare *"the suite is green"* conceals which of two different
suites ran. Method note: **diff test-case names, never compare counts.**

★ **Why (b) is recorded here rather than left in the commit message.** That message is the most
visible statement about HEAD, and it is now stale in the **untrustworthy** direction: a reader
concludes HEAD is unverified when it is verified. Same family as KDD-104 — *a status written at one
moment and read as current*. This document's own header had acquired the identical defect (*"nothing
landed from this document"*) and is corrected in the same pass.

**(c) Unchanged and still owed — the sequencing constraint from §9.4 step 1.** The attachment
remains optional at `rpc/ptx.cpp:592`, which is correct **only while the credential is still the DoS
bound**. Mandatory-commitment is load-bearing twice — it is what makes the model *correct* (§9.2:
the mempool is the one check GMs legitimately disagree on) and what makes it *survivable* (§9.4) —
so it must flip to mandatory **in the same increment that opens the endpoint**, never in a
follow-up. ★ Stated as a constraint on build order rather than as a task, because a task can be
reordered and this cannot.
