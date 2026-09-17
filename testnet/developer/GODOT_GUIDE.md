# PTX in Godot

Godot **4.3+**. Godot 3.x is not supported.

> **The contract is not in this file.** It is `vileda-hemis/PTX-Kingmaker` →
> `docs/PTXPAL_API.md`, public and authoritative, with `MAPPING_SPEC.md` beside it as a
> worked example of mapping a roll to game outcomes. Read it first. This guide covers only
> what a Godot developer has to do differently, and `sdk/godot/ptx_client.gd` is the client.

---

## 1. What you get, and what you hold

A **verifiable** random number: signed by a quorum of independent nodes and written to a
public chain, so a player who does not trust you can check the draw. That is the product.
A game that never shows the proof has bought a slow `randi()`.

**You hold rolls, not money.** HMS sent to a deposit address belongs to the operator the
moment it arrives, in exchange for rolls at the app's posted rate. What your identity holds
is an entitlement to play — *"8 raids remaining"*, never *"8 HMS"*. There is no withdrawal
endpoint, no refund path and no money balance anywhere in the rail. Entitlements are
non-refundable. Plan your economy around a count that goes down.

Speed: **the median is published, the tail is not**, and the two have different evidence
behind them.

**Median: about 1 second — and here is exactly what that rests on.** Daemon-side, 731 rolls
across the rail's whole life, of which 623 finished inside a second. ★ That population is
**705 rolls before the 2026-09-15 fixes and 26 after**, and the median is 1 s measured
*separately in each half* — so the figure is not an average taken across a change.

★ **The 26 are a thin sample and they are doing real work in that sentence, so the
asymmetry is worth stating plainly.** Twenty-six samples cannot support a p95, which would
rest on one or two points at the top of the sorted set. They can support a median, which is
the middle of that set and moves only if much of the sample moves. That is the actual
reason the median survives the withdrawal and the tail does not — not that one number is
friendlier, but that a small sample estimates the two with very different confidence.

★ **The stability was also predicted before it was measured**, which is what stops the 26
from carrying the claim alone. BUG-086's fix changed only what happens when a member is
*not* already connected; it cannot make the already-connected path faster or slower, and
that path is what produces the median. So ~1 s was expected to survive the fix, and the 26
rolls are consistent with that rather than the sole evidence for it.

Corroborated caller-side by three verification rolls through the full HTTP path: **0.84 s,
0.86 s, 1.41 s**. Three is not a distribution, it is the only caller-side data that exists,
and it does not contradict the median.

**No p95 and no worst case.** A percentile is a promise about the tail, and the tail is
precisely what changed twice this month and has not been re-measured at a useful sample
size since. The rail records no completion time, so nothing samples caller-side latency
systematically either. The old p95 of 12 s described a system that no longer exists, and
republishing it would be worse than publishing nothing.

**Design your timeout against the ceiling, not the median.** The signing round's wall is
30 s server-side, so a roll may legitimately take that long. The client waits 45 s; reads
(entitlement, availability) time out at 15 s because they wait on no signing round.

## 2. Topology — decide before you write code

**Server-authoritative is the only supported shape.** Your backend holds the PTXPal key and
sends `X-PTXPal-Key`; your game client talks to your backend and never sees the key.

A Godot export is decompilable. Extracting a `.pck` and recovering GDScript is routine
tooling, not an attack, so a key in a client is a public credential attached to plays
someone paid for. Direct-from-client needs per-install scoped tokens with caps and
revocation; none exist, so it is **not supported yet** rather than supported with a warning.

## 3. The rail owns the roll shape — design around it

Every roll is `count=1, unique=false, no excludes`, over a **range fixed per app**. You do
not pass count, low, high, unique or exclude, and this is a guarantee rather than a
limitation: the one PTX failure that charges a fee and returns nothing is unreachable by
customer input.

So you **map**, you do not parametrise. One value in the app's range arrives; you turn it
into your outcome:

```gdscript
# app range 1..10000 -> a loot tier
func tier(n: int) -> String:
    if n > 9700: return "LEGENDARY"     # 3%
    if n > 8500: return "Rare"          # 12%
    if n > 5000: return "Uncommon"      # 35%
    return "Common"                     # 50%
```

Mapping in your own code is also what lets a player check you: publish the thresholds, and
the chain proves the number. `MAPPING_SPEC.md` is the worked version of this.

> ★ **Open, and not mine to answer: one title needing two different ranges** — a d6 and a
> d20 in the same game. Under the fixed shape that is two apps with different ranges, or a
> change to the guarantee. Do not design around a guess; ask the operator.

## 4. The async reality

Not `randi()`. An HTTP call that waits on a distributed signing round.

- **Never call it in `_process`.** One roll per game event, awaited.
- **`tag` is where your game's meaning goes.** The rail composes the on-chain identifier as
  `<app>:<tag>:q=<seq>:p=<identity>` and the quorum signs it, so whatever you put in `tag`
  is committed *before* the result exists. Keep it short; the whole string is capped at 128
  bytes.
- **You do not have to make anything unique.** The rail assigns a strictly increasing
  sequence at dequeue and folds it into the signed identifier, so two identical requests in
  one block cannot collide. The client's in-flight guard is not about randomness — it stops
  a double-tapped button spending two entitlements on one event.
