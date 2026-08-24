# W4-b cost, and KDD-085 sign-over-P2P scope

**Date:** 2026-08-24 · **Gate:** RECON → PLAN (nothing landed from this document)
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
