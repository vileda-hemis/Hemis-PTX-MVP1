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

## 9. Direct-attach verdict, and the bound that actually binds (2026-08-18)

**Verdict up front:** direct-attach is **NOT closed** — but it is **not yet earned either**, and it
must not be built next. §8 read the tail correctly; what §8 missed is *why* the tail stops where it
does. Every residual failure is clipped by the **attempt budget at 9.0 s**, not by propagation
giving up. Until that bound is lifted we cannot tell "gossip never arrived" from "we stopped
asking at 9 s", and direct-attach's entire justification rests on which of those is true.

### 9.1 The ten rungs, sequential vs parallel-clean

Gen A = sequential dialer (08-17 13:42–17:39). Gen C = parallel dialer + BUG-041 fix
(08-18 00:59–02:27). Seconds; `ok/total`.

| rung | GEN A sequential | GEN C parallel-clean | p50 |
|---|---|---|---|
| **thin** clean | 24/24 · p50 1.18 · p95 1.35 · max 1.81 | *not re-run* | — |
| **thin** d25 | 23/24 · p50 2.54 · p95 2.79 · max 3.26 | 24/24 · p50 2.17 · p95 2.46 · max 2.61 | −15% |
| **thin** d50 | 23/24 · p50 4.22 · p95 4.53 · max 4.58 | 23/24 · p50 3.16 · p95 3.47 · max 3.62 | −25% |
| **thin** d100 | 24/24 · p50 7.24 · p95 7.63 · max 7.71 | 23/24 · p50 4.72 · p95 5.16 · max 5.62 | −35% |
| **thin** d200 | 8/8 · p50 12.63 · p95 13.42 · max 14.33 | 22/24 · p50 7.55 · p95 8.41 · max 8.56 | −40% |
| **load** clean | 200/200 · p50 1.47 · p95 2.15 · max 2.46 | 200/200 · p50 1.66 · p95 2.24 · max 2.35 | +13% |
| **load** d25 | 200/200 · p50 2.73 · p95 3.19 · max 3.80 | 200/200 · p50 2.16 · p95 2.94 · max 3.34 | −21% |
| **load** d50 | 199/200 · p50 4.30 · p95 6.82 · max 6.85 | 200/200 · p50 3.01 · p95 3.72 · max 4.30 | −30% |
| **load** d100 | 200/200 · p50 6.79 · p95 8.37 · max 8.95 | 200/200 · p50 4.07 · p95 4.81 · max 5.88 | −40% |
| **load** d200 | 200/200 · p50 11.76 · p95 13.35 · **max 131.74** | 200/200 · p50 5.98 · p95 7.41 · **max 7.87** | −49% |

Totals — Gen A: **1101/1104**, 3 misses (thin d25 BLS 5/6 @46.6 s; thin d50 BLS 5/6 @83.7 s;
load-d50 BLS 3/6 @114.2 s). Gen C: **load 1000/1000, zero failures**; **thin 92/96, 4 failures**
(d50 BLS 5/6; d100 BLS 5/6; d200 BLS 0/6; d200 BLS 3/6 — all at 9.09–9.10 s).

> **Scope correction.** "800/800 zero fails across all ten rungs" is the *load lane only* (it is in
> fact 1000/1000 across five load rungs). The thin lane, same binary, same night, is **92/96**. The
> near-miss class did **not** disappear.

### 9.2 Did the near-miss class disappear, or hide below the sample size?

**Neither.** It is plainly visible where it was measured. It is absent under load and present in thin:

* **Load: 0/1000.** By the rule of three the true load failure rate is **< 0.3 %** (95 % conf).
  That is a real result — but load is the *favourable* case, not the neutral one.
* **Thin: 4/96 ≈ 4.2 %** (95 % CI ≈ 1–10 %), and **delay-shaped**: 0/24 d25 → 1/24 d50 → 1/24 d100
  → 2/24 d200. **Two of the four are BLS 5/6** — one partial short, exactly direct-attach's target
  population.

Load masks the class because sustained traffic keeps gossip batched and signing paths warm. **A
quiet five-operator testnet looks like the thin lane, not the load lane.** The failure rate that
matters for launch is therefore ~4 % at injected RTT, not 0 %. With a failed roll forfeiting ~1.0
stake to the pot (recorded on-chain, §BUG-034 family), that is operator money.

### 9.3 Why parallelisation removed most of the tail — hypothesis confirmed, with a correction

