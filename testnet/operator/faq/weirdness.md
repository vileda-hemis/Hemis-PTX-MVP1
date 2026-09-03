<!-- CORPUS-AUTHORED: expected weirdness -->
<!-- CORPUS-TAG: v0.3.4-testnet -->

# Things that look broken and are not

Every entry below is a case where ptxtestnet behaves correctly and an operator reasonably
reads it as a fault. **Each entry is self-contained** — it states the wrong reading, the right
one, and whether to act. Nothing here says "see section 4"; if a condition matters it is
repeated in the entry that needs it.

★ **These entries exist because the obvious reading is wrong.** When answering from them, give
the correction *and* the reading it corrects. Dropping the wrong reading loses the point of the
entry.

---

## Symptom: my node has 0 connections and stays at height 0

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as "the network is down" or "my firewall is
blocking me". Almost always it is neither.

**What is actually happening — most likely, an `addnode` typo.** This network has **no peer
discovery of any kind**: no DNS seeds, no fixed seeds. Your node dials *only* the `addnode=` lines
in your config. If they are wrong, absent, or outside the `[ptxtestnet]` section header, your node
has nothing to contact and will sit at height 0 forever, silently.

★ **The specific typo that has bitten this network twice in two days is a missing colon in an IPv6
address.** `[2a07:244:46:6400:9100]` looks correct and is not — it has five groups where a full
IPv6 address needs eight, and the shorthand for the missing ones is a *double* colon:
`[2a07:244:46:6400::9100]`. A coordinator host ran for a full day at height 0 with exactly this,
its only peer unparseable. **Nothing logs it as an error.**

**Do you need to act?** Yes.
1. `Hemis-cli getconnectioncount` — if it is 0, this is you.
2. Check every `addnode=` line has a **double** colon where the address is abbreviated.
3. Check the lines are **below** the `[ptxtestnet]` section header. Lines above it are silently
   ignored.
4. Restart, then check `getconnectioncount` again.

---

## Symptom: `self-check.sh` section 5 says my node is NOT reachable, but it is

**Applies to:** all versions.

**The obvious reading is wrong.** You will read a section 5 FAIL as "nobody can reach me, my
firewall is broken". It may mean the opposite of what you think.

**What is actually happening.** Section 5 connects **from your own machine to your own registered
address**. On many NAT setups that succeeds via hairpin routing even when nobody outside can reach
you — and **on some it fails even when outsiders can reach you perfectly well**. A coordinator
gamemaster fails its own section 5 today while two independent external hosts reach its P2P port.

**Do you need to act?** **Check from outside before changing anything.** Ask another operator to
connect to your address, or use an external port checker. Only if *they* cannot reach you is there
something to fix. Editing firewall rules on the strength of a section 5 FAIL alone can break a
working node.

★ A section 5 **PASS** is encouraging but is not proof either, for the same reason. The two
definitive tests are another operator reaching you, and your PoSe score staying at zero.

---

## Symptom: `getinfo` says `"testnet": false` — am I on mainnet?

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as "I am on mainnet". **You are not.**

**What is actually happening.** `getinfo`'s `testnet` field reports `Params().IsTestnet()`, which is
defined as `NetworkIDString() == "test"` — i.e. **the Hemis testnet specifically**. ptxtestnet is a
*different network* with its own id, so that comparison is correctly `false`. The daemon's own
`IsTestChain()` helper *does* count ptxtestnet as a test chain; the `getinfo` field simply is not
that helper.

**Do you need to act?** No. To confirm which network you are actually on, check that your config
has `ptxtestnet=1` and that your datadir contains a `ptxtestnet/` subdirectory.

★ **Do not "fix" this by setting `testnet=1`.** That selects the Hemis testnet — a different chain
with different magic bytes — and gives you a perfectly healthy daemon on the wrong network.

---

## Symptom: peers report `Hemis Core:1.3.1` but I installed v0.3.4-testnet

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as "my peers are running old software" or "my
install did not take".

**What is actually happening.** `1.3.1` is the inherited Hemis Core lineage version carried in the
P2P subversion string. It is not the PTX release number. A node built from `v0.3.4-testnet`
advertises the lineage version to peers while `Hemisd -version` reports `v0.3.4-testnet`.

**Do you need to act?** No. To check what a node is really running, use `Hemisd -version` on that
machine — not the peer list.

---

## Symptom: my registration was rejected for an "invalid value" and the error does not say what value was needed

**Applies to:** all versions.

**The obvious reading is wrong.** You will look for the required amount in the error message. **It
is not there.** The message prints what you sent, never what was expected.

**What is actually happening.** The collateral must be **exactly 100 HMS** in a **single unspent
output**. The check is `!=`, not "at least". Both the RPC and the consensus rule reject any other
amount, and neither message contains the number 100.

★ Two payments of 50 do not combine. A 100 HMS output your wallet later consolidates is no longer a
collateral.

**Do you need to act?** Yes — send exactly 100 HMS to a fresh address, wait one confirmation, and
use `Hemis-cli listunspent 1 9999999 '["<that address>"]'` to find the entry showing
`100.00000000`.

---

## Symptom: `getnewaddress` gave me an address starting with `x`, not `y`

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as a corrupt or wrong-network address.

**What is actually happening.** ptxtestnet's public-key address version byte is **139**, and base58
encoding of that byte usually produces a leading `y` but sometimes an `x`, depending on the rest of
the address. Both are valid ptxtestnet addresses. The gamemaster registered on this network today
has an `x`-prefixed owner address, and `validateaddress` returns `"isvalid": true` for it.

