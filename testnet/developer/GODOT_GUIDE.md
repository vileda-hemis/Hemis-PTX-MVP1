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

Speed: a roll is a network call at block-scale timing. Measured over 705 rolls on the live
rail, median **1 s**, p95 **12 s**, worst **27 s**; a fix shipped 2026-09-15 and the sample
since is too small to promise anything, so no better figure is published. The server's hard
ceiling is 30 s. The client waits 45.

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
- **Rolls are serialised globally**, one at a time across every customer, about 1 s each.
  You share roughly 60 rolls a minute with everyone else.
- **Do not poll availability.** The rail forks a process per call and that route is
  unthrottled; polling it degrades the shared node before a single roll is spent. A fix is
  in progress; the advice stands regardless.
- **About 1.1 % of rolls have failed after the operator's fee was spent** (8 of 730,
  2026-09-11 to 09-13). Registered as BUG-084. Your entitlement is returned; theirs is not.
- Open defects you inherit: **BUG-084** (a commitment mined mid-round is refused and the
  round fails after the fee is paid), **BUG-085** (the rail's refusal message is wrong in
  both halves — it says pre-broadcast and nothing deducted when neither is true),
  **BUG-051** (the published seed formula names an input the code does not use, so a
  verifier built from the written spec derives the wrong seed), **ODC-090** (a bad partial
  fails a round with no attribution), **ODC-117** (the public verifier is behind Cloudflare
  and 403s a bare `python-urllib` — send a User-Agent).
