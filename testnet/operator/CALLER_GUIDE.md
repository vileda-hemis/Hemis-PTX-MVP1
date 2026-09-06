# Calling PTX — a guide for integrators

This is for someone who wants **randomness out of this chain**, not for someone running a
gamemaster. If you operate a gamemaster, you want `OPERATOR_GUIDE.md` instead.

Everything below was measured on ptxtestnet, not inferred from source. Where a claim came from a
live experiment the numbers are the ones that were observed.

---

## 1. What a caller actually needs

**Confirmed coins in a wallet.** That is the real requirement. A caller is **not** a gamemaster,
does not need a collateral, does not need to be registered, and is not paid anything.

You also currently need two config lines, and it is honest to say what they are:

```
ptxnodeid=<any label>
ptxnode=<any-id>@<any-host>:<port>
```

★ **Neither does anything on the roll path.** `ptxnodeid` is checked once (`rpc/ptx.cpp:159`) and
then never read again — it reaches neither transaction and no verifier can see it. `ptxnode` is a
leftover from an architecture that was replaced: members are found from their on-chain addresses,
and the value you put here is never dialled. They are friction, not requirements, and they are
documented here so you are not misled into thinking they matter. Both need a restart to take effect.

---

## 1a. Three ways to integrate, and the first one needs nothing

**1. Verify-only — start here.** You are handed a settle txid by someone else and you want to prove
the numbers are real. **You need no wallet, no coins, no account and nothing from us.**

```
https://ptx-explorer.lnky.uk/v2?q=<settle txid>
https://ptx-explorer.lnky.uk/v2/api/v1/tx/<settle txid>
```

Both fetch the transaction, decode it and re-derive the result from the bytes. The API returns JSON
with the parameters, the results and the checks. ★ This is the lowest-effort integration and the one
most games will start with — a player disputes a draw, you hand them a URL.

★★ **The lookup uses a node; the checks do not.** The page fetches the transaction from a node its
operator runs, then verifies it with arithmetic alone — no chain, no index. If you would rather not
depend on that node at all, pass the raw `extraPayload` hex instead of a txid and the identical
checks run with no lookup at all.

**2. Self-hosted backend.** You want to *make* rolls, so you run a wallet with coins and call
`ptx_roll` yourself. Everything else in this guide is about this model.

★★★ **The wallet belongs on your server, never in the game client.** A funded key shipped in a build
is a funded key in every player's hands, and the funding ends the same day. The client asks your
backend for a roll; your backend calls the chain and returns the txid; the client can verify it with
model 1.

**3. Broker — does not exist.** There is no service that will roll on your behalf for a fee. If you
need rolls, you need model 2 or someone running it for you privately. Said plainly so nobody plans
around a component that is not there.

---

## 1b. Batching, throughput and what you actually wait for

★★★ **One roll can return up to 1000 values for one fee.** That is the answer to "I need randomness
every frame" — you do not roll per action, you roll a batch and consume it.

**Measured:** `ptx_roll 1000 1 100000 false '[]' batch-test ""` returned **1000 values in 0.645 s**,
for the same 1 HMS any other roll costs.

* The hard cap is **`count` ≤ 1000** (`count N exceeds maximum 1000`).
* A second cap can bind first: `game_id + 8×(int excludes) + 65×(txid excludes) + 8×count ≤ 9000`.
  At `count=1000` that is 8000 bytes of the budget, leaving ~1000 for everything else.

**The two latencies are very different, and the second is the one you design around:**

| | |
|---|---|
| `ptx_roll` returns | **under a second** — measured 0.3–1.5 s, including the batch above |
| the settle **confirms** | **the next block** — a 60-second target, so 0–60 s, ~30 s typical |

★ The call returns as soon as the quorum has signed and both transactions are broadcast. The results
are final at that moment — a reorg cannot change them, because the seed and signature are
self-contained. What the block gives you is *confirmation*, not the answer.

★★ So: **do not make a player wait for confirmation to see a result.** Show the result on return,
and treat confirmation as the point at which it becomes independently verifiable by anyone. If your
game needs the draw settled before it pays out, budget a minute, not a second.

