<!-- CORPUS-AUTHORED: question index -->
<!-- CORPUS-TAG: v0.3.4-testnet -->

# How operators actually phrase things

★ **This is a retrieval aid, not content.** Every answer below lives somewhere else in this corpus;
this file only maps the words operators use onto the place that holds the answer. It deliberately
restates nothing — if it disagreed with the entry it points at, the entry would win.

★★ **Two rules about using it, and the second matters more than the first.**

1. If a question resembles anything on this list, **the corpus covers it — go and answer it.** These
   are real phrasings that were previously refused or relayed, so treating them as uncovered is a
   known mistake, not a judgement call.
2. ★★ **This list is NOT exhaustive, and absence from it is NOT evidence that the corpus lacks
   something.** Most of what the corpus covers is not indexed here. **Never cite this file as a
   reason to refuse.** A question that is not listed still gets the full three-step search, and a
   genuine gap in an operational question is still tier 4 — that route is how the documentation gets
   written, and suppressing it would be worse than any wrong answer.

★ Entries are named by their titles rather than by line numbers, because line numbers drift and
titles do not.

---

## "it won't sync" / "it just sits there" / "no peers" / "stuck at 0 blocks"

★ All the same question, and it is **covered**.
→ `weirdness.md` **"my node has 0 connections and stays at height 0"**

Nothing is discoverable on this network: no DNS seeds, no fixed seeds. Check for `addnode=` lines at
all, that they sit **under** the `[ptxtestnet]` header, and the abbreviated-IPv6 double colon.

## "is my gamemaster actually working?" / "how do I know it's doing anything?" / "nothing is happening"

★ **Covered, in three places, and the answer is layered.**
→ `weirdness.md` **"everything is registered and `Ready`, but no quorums exist and nothing happens"**
→ `weirdness.md` **"my PoSe penalty is `0` — does that mean my gamemaster is working?"**
→ `OPERATOR_GUIDE.md` **"Things that are NOT what they look like"** — run `self-check.sh` and believe
its exit code; report a FAIL, a `[????]`, or a non-zero PoSe penalty, and nothing else.

★ `Ready` means armed, not working, and quiet is usually correct.

## "can I run two gamemasters on one machine?" / "do I really need a separate VPS each?"

★ **Covered, and the answer is a policy rather than a mechanism** — say which it is.
→ `OPERATOR_GUIDE.md` **"One GM per host, one routable address per GM"**

The mechanical blocker was removed, so co-hosting is now *possible* and simply **untested**: a
supported-configuration boundary, not a technical impossibility. Do not present it as either "it
cannot work" or "sure, go ahead".

## "I sent the coins but it wouldn't register" / "invalid value" / "how much collateral?"

★ **Covered.**
→ `weirdness.md` **"my registration was rejected for an \"invalid value\"…"**
→ `OPERATOR_GUIDE.md` collateral section

Exactly **100 HMS**, one single unspent output, checked with `!=`. 1000 is mainnet's figure and is
the number people assume.

## "insufficient funds but the coins are there" / "balance is wrong"

★ **Covered.**
→ `weirdness.md` **"\"Insufficient funds\" although I can see the coins arrived"**

## "my host is IPv4 only" / "do I need IPv6?"

★ **Covered, and the answer is a hard requirement.**
→ `weirdness.md` **"my provider only offers IPv4 — can I run a gamemaster?"**

## "am I on mainnet?" / "`testnet` says false"

★ **Covered.**
→ `weirdness.md` **"`getinfo` says `\"testnet\": false` — am I on mainnet?"**

## "I haven't been paid" / "`lastPaidHeight` is going up"

★ **Covered.**
→ `weirdness.md` **"`lastPaidHeight` keeps advancing but I have received nothing"**

## "how do I upgrade?" / "there's a new tag"

★ **Covered.**
→ `OPERATOR_GUIDE.md` **"Part D — Upgrading to a new tag"**
→ `weirdness.md` **"I cloned v0.3.3-testnet and its instructions told me to clone v0.3.2-testnet"**

## "my gamemaster got banned" / "PoSe banned"

★ **Covered, and this one does not clear by itself.**
→ `OPERATOR_GUIDE.md` **"If your GM is PoSe-banned"** — needs `protx_update_service` from the wallet
host, with the BLS secret.
