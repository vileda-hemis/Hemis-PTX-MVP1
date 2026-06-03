# PTX Coordinator-Role Specification — v1

**Status:** W1.1 deliverable. Required before W1.2 ceremony code.
**Peer of:** `DKG_DESIGN_DOC_v1.md`, `DKG_IMPLEMENTATION_PLAN_v1.md`
**References:** ODC-021 (coordinator SPOF, resolved by DKG), KDD-051 (ceremony construction), KDD-052 (member set), KDD-053 (multi-quorum selection + failover).
**I2 status:** descriptive — resolved against `gm_bls_sign` source (`rpc/ptx.cpp:484–529`), 2026-06-03.

---

## §1 — The claim

PTX has no coordinator as a protocol entity. The word "coordinator" appears in the
implementation plan as shorthand for some per-roll and per-formation work; this spec
establishes that no party performing that work holds any protocol-recognised status.

> Any wallet or endpoint that can call `ptx_roll()` and fund the transaction can obtain a
> verifiable random result without routing through any privileged or central infrastructure
> service.

A coordinator may still *exist* as an optional deployment convenience — anyone may build,
run, or use one. What it may not do is *be a thing the protocol depends on*. The distinction
is the entire content of this spec, and it reduces to two invariants (§3).

---

## §2 — Two layers

**Protocol layer — no coordinator exists.** The protocol knows only three things: funded
callers, quorum Gamemaster (GM) nodes, and the on-chain `group_pk`. It has no concept of a
coordinator — no party it grants rights to, no node a roll must traverse, no state that must
be inherited, no identity GMs recognise. Remove every coordinator and the protocol is
unchanged.

**Deployment layer — a coordinator may exist, unprivileged.** A third party may run a service
that fans out to GMs and aggregates partials on a caller's behalf. This is a legitimate
convenience. It holds no keys, the protocol does not recognise it, and its existence is never
load-bearing: any caller can bypass it and self-aggregate from chain state at any time, with
no permission and no key transfer. Zero coordinators or ten, the protocol behaves identically.

**The test for which layer a coordinator lives in:** can you remove it and still complete a
roll? If yes, it is a deployment convenience. If no — if some roll cannot complete without a
specific coordinator's state, keys, or recognition — it is a protocol entity, i.e. a SPOF,
and DKG's purpose is defeated. This spec requires the answer to always be "yes, removable."

---

## §3 — The two invariants

"No coordinator as a protocol entity" is defined precisely as the conjunction of two
invariants. Both must hold; each rules out a distinct failure mode.

### I1 — No single point of failure (availability)

No coordinator's loss stops rolls. Removing any or all coordinators must not halt the system.

This holds because the coordinating work — read `group_pk` from chain, fan out to the
quorum's GMs, collect ≥ t partials, run Lagrange recovery, verify — is performed by the
**caller's own node**, transiently, for its own roll. The caller is the hub; GMs are leaves,
each answering only for itself, never relaying or aggregating another GM's partial. There is
no fan-out node a roll must pass through other than the caller's own. Nothing to persist,
recover, or hand over.

Proven by the §4 test.

### I2 — No protocol-privileged party (authority)

No coordinator the protocol grants authority, recognition, or exclusive right. Even a
coordinator that is present and running must be unable to decide anything that callers and
verifiers cannot independently check.

A coordinating caller decides nothing affecting correctness:

- Not which ceremony contributions are valid — GMs verify each other (KDD-051
  complaint/justification).
- Not `group_pk` — deterministic ceremony result, fixed on-chain in the PTXDKG record
  (KDD-052), verifiable by any observer.
- Not which quorum serves a roll — computed deterministically from chain state (KDD-053, §5).
- Not the result — threshold recovery is deterministic: for a given seed and quorum there is
  exactly one valid recovered signature, and any t valid partials recover it identically
  (confirmed by W3.1-Test1: all C(n,t) subsets recover byte-identical signatures). The caller
  has no degree of freedom to bias; it recovers correctly or fails.

