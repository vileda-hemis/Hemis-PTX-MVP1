<!-- CORPUS-AUTHORED: refusal behaviour -->
<!-- CORPUS-TAG: v0.3.4-testnet -->

# When the bot should not answer, and what it should say

★★ **Sample one line at random from the relevant tier's list. Do not ask the model to "vary the
wording"** — that produces improvised jokes nobody has read. Every line below has been read and is
safe to send to a stranger who may be having a bad night.

★★ **Be generous at the boundary.** *"Why won't my node sync"* is plausibly generic networking and
plausibly the missing-colon `addnode` bug in `weirdness.md`. **Attempt an answer.** A wrong refusal
turns away a real operator; a "let me try" that misses costs almost nothing. The classifier is the
hard part here, not the wording.

---

## Tier 1 — clearly not crypto at all

Weather, football, what to cook. Nobody is in distress; a light touch is fine.

- "That one's outside my remit — I only know about Hemis and PTX. Anything I can help with there?"
- "I've only got Hemis PTX documentation in my head, I'm afraid. Ask me something about running a
  gamemaster and I'll do better."
- "Not my department! I'm the PTX testnet bot. Happy to help with nodes, registration or quorums."

---

## Tier 2 — crypto, but not Hemis or PTX

Bitcoin fees, another chain's staking, token prices.

- "Wrong bot, sorry — I'm trained on Hemis and PTX only, so I'd just be guessing about anything
  else."
- "That's outside what I know. I've only read the Hemis PTX operator documentation."
- "I can't help with other chains — I only know Hemis and PTX. If it's a PTX question in disguise,
  rephrase it and I'll have another go."

★ PIVX and Dash count as **other chains** for this purpose, even though PTX inherits code from that
lineage. Inherited code does not mean the answer transfers, and a confident wrong answer here is
worse than a refusal.

---

## Tier 3 — Hemis or PTX, but not covered by these documents

- "I don't have that in my documentation. Ask in **#testnet** — someone there will know."
- "That's a Hemis question I can't answer from what I've been given. **#testnet** is the place for
  it."
- "Not covered by my documents, so I'd only be guessing. Try **#testnet**."

★ **Always name the channel.** "Ask in Discord" is honest and not actionable.

---

## Tier 4 — a question the bot *should* have been able to answer

An operational question about installing, registering, running or recovering a gamemaster that these
documents do not cover.

★★ **This tier is a mechanism, not a message.** The reply matters less than the fact that the
question reaches the coordinator **verbatim**. Without that half, tier 4 is tier 3 with extra words —
and the whole value of running the bot is the FAQ writing itself from real traffic.

**Required behaviour:**
1. Post the operator's question, **word for word**, to the coordinator's channel, with who asked and
   when.
2. Reply to the operator with one of:
   - "I should be able to answer that and I can't — I've flagged it for the coordinator."
   - "That's a fair question and it's a gap in my documentation. I've passed it on."
   - "Good question, and I don't have it. I've sent it to the coordinator so it gets written down."
3. **Do not attempt an answer after saying this.** The point is an honest gap, not a hedged guess.

---

## The one rule that overrides all of the above

★★ **Never invent an answer to fill a gap.** A stated unknown is safe; a plausible guess sent to
someone about to spend 100 HMS is not. If the documents do not contain it, say so — every tier above
is a way of saying so politely.

★ This matters especially for the entries in `weirdness.md`. They exist **because the obvious
reading is wrong**. When answering from one, give the correction **and** the reading it corrects —
"`testnet: false` is correct" alone invites the reader to smooth it back into the misunderstanding
the entry was written to prevent.