The working hypothesis — *dialling all eleven at once means the sixth-fastest partial lands while
later members are still being waited on rather than not yet asked* — is **confirmed**, and it is
the dialer's explicit design (`ptx_fanout.cpp:378-388`): the round returns at the threshold-th
fastest partial via loopbreak from the completion callback, and stragglers are abandoned at teardown.

**Correction: concurrency alone was not sufficient — removing the per-pass barrier was.** The first
parallel cut kept pass barriers; because every roll's commitment is freshly broadcast, pass one
always ends below threshold on not-seens, and the barrier then taxed *every* roll by the slowest
member's full dial (4.25 s measured with one 2000 ms member). "Stop at the sixth fastest" requires
the single-loop shape, not merely parallel dials.

**Second correction, and this is the important one: parallelisation also tightened the effective
timeout by 5–12×.** The attempt budget is a *re-dial-opportunity* count, not a time bound. Under
the sequential dialer one "attempt" was a full serial pass over 11 members, so 60 attempts spanned
46–114 s of wall clock — which is exactly where Gen A's three failures sat. Under the parallel
dialer a tick is 150 ms flat, so the same 60 attempts now expire at **9.0 s**. All four Gen C
failures land at 9.09–9.10 s. `60 × 150 ms = 9.000 s`. The fan-out now gives up an order of
magnitude sooner in real time than the constant was calibrated for.

### 9.4 The bound that actually binds — `FANOUT_WALL_MS` is unreachable

Both ceilings are enforced in the same 150 ms tick (`ptx_fanout.cpp:505-526`): the wall at
`wall_ms >= 30000`, the budget at `++ticks >= 60`. Ticks are the only clock either sees, so the
budget fires at tick 60 (~9.0 s) and the wall would need tick ~200 (~30 s). **The wall can never
fire in normal operation.** It survives only as protection against tick starvation (mean tick
interval > 500 ms).

This makes §5's ship reasoning self-refuting. §5 justifies 30 s as *">2× the worst legitimate
ladder observation (14.33 s) — it rejects nothing real."* The criterion is right; the code does
not implement it. **9.0 s rejects that 14.33 s observation outright** — and Gen A's *entire* thin
d200 success distribution (p50 12.63, p95 13.42, max 14.33) sat above 9.0 s, all 8 of which
completed successfully under the old constants. The parallel dialer moved the distribution down to
p50 7.55 / p95 8.41, so most rolls now fit — but the cutoff now sits **inside the upper tail
instead of outside it**. At thin d200, p95 is within 0.6 s of the cutoff: the system is running at
~93 % of its budget at p95, which is a cliff, and intercontinental testnet operators would sit on it.

**We therefore cannot currently distinguish** a roll whose gossip never arrived from one whose
gossip would have arrived at 11 s. Every failure we have is budget-clipped.

### 9.5 The cheap experiment that must precede any direct-attach decision

One constant, ~30 minutes of ladder:

```
- static const int FANOUT_MAX_ATTEMPTS = 60;
+ static const int FANOUT_MAX_ATTEMPTS = FANOUT_WALL_MS / FANOUT_RETRY_MS;  // 200 -> wall is the single authority
```

Deriving it makes the wall the one real bound and prevents the two from drifting apart again.
Re-run thin d100 and d200 (48 rolls, ~15 min each):

* **Failures vanish** → the residual was budget-clipping, propagation does deliver, and
  **direct-attach closes on evidence** — its target population was an artefact of our own cutoff.
* **Failures persist at ~28 s** → gossip genuinely is not delivering within any sane window, and
  **direct-attach is proven necessary**, with the strongest evidence it has ever had.

Cost of raising it: a doomed roll blocks the caller ~28 s instead of ~9 s. Since a failed roll
forfeits stake either way, completing late strictly beats failing early. Risk is low and bounded
by the wall, which becomes reachable for the first time.

### 9.6 Registry outcome

* **Direct-attach — PARKED, gate named (not closed, not deferred-without-reason).** Reopening or
  closing it is now a *decidable* question with a 30-minute experiment attached, where before it
  was a judgement call. It was parked three times for want of evidence; it stays parked a fourth
  time because the evidence we have is confounded by our own timeout. **Standing reopen condition
  unchanged:** a failure tail at higher RTT or larger mesh. **New close condition:** §9.5 returns
  clean at a 28 s budget.
