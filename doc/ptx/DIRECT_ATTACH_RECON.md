# Direct-attach — design recon (2026-08-19)

**Status: recon, not a build plan.** The evidence question is settled elsewhere
(`FANOUT_BUDGET_ANALYSIS.md` §11 and §12). What remained was one design question, and this
document leads with it because if it has no answer the feature closes on a design constraint
regardless of how good the evidence is.

**Verdict up front: the anti-free-preview property is PRESERVED, by construction, and without
reintroducing a propagation wait.** The reason is narrower and more solid than expected — see §1.

---

## 1. The gate — and why attach does not weaken it

The gate is `PTX_SignRoundIfCommitted` (`ptx/ptx_mempool.cpp:274`), and everything turns on what it
actually tests. It calls `PTX_RollCommitmentPresent` (`:252`), which is:

```cpp
LOCK(mempool.cs);
for (const auto& e : mempool.mapTx) {           // ← the member's OWN mempool
    if (!tx->IsPTXRollCommitTx()) continue;
    if (p.round_seed == round_seed && p.quorum_hash == quorum_hash) return true;
}
```

★ **The security predicate is LOCAL MEMPOOL PRESENCE. It is not "was this broadcast to the
network", and it never was.** Gossip is merely the delivery mechanism that gets the bytes to a
place where this predicate can pass. `rpc/ptx.cpp:292` says so in its own words: *"The quorum
members' gm_bls_sign gates on seeing this commitment **in their mempool**."*

That collapses the design question:

| | today | direct-attach |
|---|---|---|
| how the bytes arrive | P2P gossip | attached to the sign request |
| what the member does with them | `AcceptToMemoryPool` | `AcceptToMemoryPool` |
| **what the gate tests** | **local mempool presence** | **local mempool presence** |

**The predicate tested is byte-for-byte the same.** Direct-attach changes the transport and nothing
about the property. This is why the answer is "preserved by construction" rather than "preserved if
we are careful".

**The mandatory condition:** the member must run the attached transaction through the *normal*
`AcceptToMemoryPool` path. **Verify-without-accepting would break the property** — see §2.

**It does not reintroduce a propagation wait.** `AcceptToMemoryPool` is local and synchronous. It
needs the commitment's *inputs* in the member's UTXO view, and the builder already guarantees those
are confirmed coins (`ptx_mempool.cpp`: *"Sign funding inputs (all confirmed → pcoinsTip)"*). So the
member needs chain sync — which it has — not gossip of this transaction.

---

## 2. Threat model, worked

**(a) "Well-formed but never-broadcast commitment: send it to eleven members, collect six partials,
never publish. Result obtained, no fee paid."**

Does not work, for two independent reasons.

1. **Handing it to eleven members *is* publication.** Acceptance into a member's mempool is followed
   by that member's ordinary relay; the caller cannot accept-and-withhold. The transaction is in
   eleven mempools and spreading before the sixth partial comes back.
2. **`AcceptToMemoryPool` is what makes the fee real.** It validates signatures, checks the inputs
   exist and are unspent in the member's own UTXO view, and enforces fee policy. A commitment that
   is not genuinely funded does not get in, so the gate does not open. "Well-formed" is not enough —
   the attack requires *mempool-valid*, and mempool-valid means paid.

**(b) Why verify-without-accepting fails.** A member that merely parsed and signature-checked the
attached transaction would open exactly the hole (a) closes: no UTXO check (inputs could already be
spent), no relay (the caller really could withhold), no fee policy. **Accept-into-mempool is the
mechanism, not an implementation detail** — the whole property rides on it.

**(c) Double-spend after collecting partials.** The caller could try to spend the commitment's
inputs again with a conflicting transaction. **This is unchanged from today** — today the caller
also broadcasts first and could then attempt a conflicting spend. Attach does not weaken it, and
arguably strengthens it: eleven members hold and relay the commitment immediately rather than after
a multi-hop gossip delay, so the honest transaction has a *better* head start than it does now.
The binding itself is the coin-chain: the settle spends the commitment's `vout[1]`, so once the
commitment is mined the UTXO is consumed and no second settle is possible at any depth (BUG-034's
no-upper-bound analysis).

**(d) Residual, and it is new: members now run `AcceptToMemoryPool` on bytes supplied by a caller.**
That is a DoS surface — spam a member with expensive-to-validate rejects. It is bounded (the sign
RPC is already an authenticated endpoint, and validation cost is one transaction) but it is a real
new attack surface and belongs in the build's test matrix: reject-before-ATMP on obvious
malformation, and rate-limit per caller.

