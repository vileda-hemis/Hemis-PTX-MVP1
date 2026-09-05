<!-- CORPUS-AUTHORED: protocol explanation -->
<!-- CORPUS-TAG: v0.4.0-testnet -->

# What PTX actually does

Written for someone asking a question, not for someone who already knows. Each section stands
alone.

★ **Where this disagrees with the copied operator documents in `derived/`, those win.** They are
verbatim copies of the authoritative guides; this file is explanation.

---

## What is a gamemaster, in one paragraph

A gamemaster is a node that has locked **100 HMS** of collateral on chain and registered an
identity — an address peers can reach it at, and a BLS key it signs with. Registration puts that
identity in a list every node agrees on. Gamemasters are the set from which quorums are drawn.

★ The collateral is **not spent and not sent to anyone.** It stays in your wallet. It is a stake,
not a fee.

★★ **It is locked against your own staker only once the registration confirms** — not before. In the
gap between funding the collateral and the registration confirming it is an ordinary spendable coin,
and the registration transaction itself can spend it to pay its own fee. That is what halted the
chain on 2026-09-05. After registering, check the collateral is not among that transaction's own
inputs: `Hemis-cli getrawtransaction <your-protx-txid> 1`. If it is, that registration will never
confirm — re-fund and register again.

---

## What is a quorum

A quorum is a group of **11 gamemasters** chosen to hold shares of a single signing key. They are
selected deterministically from the registered set, so every node computes the same membership
without anyone announcing it.

The 11 members run a **distributed key generation** ceremony: each contributes secret material, and
at the end each holds one *share* of a key **that nobody ever holds in full**. There is no moment at
which the whole private key exists on any machine, including the coordinator's.

★ **Below 11 registered gamemasters, no quorum can form at all** — the formation code does not run.
An empty quorum list on a small network is expected, not a fault.

---

## Why threshold signatures, rather than one signer

A signature from a single node is only as trustworthy as that node. Threshold BLS lets **any 6 of
the 11** members combine their partial signatures into one signature that verifies against the
quorum's single public key — while **5 colluding members can produce nothing at all**.

That gives two properties at once:

- **No single point of trust.** No member, and no operator, can sign alone.
- **No single point of failure.** Five members can be offline, banned or hostile and the network
  still signs.

★ The combined signature is indistinguishable from any other signature by that quorum. A verifier
checking it cannot tell *which* six members produced it, and does not need to.

---

## What a roll is, and what it proves

A roll is a random draw the chain can prove it did not choose.

1. A caller publishes a **commitment**: the parameters of the draw and a seed, fixed in a
   transaction *before* anything is signed.
2. The quorum signs that seed. The signature is a **threshold BLS signature** — it required six
   independent members and no one of them could have produced it.
3. The **beacon** is the hash of that signature.
4. The **results** are derived from the beacon by a fixed, public mapping.

★★ **The point is that step 2 has exactly one possible answer.** A BLS signature over a given
message by a given key is unique — there is no nonce to grind, no choice to make. So nobody, caller
or quorum member, can steer the outcome by trying again. The commitment fixes the question before
the answer exists, and the answer is forced.

**What it does not prove:** that the caller's parameters were fair, or that the game using the
result honoured it. It proves the *draw* was not manipulated.

---

## How anyone can check a roll themselves

Every roll's inputs and outputs are in the transaction. The public verifier at
`https://ptx-explorer.lnky.uk/v2` re-derives them from raw bytes and reports four checks:

- **P** — the parameters hash matches the parameters.
- **A** — the seed is the hash of the declared inputs.
- **B** — the beacon is the hash of the quorum signature.
- **C** — the results are what the mapping produces from that beacon.

★ **A fifth check, D — that the signature verifies under the quorum's key — is deliberately reported
as NOT PERFORMED**, because it needs a synced node and the verifier has none. It is shown as "not
performed" rather than omitted or quietly passed, so nobody mistakes an unchecked claim for a
checked one.

★ Every API response includes the raw payload bytes, so a caller can re-derive the answer instead of
trusting the service. That is deliberate: a verifier you have to trust is not a verifier.

---

## What PoSe is, and what gets you banned

PoSe — "proof of service" — is the network's own verdict on whether your gamemaster is doing its
job. Penalties accrue when your node fails to participate in quorum formation. Accumulate enough and
your gamemaster is **banned**: it stops being selected and stops earning.

★ **The usual cause is unreachability, not misbehaviour.** A node that is firewalled, on the wrong
address family, or simply down cannot participate, and the network cannot tell that apart from a
node that will not.

★ Recovery is possible and does not cost you your collateral. It runs from your **wallet** host and
needs your BLS secret — a banned gamemaster cannot supply its own key, and it has no wallet to pay
the fee with. That is why the operator guide tells you to keep a copy of that secret somewhere off
the gamemaster.

★ Your PoSe score is the one health check that is **the network's opinion rather than your
machine's**, which is why it is worth more than any test your own node can run on itself.
