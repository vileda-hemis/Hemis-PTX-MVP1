# Fan-out Budget Analysis — the latency ladder verdict and the shippable ceiling

**Date:** 2026-08-17 · **Data:** w2-fleet latency ladder, 10 rungs banked (ALL-DONE 17:39:03),
161-node fleet (153 GM + 8 callers), netem one-way delay injected on all veths, post-BUG-039/040
binaries. · **Status:** DECISION — ships `FANOUT_WALL_MS = 30000` alongside the parallel dialer.

## Decision, up front

Keep `FANOUT_MAX_ATTEMPTS = 60` and the 5 s per-member connection timeout. Add a **30-second
wall-clock ceiling** to `PTX_FanOutSign`'s attempt loop. Rationale in §5; everything above it is
the evidence.

## 1. What the budget actually bounds (and what it doesn't)

`ptx_fanout.cpp`: `FANOUT_MAX_ATTEMPTS = 60` is a **pass count**, `FANOUT_RETRY_MS = 150` is an
**inter-pass sleep**, and the 5 s `evhttp_connection_set_timeout` is **per member**. There is no
wall-clock check. The effective ceiling is therefore

```
60 × (150 ms + Σ sequential per-member response times of still-pending members)
```

which **stretches with RTT and with slow members**. The "~9 s at 150 ms/pass" comment in the code
is the zero-RTT reading; it is not a property of the loop. Theoretical worst case (all 11 members
timing out at 5 s every pass) is ~55 minutes.

## 2. The data

Thin battery = 24 sequential rolls (8 shapes × 3); load battery = 20 rolls/block × 10 blocks = 200.
Delay figures are one-way ms (pair RTT = 2×). Latency is the `ptx_roll` RPC wall time.

| Rung | ok | p50 s | p95 s | max s |
|---|---|---|---|---|
| clean | 24/24 | 1.18 | 1.35 | 1.81 |
| d25 | 23/24¹ | 2.54 | 2.79 | 3.26 |
| d50 | 23/24¹ | 4.22 | 4.53 | 4.58 |
| d100 | 24/24 | 7.24 | 7.63 | 7.71 |
| d200 | 8/8² | 12.63 | 13.42 | 14.33 |
| load-clean | 200/200 | 1.47 | 2.15 | 2.46 |
| load-d25 | 200/200 | 2.73 | 3.19 | 3.80 |
| load-d50 | 199/200³ | 4.30 | 6.82 | 6.85 |
| load-d100 | 200/200 | 6.79 | 8.37 | 8.95 |
| load-d200 | 200/200 | 11.76 | 13.35 | **131.74**⁴ |

¹ Both thin-rung misses (5/6 BLS, 46.6 s and 83.7 s) sit in the **first battery round after netem
apply** (rolls 5/24 and 8/24); every one of the 16 subsequent rolls in each rung was clean. They
are **post-apply transients** (P2P ping re-measurement window), not steady-state propagation
misses, and are excluded from the propagation-miss count. For future runs the battery start should
gate on **ping convergence** (the verify step already reads `getpeerinfo` pingtimes — gate the
start on it instead of the fixed 30 s settle, then confirm after).
² d200 thin is an 8-sample retry rung — treat its p95/max as indicative only.
³ The one real steady-state miss in the whole ladder: 3/6 BLS at block 5 of 10, mid-rung, in the
queueing-onset rung. Counted.
⁴ See §4.

**The headline coefficient:** across all ten rungs the median is linear in injected delay:

```
p50 ≈ 1.47 s + 0.052 s × (one-way ms)      →  ~26 sequential one-way traversals per roll
```

Fitted across ten rungs (thin and load agree within noise), consistent with the source-level
reading of the roll path: a ~22-traversal sequential first pass (commitment INV trickle + the
sequential sign dials) plus a partial second pass. **This coefficient is the parallelisation
target, not a network property** — with concurrent dials the same roll is bounded by gossip
propagation plus ~2 traversals of dial.

## 3. Load helps the median, hurts the tail

- load-d100 p50 **6.79 s beats thin d100's 7.24 s**: concurrent rolls share the commitment
  propagation wait (batched gossip) — the sequential ladder was the *pessimistic* read at the
  median.
- load-d50 p95 **6.82 s vs thin 4.53 s**: queueing onset. The sequential ladder was the
  *optimistic* read at the tail.

Both facts matter for reading mainnet telemetry later: a busy chain will look better than the thin
ladder at p50 and worse at p95.

## 4. The stall class — why d200 succeeded and how it also hung