* **The bound mismatch (§9.4) is the actionable defect** and outranks direct-attach: it is one
  line, it makes a documented safety margin real, and it protects operator stake at launch RTT.

### 9.7 The d200 residual-serialisation question — closed, and it does *not* implicate connection reuse

Excess latency over the clean baseline, per ms of one-way injected delay. Slope × 1000 = one-way
traversals; ÷2 = round trips. A fully-parallel fan-out needs **2–3 RTT**.

| lane | d25 | d50 | d100 | d200 | mean |
|---|---|---|---|---|---|
| Gen A thin | 27.2 RTT | 30.4 | 30.3 | 28.6 | **29.1 RTT** |
| Gen A load | 25.2 RTT | 28.3 | 26.6 | 25.7 | **26.5 RTT** |
| Gen C thin | 18.6 RTT | 19.2 | 17.4 | 15.8 | **17.7 RTT** |
| Gen C load | 10.0 RTT | 13.5 | 12.1 | 10.8 | **11.6 RTT** |

**The clean d200 did not land near target.** It gave **5.98 s load / 7.55 s thin** against the
2–2.5 s target — and the thin figure is *worse* than the holed run's 6.77 s (the holed run's d200
had only 20 successful samples and its four holes were `docker exec` failures against crashed
callers, not protocol misses; it is not a like-for-like comparison and should not be cited as a
regression). So **residual serialisation is real and large: ~11.6 RTT under load, ~17.7 thin,
against a 2–3 RTT target — still 4–6× off.** Serialisation did halve (26.5 → 11.6 load), confirming
§8, but half of far-too-much is still far too much.

The slope is **strikingly linear across all four rungs within each lane** (load 10.0–13.5, thin
15.8–19.2). Linearity in delay with a constant coefficient is the signature of a **fixed chain of
sequential round trips**, not queueing, not contention, not host load — the count of serialised
hops is a structural constant of the protocol path.

**Connection reuse is the wrong suspect, by §8's own finding.** §8 concluded the residual "lives
upstream of the dials" and then nominated connection reuse — a *dial-side* fix — as first suspect.
Those cannot both be right. Re-dials do pay a fresh connection (`ptx_fanout.cpp:362-372`, deliberate),
but that is ~1 RTT per attempt against an 11–18 RTT total, and the 150 ms tick is a fixed timer that
does not scale with RTT. The far larger term is multi-hop INV/GETDATA/TX gossip relay across the
161-node mesh before members will sign at all (the BUG-032 local-mempool gate) — which is *upstream
of the dials*, exactly where §8 located it, and which is what direct-attach would remove.

**Recommendation: park connection reuse.** It is a dial-side micro-optimisation aimed at ~1 of
11–18 RTT, on a lane with zero measured failures. If serialisation is attacked later, the first
move is **measurement, not a guess**: instrument the commitment's mesh-hop count from broadcast to
each member's mempool acceptance, and attribute the 11–18 RTT before optimising any of it.

### 9.8 Numbers superseded

* "3 misses in 848 rolls" — miss **count** is right; the denominator is **1104** (thin 104 + load 1000).
* "800/800 across all ten rungs" — load lane only, and it is 1000/1000; the thin lane is 92/96.
* Gen C has **no thin-clean rung**; slopes above use Gen B's thin clean (1.24 s, same dialer family).
  A thin-clean rung is owed on the next ladder run.

---

## 10. §9.5 executed — the wall fires for the first time, and the failure class splits (2026-08-18)

Gen D. Binary `f0e5458f` = trunk `5193525` + the one-line §9.5 change, **callers only** by
construction (`PTX_FanOutSign` has a single call site, `rpc/ptx.cpp:320`, reached only by
`ptx_roll`; GMs are responders and never run the dial loop). The *only* delta against Gen C is
the constant. Thin d100 + d200, 24 rolls each, same battery, same netem, same convergence gate.

```
- static const int FANOUT_MAX_ATTEMPTS = 60;
+ static const int FANOUT_RETRY_MS     = 150;      // reordered: the derived value
+ static const int FANOUT_WALL_MS      = 30000;    // must follow its operands
+ static const int FANOUT_MAX_ATTEMPTS = FANOUT_WALL_MS / FANOUT_RETRY_MS;   // 200
```

### 10.1 Results