---

## 2. The call

```
ptx_roll count low high unique exclude game_id caller_salt
```

A real, working invocation:

```
Hemis-cli ptx_roll 6 1 49 true '[13]' my-game-name deadbeef
```

Six distinct numbers from 1 to 49, never 13.

| argument | meaning |
|---|---|
| `count` | how many values to draw (1–1000) |
| `low` / `high` | inclusive range |
| `unique` | `true` = all distinct; `false` = repeats allowed |
| `exclude` | JSON array — see §5, it accepts two different things |
| `game_id` | your label, free-form — see §4 |
| `caller_salt` | hex, or `""` — your entropy contribution |

`unique` selects between two genuinely different algorithms: `true` shuffles the eligible pool,
`false` draws independently with rejection. Both are verifiable; they simply produce different
distributions.

---

## 3. What it costs, and what can fail

**Every roll costs 1 HMS.** The fee is paid at the *commitment*, before any result exists — that is
deliberate, and it is what stops a caller previewing a result and discarding it.

A roll is **two transactions**: a commitment that pays the fee, and a settle that publishes the
result. `tx_id` in the response is the **settle**.

### Failures that cost you nothing

These stop before the commitment is broadcast:

* Bad arguments — range, count, exclude format, payload budget.
* `-32050 commitment input N not in the confirmed UTXO set (not yet confirmed, or spent)` —
  see §6. This is the common one and it is free.

### ★ The failure that costs 1 HMS

If your parameters leave **fewer eligible values than `count` on a `unique` draw**, the roll fails
with `pool too small for unique draw` — and it fails **after** the commitment is broadcast and the
quorum has signed. **You are charged and you get no result.**

The same applies to `exclude set covers entire range` on a non-unique draw.

**Check your own arithmetic before calling**, because nothing checks it for you:

```
eligible = (high - low + 1) - (number of excluded values in range)
unique draw requires:  eligible >= count
```

`ptx_roll 6 1 8 true '[2]'` is safe — 7 eligible for 6 draws. `ptx_roll 6 1 6 true '[2]'` is not —
5 eligible for 6 draws, and it will take your fee before telling you.

---

## 4. `game_id`

* **Cap 128 bytes.** It also shares a 9000-byte budget with your exclusions and results:
  `game_id + 8×(integer excludes) + 65×(txid excludes) + 8×count ≤ 9000`.
* **It is stored byte-identical and permanently.** Measured: 112 bytes sent, 112 bytes on chain,
  unchanged.
* **It is hashed into the round seed**, so it is bound to the signed value rather than being a
  loose label.

★★ **There is no validation of its content — only its length.** Anything you put there reaches
every consumer intact: explorers, APIs, chat bots, your own UI.

**If you render a `game_id` you did not create, escape it.** It can contain markup, control
characters, or anything that looks like a link. This chain will not sanitise it for you and the
value is permanent.

---

## 5. `exclude` — integers only

Values never to draw:

```
'[13]'          exclude 13
'[1, 2, 3]'     exclude 1, 2 and 3
```

They are excluded from the draw and folded into `params_hash`, so the seed commits to exactly the
exclusions that were applied. Remember §3: an exclusion that leaves fewer eligible values than
`count` on a `unique` draw costs you 1 HMS.

### ★★ Transaction ids are NOT accepted

You may see `exclude_txids` in the payload structure, and older help text described 64-character
transaction ids as a way to exclude everything a previous roll produced. **That is not implemented,
and `ptx_roll` now refuses it:**

```
error -32602: tx_id exclusions are not implemented — use integer exclusions
```

★ The refusal is free — it happens before the commitment is built, so nothing is broadcast and no
fee is paid.

★★★ **Why it is refused rather than ignored, because the history matters if you are reading the
payload format.** Until 2026-09-06 a tx_id was *accepted*: written into both payloads, folded into
`params_hash`, and therefore committed to by the seed the quorum signed — and then **silently
dropped** before the draw. Measured: excluding a roll that produced `[6,3,1,7,8,4]` from a 1–8 draw
should have forced the result to `{2,5}`; it returned `[7,1]`, both inside the excluded set, while
the payload recorded the exclusion. The chain was attesting to something it had not done. Refusing
the input removes that possibility entirely — `exclude_txids` is now empty in every payload.