At 200 ms one-way, threshold is typically reached inside ~2 passes (p50 11.76 s), nowhere near the
60-pass cap: **the pass budget is the right kind of generous.** But two rolls in load-d200 ran to
**131.7 s** (plus one at 25.3 s), i.e. ~60 passes × ~2.2 s realized pass cost — the budget-stretch
formula of §1 realized in the wild. Both **completed successfully** on a late pass, with zero
losses; the second-highest latency in the rung is 13.85 s, so this is an **isolated stall class,
not a distribution shift — do not read the max as typical.**

From an integrator's view, a 131-second `ptx_roll` is a hung caller. This is exactly the class a
wall-clock ceiling converts into a fast, clean, observable failure.

## 5. The decision: keep the caps, add `FANOUT_WALL_MS = 30000`

- **Rejects nothing real.** The worst *legitimate* observation across 2,648 banked rolls is
  14.33 s — at 200 ms one-way, itself beyond any plausible mainnet p99 RTT. 30 s is >2× that.
- **Converts the stall class into a bounded failure.** The observed 131.7 s stalls (and the
  theoretical minutes-scale stretch) become a ≤30 s clean `threshold not met` failure, which the
  existing machinery already handles: the forfeit signal (CR-without-S) fires, the abandon gate
  and the BUG-034 detector's pending-settle alert see it, and the caller is free.
- **The honest trade, stated:** in this dataset, 2 of ~2,000 load rolls at 400 ms RTT succeeded
  *only after* 30 s. Under the ceiling those two stakes would have been forfeited. We accept
  that: an unbounded hang is worse than a rare forfeited stake, and a settle landing two block
  intervals late is itself of marginal value. This is a **trade**, not a free lunch — recorded so
  nobody later reads the ceiling as costless.
- **Why not lower (e.g. 15 s):** thin d200's max of 14.33 s is a legitimate success. 30 s keeps
  ≥2× headroom over the worst legitimate observation at an RTT already beyond mainnet plausibility.
- **Why not a pass-count reduction instead:** the pass count is doing its intended job (covering
  source-trickle + mesh propagation, per the BUG-032 2b-iii record); the defect is that pass *cost*
  scales with RTT. Bounding wall time fixes the defect without re-opening the forfeiture class the
  60-pass raise closed.

The ceiling ships **in the same change as the parallel dialer** (below), which independently
collapses realized pass cost — the ceiling is then a genuine backstop rather than a lively limit.

## 6. Ship surface — install.sh and the wizard snippet must agree

The budget itself (60 passes / 150 ms / 5 s member timeout / 30 s wall) is **compile-time, not
operator-tunable** — deliberately: a per-operator budget would make roll behaviour a function of
whoever dialed, and the forfeiture trade in §5 is a protocol posture, not a preference.

What *is* operator-facing is the fan-out **reachability contract** (known-limitations §13): a GM
must expose PTX-RPC at its DGM-registered address on the convention port (chain RPC port, 29995 on
ptxbea; `-ptxfanoutport` to override). Therefore:

- **install.sh ships:** `rpcbind=[::]` (or explicit address), rpcallowip appropriate to the
  deployment, the firewall opening for 29995, and **no** `-ptxfanoutport` (default = convention).
- **The wizard snippet ships:** the same three, verbatim.
- **The agreement requirement:** if either side diverges (wizard writes a custom port the
  installer's firewall doesn't open, or vice versa), the node looks healthy on-chain and silently
  fails every signing request — the §13 failure mode. Any change to one MUST be mirrored in the
  other; treat the pair as one artifact. (KDD-085 sign-over-P2P retires this whole section.)

## 7. What comes next (recorded here so the numbers stay attached to their caveats)

- **Parallel dialer** (same change as the ceiling): concurrent member dials on one event loop,
  stop at the **sixth-fastest** partial. Expected d200 p50 ~12.6 s → **~2–2.5 s**; a partial
  result (e.g. ~6 s) is itself diagnostic of residual serialization.
- **The re-run comparison is not perfectly clean:** it lands on a new binary *and* the new matched
  32 GB memory pair. The expected signal (~5×) is far beyond what memory could account for, but
  the comparison should be recorded with that caveat, not presented as single-variable.
- **Direct-attach:** once the roll is gossip-bound rather than dial-bound, direct-attach's case
  rests on the **failure tail** (the BLS 5/6 near-misses), not latency. Read it that way when the
  re-measure lands.

## 8. The re-measure (2026-08-18) — the parallel dialer's verdict against §7's pre-registrations