---

## 3. Updated premises (several have moved since the feature was first parked)

* **Fan-out is now parallel** with a 150 ms re-dial tick and a 30 s wall (§10, KDD-087). With the
  commitment attached, the first dial carries everything the member needs, so the `retryable`
  branch — which exists *only* for `!PTX_RollCommitmentPresent` — should essentially never fire.
  **The re-dial loop does not disappear** (transport errors still need retries) but its dominant
  cause does, and the "wait-not-reject" comment at `ptx_mempool.cpp:280` becomes vestigial.
* **Expected post-attach floor.** §11 measured the dial-and-sign component directly: **0.10 s clean,
  0.45 s at d100, 1.68 s at d200.** Roll latency should approach those numbers, i.e. **1.10→~0.10 s,
  4.71→~0.45 s, 8.01→~1.68 s** (~5–11×). Stated as a *prediction to be falsified*, not a claim.
* **Every observed failure class is eliminated by construction — confirmed against the code, not
  assumed.** `PTX_SignRoundIfCommitted` has exactly three failure branches: missing commitment
  (retryable), no CURRENT `sk_share`, and BLS signing failure. Only the first is propagation-related,
  and it is the one attach removes. Near-miss, partial non-delivery and total blackout (§12.3) are
  all instances of that single branch.

---

## 4. Mechanics

* **Where it rides.** A third, optional parameter on `gm_bls_sign`
  (`rpc/ptx.cpp:544`, currently `{"round_seed_hex","quorum_hash"}`): the raw commitment hex.
  Size is a few hundred bytes — irrelevant next to the request it rides on.
* **Member side.** On `!PTX_RollCommitmentPresent` **and** an attachment present: decode → `TryATMP`
  → `RelayTx` → re-check the gate → sign. Keep the existing retryable path for the no-attachment
  case, so behaviour is unchanged for callers that do not attach.
* **Caller side: keep the caller's own broadcast.** It funds the coin-chain output the settle
  spends, and it keeps the gossip path alive as a fallback. Attach is an *additional* delivery
  channel, not a replacement — which is also what makes a mixed fleet safe.
* ★ **Compatibility is free — checked, not assumed.** `gm_bls_sign` guards with
  `request.params.size() < 2`, **not** `!= 2` (`rpc/ptx.cpp:546`). An old member handed a third
  parameter therefore **ignores it and behaves exactly as today** — returning the retryable
  "not seen" until gossip delivers. Combined with the caller keeping its own broadcast, a mixed
  fleet needs **no version negotiation, no feature flag, and no fallback logic**: attaching is a
  no-op against old members and an optimisation against new ones. (Had the guard been `!= 2` this
  would have been the one piece of real protocol work; it is not.)
* **Interaction with KDD-085 (sign-over-P2P, owed).** The fan-out currently assumes GMs expose
  PTX-RPC at DGM-IP:standard-port, which permissionless operators violate. If delivery moves to P2P,
  the attachment rides that message instead. **The two should be designed together** — attach is a
  payload change and KDD-085 is a transport change, and doing them separately means touching the
  same call site twice.

---

## 5. ★ Activation — this is NOT consensus-adjacent

The earlier framing assumed a consensus gate. **It does not need one.** The commitment transaction's
format, validation, and the commit/settle pairing rules are all untouched; the only change is how
the bytes reach a member before it signs. Nothing a validator checks depends on how a member learned
about a transaction. `gm_bls_sign` is a caller↔GM RPC, not a consensus interface.

**Consequence: no `UPGRADE_*` gate, no activation height, no coordinated flag day.** It ships as a
rolling deploy, and the mixed-fleet period is safe because the caller keeps broadcasting anyway.
This removes what would otherwise have been the most expensive part of the work.

---

## 6. What this recon does NOT establish

* No code is written and no measurement of an attached implementation exists. The §3 latency
  numbers are a prediction derived from §11's decomposition, and should be tested against a build.
* The DoS surface in §2(d) is identified, not designed against.
* Compatibility turned out to be a non-issue (§4), so the remaining unknowns are the DoS surface
  and the member-side ATMP wiring — both small. The recon did not surface a blocker.
* If the blackout class (§13 / `FANOUT_BUDGET_ANALYSIS.md`) turns out to have a cheap caller-side
  cause, the *failure-elimination* half of the case shrinks and direct-attach should be re-justified
  on latency alone — which §11 supports independently, but the argument would be narrower.