A malicious coordinating caller can at most fail its own roll. It cannot produce a wrong
result that verifies, because verification is against an on-chain `group_pk` it did not choose.

I2 requires one property of the GM signing path:

> **A GM produces a partial signature in response to any well-formed roll request,
> regardless of requester identity.** It authenticates the *request* (well-formed seed) — not
> the *requester*, and (today) not whether the request is funded; funding is enforced at
> settlement, not at signing (see §3.4). If a GM signed only for a recognised coordinator,
> callers could not self-aggregate and a recognised coordinator would become a
> protocol-privileged party, violating I2.

> **[RESOLVED 2026-06-03 — descriptive]** The `gm_bls_sign` handler
> (`rpc/ptx.cpp:484–529`) performs no requester authentication: it checks only that the seed
> is well-formed hex and that the GM holds a key, then signs. There is no allowlist, no
> coordinator identity, no peer gate. The only auth is transport-level HTTP Basic
> (`httprpc.cpp:122–157`) — orthogonal deployment hardening, not coordinator-recognition. I2
> therefore holds *structurally today*: there is no coordinator identity in the signing path
> to privilege, so there is nothing to remove. The property is descriptive, not a W1.2
> requirement. The mainnet-deployment constraint (§7) is that transport-layer auth must not be
> implemented as requester-recognition, which would create the privilege I2 forbids.

### Popularity is not centralisation

A coordinator service that, in practice, becomes the path most callers use does **not**
violate either invariant — provided the bypass remains available (I1) and the service holds
no protocol privilege (I2). If every caller *chooses* one popular coordinator but any of them
*could* self-aggregate tomorrow with no permission and no key transfer, the protocol remains
decentralised: domination confers no protocol power and is unilaterally exitable. Market
concentration of coordinator services is a real-world dynamic but a non-protocol one, and is
out of scope here (§7).

---

### §3.4 — Unfunded signing is not a vector (why funding is not gated at signing)

GMs sign any well-formed seed without checking that it corresponds to a funded on-chain
round. This is deliberate, and it does not create an abuse vector:

- **A single partial is worthless** — it is one share; recovery needs ≥ t.
- **A full unfunded recovery** (an attacker reads the quorum's public member node_ids from the
  PTXDKG record, queries all members, recovers the signature itself) yields a
  *publicly-determined value*, not a secret. The recovered signature is deterministic for a
  given seed and `group_pk` (W3.1-Test1: all t-subsets recover identically). The attacker has
  computed the output any funded roll on that seed would also produce — it has learned nothing
  others cannot, and it confers no front-running advantage, because the seed is anchored to
  fixed, unforecastable inputs (anchor block hash, nonce chained off the previous recovered
  signature — design doc §3). The output is knowable exactly when its inputs are on-chain, i.e.
  when everyone else can know it. **This protection rests on the seed entropy anchoring, not on
  the signing gate.**
- **It pays nothing** without a funded settlement transaction, which the chain validates.

Binding signing to confirmed funded rounds was considered and rejected: it would require the
GM to verify per-roll chain state before signing, forcing the roll to wait for the funding tx
to confirm on-chain — **≥ 1 block of added latency**, breaking the sub-second roll the product
depends on — to prevent a non-harm. Funding is therefore enforced at settlement; signing stays
stateless and fast.

The residual exposure is **resource consumption** (CPU to sign, bandwidth to answer) on a
public GM endpoint — a standard internet resource-DoS surface, not a protocol vulnerability.
It is addressed at the deployment edge (§7), not in protocol.

## §4 — The falsifiable test (required at the W1.2 validation gate)

I1 (and, via the property in I2, the absence of requester-recognition) is proven by a measured
test, not asserted.

**Cold-caller completion from chain state alone.**

> A process with no prior relationship to the quorum — no coordinator state, no key material,
> no configuration naming it as a coordinator — starts cold, reads `group_pk` (and, for
> multi-quorum, the active-quorum set) from the chain, sends a roll request to the quorum's
> GMs, collects ≥ t partials, recovers, and obtains a verifying result.