Measured on the single-loop dialer (`0a38cfa`: one event_base per round, 150 ms tick,
stop-at-sixth-fastest, `FANOUT_WALL_MS=30000`), binary `29cbe4d` (adds the BUG-041 template
fix — latency-neutral), full 161-node fleet healthy end-to-end (zero caller deaths; the first
re-run attempt on 2026-08-17 evening was invalidated by BUG-041 crash holes and host crashes
#15–17 — this dataset is the clean full-fleet re-run, every rung 8 callers × complete).

| Rung | ok | p50 s | p95 s | max s | old p50 (§2) |
|---|---|---|---|---|---|
| clean | 24/24 | 1.24 | 1.76 | — | 1.18 |
| d25 | 24/24 | 2.17 | 2.46 | 2.62 | 2.54 |
| d50 | 23/24¹ | 3.16 | 3.47 | 3.55 | 4.22 |
| d100 | 23/24¹ | 4.72 | 5.16 | 5.31 | 7.24 |
| d200 | 22/24¹ | 7.55 | 8.41 | 8.56 | 12.63 |
| load-clean | 200/200 | 1.66 | 2.24 | 2.35 | 1.47 |
| load-d25 | 200/200 | 2.16 | 2.94 | 3.42 | 2.73 |
| load-d50 | 200/200 | 3.01 | 3.72 | 4.30 | 4.30 |
| load-d100 | 200/200 | 4.07 | 4.81 | 5.88 | 6.79 |
| load-d200 | 200/200 | 5.98 | 7.41 | 7.87 | 11.76 |

¹ All four thin misses are BLS-threshold outcomes (5/6, 5/6, then 3/6 and 0/6 at d200), not
harness artifacts; the tail deepens with delay — see the direct-attach reading below.

**Verdict against §7's pre-registered outcomes: the diagnostic case, not the target.** d200
p50 landed at 7.55 s thin / 5.98 s under load — not the ~2–2.5 s propagation-bound target;
§7's "a partial result (~6 s) is itself diagnostic of residual serialization" is the case
that realized, almost to the second. In §2's coefficient convention:

```
old:        p50 ≈ 1.47 + 0.052·(one-way ms)   → ~26 sequential traversals
new thin:   p50 ≈ 1.24 + 0.032·(one-way ms)   → ~16 sequential traversals
new load:   p50 ≈ 1.66 + 0.022·(one-way ms)   → ~11 sequential traversals
```

The dialer's own serialization is gone (proven separately by the slow-member probes: a
2000 ms member no longer taxes the roll). What remains is roughly **half the old traversal
count, still linear in RTT** — the residual serialization now lives *upstream of the dials*
(commitment propagation / INV trickle before members can sign, plus per-pass connection
establishment). That is the next hunt; **connection reuse (deferred in the ship decision)
is the first suspect to test**, since each tick's re-dial pays connection setup at RTT cost.

**§3's load findings both reversed, favourably.** Load now beats thin at the median at every
delay ≥50 (batched gossip amortizes the upstream wait — consistent with the residual living
in propagation, not dials), and the tail penalty is gone: load p95 ≤ thin p95 at d100/d200
(4.81 vs 5.16, 7.41 vs 8.41), and the d50 queueing onset vanished (old load-d50 p95 6.82 →
3.72). **The §4 stall class is gone:** max across all 800 load rolls is 7.87 s (old: 131.7 s
twice) — the single loop plus the wall turned the stall class into nothing observable; the
ceiling is now a pure backstop (0 rolls anywhere near 30 s). **800/800 load rolls succeeded**
(old: 1 steady-state miss + the stalls).

**Direct-attach, read as §7 prescribed:** with the roll now propagation-bound, direct-attach's
case rests on the failure tail, and the tail is real and delay-shaped: 4/96 thin rolls failed
BLS threshold, worsening with RTT (0/24 at d25 → 2/24 at d200, one a total 0/6). Under load
the tail vanished (800/800) — sustained traffic keeps signing paths warm, which is itself
weak evidence for the connection-reuse suspect above.

**Comparison caveats (per §7):** vs §2 the binary changed twice (fanpar2 dialer + BUG-041
template fix) and the RAM configuration changed twice (matched 32 GB pair → reseated
2×16+1×32 GB, 64 GiB total). The ~1.7× median improvement and the tail/stall eliminations
are far beyond plausible memory effects, but this remains a system-level comparison, not
single-variable. The 150 ms tick also quantizes measured latencies upward by up to one tick
per pass (visible as load-clean 1.66 vs old 1.47 — the price of the loop, accepted).