★ If you need uniqueness *across* rolls, do it caller-side: keep the previous results and pass them
as integers, within the 9000-byte budget.

---

## 6. Throughput: the confirmed-coin cycle

**Each roll spends exactly one confirmed coin, and returns its change unconfirmed.** Until a block
confirms that change, the coin is not spendable again.

So your sustainable rate is **the number of confirmed coins you hold**, and it refills each block.

★ **A single large coin funds exactly one roll at a time, the same as a small one.** Holding 69,000
HMS in one output gives you one roll per block. Holding it in twenty outputs gives you twenty.
**If you need a higher rate, hold more coins — not bigger ones.**

Check with `listunspent 1` and count the outputs, **not** `getbalance`. A wallet showing a large
balance can legitimately have zero spendable coins because it is all unconfirmed change.

**Measured, on a wallet funded with exactly 4 coins:**

```
roll 1 · 4 confirmed coins · succeeded
roll 2 · 3 confirmed coins · succeeded
roll 3 · 2 confirmed coins · succeeded
roll 4 · 1 confirmed coin  · succeeded
roll 5 · 0 confirmed coins · -32050, free, no commitment broadcast
```

Four fees across five attempts. The boundary is cheap to probe deliberately.

---

## 7. `caller_salt`

Hex, or empty. It is mixed into the round seed via a nonce, so it is your contribution to the
entropy — but it is **not** what makes the result unpredictable. The result comes from a threshold
signature no single party can produce.

**An empty salt is valid and works.** Verified: `ptx_roll 1 1 49 false '[]' <game_id> ""` succeeds.

---

## 8. What you can verify, and what you cannot

Every roll is checkable from the transaction bytes alone, by you, with no node:

```
https://ptx-explorer.lnky.uk/v2?q=<settle txid>
https://ptx-explorer.lnky.uk/v2/api/v1/tx/<settle txid>
```

| check | claim |
|---|---|
| **P** | `ptx_params_hash` matches the parameters you asked for |
| **A** | `round_seed` is the hash of `game_id`, height, nonce and params |
| **B** | `beacon == SHA256(quorum_sig)` |
| **C** | `results` follow from the beacon |
| **D** | *not performed by the page* — it needs the quorum's key |

★ Check D is performed **by consensus**, which verifies the threshold signature against the
quorum's committed group key. The page does not claim credit for it.

### ★★ What is not verified by anyone

**Consensus checks the signature over the seed. It does not re-derive the beacon from the signature,
nor the results from the beacon.** Those are checks B and C, and they exist only off-chain — in the
verifier above. A settle whose results did not follow from its beacon would still be accepted by the
network; it would simply fail check C for anyone who looked.

So: **the signature proves a quorum signed that seed. Checks B and C are what tie the numbers to
it, and running them is on you.**

★★★ **And check C confirms consistency, not correctness.** It recomputes the results from the
beacon using the parameters in the payload — so it catches numbers that do not follow from the
beacon, and it cannot catch a payload whose *parameters* misdescribe what was asked for. This is not
hypothetical: when tx_id exclusions were silently dropped (§5), check C **passed** on a roll that
ignored its own recorded exclusion, because the verifier computed with an empty exclusion set and so
had the node. Both agreed on a result honouring nothing. Check P is what binds the parameters to the
seed, which is why it is checked first and why it exists at all.

### ★ `caller_pubkey`

You will see `caller_pubkey` in check A's formula. **You cannot set it and it is always empty.** The
seed derivation has a slot for a caller identity and `ptx_roll` passes nothing into it.

**No caller identity of any kind reaches the chain.** Not your node id, not an address, nothing.
`game_id` and `caller_salt` are the only caller-controlled values that are recorded — so if you need
to tie a roll to a player, a session or a match, **it has to go in one of those two**, and it cannot
be recovered from anything else.