- **`request_id` is the idempotency key.** A replay returns the original result and never
  spends a second roll. On a timeout, re-send the same one; never issue a fresh one for the
  same event.

## 5. Failure policy is a design decision, not an error handler

The beacon can be unavailable, and your game must do something. **You** choose:

| Policy | Use when | Cost |
|---|---|---|
| **Block** | the outcome is the game — a raid, a duel, a jackpot | the player waits and sees failures |
| **Queue** | the outcome can land later — daily rewards, mailed loot | durable state and a worker |
| **Local fallback, marked unverified** | cosmetic, low-stakes drops | you must *tell the player*, and never mix silently |

Honour `charged`: it refers to **your entitlement count**. A failure that spent a roll must
not be retried automatically. Some failures cost the operator a chain fee and cost you
nothing — that is their ledger, and it is why the field means one thing only.

## 6. Show the verify link

Put the roll's transaction in front of the player on anything that matters, one link beside
the result. It is what separates this from a number you could have made up.

## 6b. Local development, and the two knobs that matter

`dev_mode` runs everything locally: no network, no entitlement spent. Two settings are not
optional if the rehearsal is to mean anything.

**`dev_range_high` — set it to your app's range.** The rail fixes the range per app, so
there is no correct default and the client does not invent one: left at 0, every dev roll
fails loudly. That is deliberate. A stub quietly drawing 1..10000 for an app whose range is
1..20 gives you a mapping that passes in development and breaks in production, which is the
worst failure a dev mode can have.

**`dev_rolls_remaining` — set it to 0 sometimes.** Running out is a production outage for
your game, it is on the checklist below, and it was previously the one path you could not
rehearse locally because the client always answered 999. Now you can exercise it.

Dev rolls **vary by default**, because the usual dev task is exercising an outcome table
and a value fixed per event shows one branch of it forever while looking like working code.
Set `dev_seed` to any non-zero value when you want a run to repeat exactly — a regression
test, or a bug report someone else has to reproduce.

★ `dev_mode` refuses to start in a release export. There is deliberately **no API key field
on the client at all**: your backend holds the key, and a field that does not exist cannot
be shipped inside a `.pck`.

## 7. Going live

- [ ] Key on your backend only; no key in any export.
- [ ] `dev_mode` off; confirm a release build refuses to start if it is on.
- [ ] Outcome mapping published where players can read it.
- [ ] Failure policy chosen, written down, visible to the player where it matters.
- [ ] Timeout left at 45 s.
- [ ] Alerting on rolls remaining: running out is a production outage for your game.
- [ ] Verify link shown.

---

## Honest limits

- **Public testnet.** tHMS has no monetary value and the chain may be reset.
- **One host, one wallet, no failover.** The rail runs on a single VM with its own node and
  its own spend-only pool. If that VM is down, every developer is down. No SLA.
- **Rolls are serialised globally — one at a time, across every customer.** ★ That part is
  structural and certain: a single process-wide lock in the rail, not a quota and not a
  tuning choice. The *rate* attached to it is arithmetic on the median above — about a
  second a roll puts the whole system's ceiling on the order of 60 a minute — and it is a
  **ceiling under no contention, shared by everyone**, not an allocation you can plan
  against. Treat the serialisation as the fact and the number as its consequence. If you
  need guaranteed throughput, the thing to ask for is not a larger number; it is a second
  rail.
- **Calling availability in a loop is pointless — and that is now the only reason.** The
  defect that made it actively harmful is fixed (BUG-089, 2026-09-16): the count is cached
  server-side, so requests inside the window share one wallet call between them instead of
  each costing one. What remains is that polling faster than the cache window returns you
  the same number, and that a roll performs its own authoritative check regardless — so a
  "yes" here is a pre-flight hint, never a guarantee. Call it to show a player the rail is
  up. Do not call it before every roll.
- **About 1.1 % of rolls have failed after the operator's fee was spent** — 8 of 730 real
  rolls, 2026-09-11 to 09-13. ★ **Two defect numbers, because they are two different
  things:** the *cause* is **BUG-084**, which is open; the *count*, and the rail's
  mis-reporting of those failures, is **BUG-085**, whose message was fixed on 2026-09-16.
  Your entitlement is returned either way. The operator's fee is not.
- **Open defects you inherit:** **BUG-084** (a commitment mined mid-round is refused as
  terminal and the round fails after the fee is paid — the live one, above); **BUG-051**
  (the published seed formula names an input the code does not use, so a verifier built
  from the written specification derives the wrong seed — build from the implementation);
  **ODC-090** (a bad partial signature fails a round with no attribution); **ODC-117** (the
  public verifier sits behind Cloudflare and 403s a bare `python-urllib` — send a
  User-Agent).
- **Fixed since this guide was first written**, listed so you do not act on advice that was
  true last week: **BUG-085** — the refusal now distinguishes a free refusal from one where
  the fee was spent, and carries `charged`, so you can tell them apart instead of reading a
  message that claimed "pre-broadcast, nothing deducted" for both. **BUG-089** —
  availability, above. **BUG-086/087** — the sign-round stalls behind the old latency
  figures.