| rung | Gen C (60 att, 9.0 s) | Gen D (200 att, 30 s) |
|---|---|---|
| thin d100 | 23/24 — p50 4.72, p95 5.16, max 5.62 | 23/24 — p50 4.73, p95 5.18, max 5.19 |
| thin d200 | 22/24 — p50 7.55, p95 8.41, max 8.56 | **24/24** — p50 7.56, p95 8.47, max 8.59 |
| combined | 45/48 | 47/48 |

**Failure latencies are the whole story.** Gen C: 9.09 s, 9.10 s, 9.10 s — all three at
60 × 150 ms, i.e. clipped by our own constant. Gen D: a single failure at **30.11 s**.

### 10.2 ★ The wall fired — first observation in the project's history

```
PTX: FanOutSign: wall-clock ceiling 30000ms hit after 199 tick(s)
     (5 collected, 3 pending, 3 inflight) — returning
```

One occurrence, caller4, fleet-wide (all 8 caller logs swept); **zero** "propagation budget
exhausted" lines anywhere. §9.4's diagnosis is now confirmed by direct observation rather than
by reading the tick arithmetic: the wall was unreachable before, it is reachable now, and it is
the single authority in practice — it fires at tick 199, one tick ahead of the derived budget,
because the wall check precedes the budget check and `wall_ms` accumulates real time ≥
`ticks × 150 ms`. The two can no longer drift apart.

### 10.3 The happy path is unchanged — the raise is costless

p50 4.72→4.73 and 7.55→7.56; p95 5.16→5.18 and 8.41→8.47. Identical within noise, as predicted:
stop-at-threshold still returns at the 6th partial (successes closed in 29–51 attempts, never
near either ceiling). Raising the ceiling costs nothing on rolls that were already succeeding.
The price is paid **only** by a doomed roll, which now blocks ~30 s instead of ~9 s — observed
once in 48. Since a failed roll forfeits stake either way, completing late strictly beats
failing early.

### 10.4 ★ §9.5's pre-registered dichotomy was too coarse — the class SPLIT

§9.5 offered two outcomes: failures vanish (direct-attach closes) or failures persist at ~28 s
(direct-attach proven). **Both happened, to different sub-populations**, because the failures
were never homogeneous:

* **The delay-shaped near-miss tail WAS our own cutoff.** Both d200 failures sat at 9.10 s and
  both are gone at a 30 s budget — d200 went 22/24 → 24/24, the *worse* RTT rung going clean.
* **A residual hard class survives 30 s of re-dialling.** The Gen D failure is not a timing
  margin: **3 of 11 members never delivered a partial across 199 re-dial opportunities**, and 3
  were still inflight at the wall. 30 seconds of asking did not produce them.

So the confound §9.4 identified is now removed, and what it was hiding is visible: a genuine
non-delivery population, cleanly separated from the budget-clipped one. **This is direct-attach's
actual target, isolated for the first time.**

Note it landed at **d100, not d200** — the rung with *less* injected delay went clean at 24/24
while the lighter rung carried the failure. Whatever this residual is, **it is not RTT-shaped**,
which argues a different mechanism from propagation margin (cf. the historic total-blackout
class, though this one is partial: 3 of 11, not 10–11 of 11).

### 10.5 What is and is not established — read before citing

* **ESTABLISHED (mechanism, by direct observation):** the wall is reachable and authoritative;
  the derived constant behaves exactly as designed; a failure class exists that 30 s of re-dial
  does not fix.
* **NOT ESTABLISHED (statistics):** that the pass rate improved. 45/48 → 47/48 is **not**
  significant (nor is 22/24 → 24/24 on its own); n is far too small. Do **not** cite Gen D as
  "the fix raised the success rate" — it is one event either way.
* **Consequently direct-attach is NOT yet "proven" in the §9.6 sense.** Its close condition
  ("§9.5 returns clean at a 28 s budget") is **not met** — one roll failed at the wall. But
  promoting a single observation to a build decision would repeat the error §9.5 exists to
  prevent. **Replicate first:** thin d100 + d200 at n≥3 samples (≥144 rolls) on the Gen D
  binary, counting wall-hits, and decompose each by pending/inflight at the wall.
* §9.5's own framing is superseded by §10.4: a two-way dichotomy on a population that turned
  out to be a mixture.

### 10.6 Corrections owed to earlier sections

`§Decision, up front` ("Keep `FANOUT_MAX_ATTEMPTS = 60`") and `§1` (describing 60 as a pass
count independent of the time bound) are **superseded by §9.4 and this section**. They are left
in place as the record of what was believed at ship time; the operative values are here.