**Do you need to act?** No. If you want certainty for any address, run
`Hemis-cli validateaddress <address>` and check `isvalid`.

---

## Symptom: "Insufficient funds" although I can see the coins arrived

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as the wallet losing your coins.

**What is actually happening.** `getbalance` counts **confirmed** coins only. A transfer appears in
`listtransactions` as soon as it is seen, but cannot be spent until it is in a block. Blocks are
about a minute apart.

**Do you need to act?** Wait for one confirmation and check `Hemis-cli getbalance`. When it shows
the amount, you can spend it.

---

## Symptom: a command answered, but with information from the wrong node

**Applies to:** all versions. ★ This one is dangerous because the answer looks fine.

**The obvious reading is wrong.** You will trust the answer because a command that returns JSON
looks like it worked.

**What is actually happening.** A **gamemaster-role** install has RPC credentials for its own
datadir and nothing else. A `Hemis-cli` command run without `-datadir` may pick up a different
config — or reach a different daemon on the same host — and answer from it. Two hosts were reported
as running the wrong software version this way before the mistake was caught.

★ Config keys like `rpcport` are also **network-section-scoped**: outside `[ptxtestnet]` the daemon
ignores them, warns once at startup, and binds the ptxtestnet defaults instead — so a node can be
listening on ports you did not ask for while looking healthy from the inside.

**Do you need to act?** Always pass the datadir explicitly:
`Hemis-cli -datadir=$HOME/.Hemis <command>`. If an answer surprises you, re-run it that way before
believing it.

---

## Symptom: `lastPaidHeight` keeps advancing but I have received nothing

**Applies to:** all versions.

**The obvious reading is wrong.** The field is named as though it means "the height at which I was
last paid". It does not.

**What is actually happening.** `nLastPaidHeight` is written when a gamemaster is **selected as the
payee for a block**, in the block-connection path. There is no check in that code for whether
gamemaster payments are actually enabled. So on a network where payments are switched off, the
field advances for selected gamemasters while nothing is paid to anyone.

**Do you need to act?** No. Judge payment by your payout address's balance, not by this field.

---

## Symptom: everything is registered and `Ready`, but no quorums exist and nothing happens

**Applies to:** all versions.

**The obvious reading is wrong.** You will read an empty `ptx_quorum_list` as a fault in your node.

**What is actually happening.** A quorum needs **11 registered gamemasters**. Below that the code
that forms quorums does not run at all, so `ptx_quorum_list` is correctly empty and no rolls can be
signed. Your node being registered, armed and `Ready` with nothing happening is the **expected**
state until the network reaches eleven.

**Do you need to act?** No. Ask the coordinator how many gamemasters are registered if you want to
know how far off it is.

---

## Symptom: I registered without `ptxPaymentAddress` — can I add it now?

**Applies to:** all versions. ★ **No, and this is the one mistake that cannot be undone in place.**

**The obvious reading is wrong.** You will look for an update command. There is not one for this
field, and **the registration response does not show its absence**, so nothing tells you it is
missing.

**What is actually happening.** `ptxPaymentAddress` is registration-time state. A gamemaster
without it earns block rewards normally but is **not eligible for PTX lottery payouts**, and no
`protx_update_*` command sets it.

**Do you need to act?** Yes, if you want lottery eligibility: **re-register**. The collateral is not
spent by a failed or incomplete registration, so it can be reused. Setting `ptxPaymentAddress` to
the same value as your payout address is fine and is what most operators should do.

---

## Symptom: my provider only offers IPv4 — can I run a gamemaster?

**Applies to:** v0.3.3-testnet and later, where `install.sh` enforces this.

**The obvious reading is wrong.** You will expect a node with a working internet connection to be
able to participate, and you may expect a second, dual-stack machine to bridge the gap. **It cannot.**

**What is actually happening.** Signing is **point-to-point**: the caller connects directly to the
address you registered, and no relay carries that connection — not a peer, not the coordinator, not
a dual-stack node you add later. A gamemaster on the wrong address family is not a degraded
participant; **for those peers it does not exist**, while still syncing and reporting `Ready`.
Worse, a gamemaster that cannot exchange quorum-formation traffic can be marked a failed participant
and PoSe-banned for a network topology it did not choose.

★ A **wallet** host needs IPv6 too, for a simpler reason: the seed peers are IPv6, and this network
has no peer discovery, so an IPv4-only wallet host has nothing to dial.

★ Addresses starting `fd` are ULA. Linux reports their scope as "global" and they are **not**
routable — they do not count.

**Do you need to act?** Yes. Get IPv6 on the host — most providers enable it free on request. If
yours cannot, tell the coordinator **before** provisioning. `install.sh` refuses such a host and
refuses before writing anything, so a wrong machine is left clean.

---

## Symptom: I cloned v0.3.3-testnet and its instructions told me to clone v0.3.2-testnet

**Applies to:** the `v0.3.3-testnet` tag only. Fixed in `v0.3.4-testnet`.

**The obvious reading is wrong.** You will wonder which tag is correct, or assume you misread.

**What is actually happening.** `v0.3.3-testnet` shipped with a one-page onboarding document whose
clone command named the previous tag. It was a documentation defect in that release, tracked as
BUG-060 and fixed in `v0.3.4-testnet`.

**Do you need to act?** Use **`v0.3.4-testnet` or later**. If you already installed from
`v0.3.2` or `v0.3.3`, nothing on your node is wrong because of this — the binaries in those
releases are sound — but re-clone at the current tag so the documents you are following match the
software.