Stronger form (subsumes the implementation plan's "coordinator-replacement" test):

> Crash the process that initiated the previous roll. A *different*, arbitrary, cold process —
> with no key-material transfer from the first — completes the next roll.

**Pass:** the cold process completes a roll and `PTX_BLS_Verify` passes, with no key-material
transfer and no coordinator-specific configuration. **Fail:** the roll cannot complete without
state, keys, or recognition inherited from a prior coordinator — a retained SPOF or privileged
party, and W1.2 is not done.

This is the *signing-path* slice of the "survives originating-operator teardown"
decentralisation litmus. Stake distribution and independent peering are the other two slices,
out of scope for this spec.

---

## §5 — Multi-quorum: selection and failover (reference to KDD-053)

Single-quorum (W1.2) has one quorum; there is no selection question, and I1/I2 are tested
against that one quorum. The following applies only when N > 1 active quorums exist (ODC-024
era); it is fully specified in KDD-053 and stated here only to confirm it preserves the
invariants.

- **Selection is computed, not chosen:** `ordering[0]` from
  `deterministic_shuffle(active_set, H(anchor_block_hash ‖ game_id ‖ roll_index_in_block))`,
  caller-controlled fields excluded. Caller and verifier compute the same ordering — so
  choosing the quorum is not a coordinator privilege (preserves I2).
- **Active set and N** are evaluated as of the roll's anchor block height, so caller and
  verifier agree.
- **Failover preserves no-steering:** chain-evident primary failure → verifiable re-route down
  the deterministic fallback chain; caller-observed failure → re-roll with a new seed (fresh
  draw, not a re-route of the fixed seed). Neither path lets a caller select among outcomes.

The coordinating function in multi-quorum is unchanged from §3 except that the caller first
*computes* which quorum, then performs the same direct fan-out and self-aggregation.

---

## §6 — Residual functions (all un-privileged)

The functions historically called "coordinator" duties, and why each carries no privilege:

| Function | Who can do it | Why un-privileged |
|---|---|---|
| Detect formation conditions, broadcast a formation proposal | Any node | Consensus is established through the ceremony (KDD-051 QUAL-locking), not the proposal. The proposer decides nothing about membership or the key. |
| Detect rotation conditions, broadcast a rotation proposal | Any node | Rotation re-runs the ceremony (KDD-045); the proposer has no authority over its outcome. |
| Collect partials, run Lagrange recovery for a roll | Any funded caller | Per §3: no key material, decides nothing, verifiable against on-chain `group_pk`. |
| Submit the settlement transaction | Any node | The transaction is consensus-validated; submission confers no authority. |

None is a standing role. Each is an action any appropriately-positioned node may take.

---

## §7 — Out of scope

- **Ceremony internals** (five-phase GJKR-hardened DKG) — KDD-051, W1.2.
- **Quorum lifecycle** (formation, rotation, disband, ejection) — KDD-044/045/046/047, W2.
- **Multi-quorum membership and scaling** — ODC-024 (open).
- **Market concentration of coordinator services** — a real-world dynamic, not a protocol
  property; the protocol guarantees exit, not market structure.
- **Deployment hardening for the GM signing endpoint** — a separate operational track. The
  endpoint's exposure is a *resource-DoS* surface (unmetered signing consumes CPU/bandwidth),
  not a protocol vulnerability (§3.4). This is a deliberate non-mitigation at the protocol
  layer: protocol-level mitigation would cost either ≥ 1 block of roll latency (funding-bound
  signing) or per-caller requester-recognition that erodes I2, both more expensive than the
  harm. It is therefore handled at the deployment edge by **identity-agnostic** rate-limiting
  (IP / connection / reverse-proxy), explicitly **not** per-caller-credential limiting at the
  GM — the latter would maintain per-caller identity on the GM and drift toward the
  requester-recognition I2 forbids. Constraint for the track: any transport-layer auth on a
  public mainnet GM endpoint must not become requester-recognition.

---

*PTX Coordinator-Role Specification · v1 · 2026-06-03 · I2 resolved descriptive (gm_bls_sign read); W1.1 deliverable*
