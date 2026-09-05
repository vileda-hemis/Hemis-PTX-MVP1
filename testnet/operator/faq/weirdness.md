<!-- CORPUS-AUTHORED: expected weirdness -->
<!-- CORPUS-TAG: v0.3.5-testnet -->

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

## Symptom: peers report `Hemis Core:1.3.1` but I installed v0.3.5-testnet

**Applies to:** all versions.

**The obvious reading is wrong.** You will read this as "my peers are running old software" or "my
install did not take".

**What is actually happening.** `1.3.1` is the inherited Hemis Core lineage version carried in the
P2P subversion string. It is not the PTX release number. A node built from `v0.3.5-testnet`
advertises the lineage version to peers while `Hemisd -version` reports `v0.3.5-testnet`.

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

## Symptom: `systemctl` says the unit is **enabled** but the daemon running is not the unit's

**Applies to:** all versions.

**The obvious reading is wrong.** `systemctl enable --now hemis-ptx` printed no error you noticed,
`Hemis-cli getblockcount` answers, and the node is synced. **It looks finished.** It is not: the
daemon answering you is the one *you* started by hand, and the unit is enabled but **dead**.

**What is actually happening.** `enable --now` is two operations, and only the first succeeded.
`enable` creates the boot symlink — that always works. `--now` then tries to *start* the service,
and it cannot, because the daemon you hand-started in the previous step **still holds the datadir
lock**. The unit fails with *"Cannot obtain a lock on data directory"*.

★★ **Nothing tells you.** `systemctl enable --now` does not fail loudly when only the `--now` half
fails, your node keeps answering, and every check passes — because a node *is* running. It is just
not the one systemd will bring back.

**The consequence arrives at the next reboot.** The hand-started daemon dies with the machine, the
unit tries to start it, and — depending on how many times it already retried — may have tripped its
restart limit and stopped trying. Your gamemaster is down, unreachable, and accruing PoSe penalties.
★ This was measured on four coordinator hosts on 2026-09-02, all running hand-started daemons beside
an enabled-but-stopped unit, including the one holding the entire float.

**How to tell:**

```bash
systemctl is-enabled hemis-ptx     # enabled
systemctl is-active  hemis-ptx     # ★ failed or inactive -- THIS is the tell
pgrep -a Hemisd                    # yet a Hemisd is running: it is yours, not the unit's
```

**Do you need to act?** Yes, and it takes three commands:

```bash
Hemis-cli stop
sudo systemctl reset-failed hemis-ptx   # ★ the retry limit will have tripped
sudo systemctl enable --now hemis-ptx
systemctl is-active hemis-ptx           # must now say: active
```

★ **`reset-failed` is the step people miss.** After five failed starts in five minutes the unit
enters a failed state and *refuses to retry* — that limit is deliberate, so a broken unit stops
hammering rather than looping invisibly. Until you clear it, `enable --now` will appear to do
nothing at all.

★★ **The documents used to cause this**, by telling you to hand-start the daemon for
`generateblskeypair` and then to enable the unit, with nothing in between. They now say
`Hemis-cli stop` first. If you followed an earlier copy, this entry is why.

---

## Symptom: my gamemaster will not arm — "Local address ... does not match the address from ProTx"

**Applies to:** all versions.

**The obvious reading is wrong.** The message looks like a network fault, so you will check
connectivity. **It is not about reachability.** The node compares the address *and port* it
advertises against the one recorded on chain at registration, and refuses to arm when they differ.
It is a **configuration disagreement**, and it is one of the few PTX errors that names both sides —
read them, because the difference is usually just the port.

**What is actually happening.** There are two causes and they look identical:

1. ★ **The port in your config disagrees with the registered one.** Most often `port=` and
   `rpcport=` have been transposed: the node then advertises its **RPC** port as its P2P address.
   A fresh install never does this — a first gamemaster gets `port=29994` / `rpcport=29995`, and a
   second or later one on the same host gets `29996`/`29997`, `29998`/`29999`. **If yours are
   crossed, they were edited by hand.**
2. ★★ **You registered without a port at all.** `protx_register` accepts a bracketed address with
   no port and **does not complain** — the chain stores the network default **`:29993`**, which no
   host listens on. The registration succeeds, the node syncs, `getgamemasterstatus` reports
   `Ready`, and nothing goes wrong until arming.

**Which one is it?** Compare them directly:

```bash
grep -E '^(port|rpcport)=' ~/.Hemis/Hemis.conf     # on the gamemaster host
Hemis-cli protx_info <proTxHash> | grep -i service  # the registered address:port
```

If the registered port is `29993` you never set one. If it is your **RPC** port, the two config
lines are transposed.

**Do you need to act?** Yes, and ★★ **the fix is `protx_update_service`, not re-registration.**
Your registration and collateral are fine; only the service address is wrong, and that is exactly
what this command changes.

```
protx_update_service "proTxHash" "ipAndPort" ("operatorPayoutAddress" "operatorKey")
```

★★ **Run it from your WALLET host, and the BLS SECRET has to travel there.** This is the part
nobody expects: the wallet host pays for the transaction, but it does not hold your gamemaster's
operator key, so you must pass the **secret** as the fourth argument. The guide mentions this only
under PoSe recovery; it applies here too, and it is the one time the secret leaves the gamemaster.
Move it deliberately, use it, and do not leave a copy behind.

★ **Fix the config as well as the chain.** Updating the registration alone leaves a host whose
`port=` is still wrong; correct both, then restart the daemon and arm.

---

## Symptom: I installed the wrong role — can I re-run `install.sh` to convert this host?

**Applies to:** all versions.

**The obvious reading is wrong.** `install.sh` looks idempotent, so re-running it with a different
`PTX_ROLE` looks like the way to change a host's role. **It is not.** Re-running does **not** convert
anything, and depending on the host it either refuses outright or quietly leaves you worse off.

**What is actually happening.** Two separate behaviours, and you need both:

★ **The config is never overwritten once it exists.** If `~/.Hemis/Hemis.conf` is present,
`install.sh` prints *"already exists — leaving it alone"*, writes a **reference** config beside it as
`Hemis.conf.template` for you to `diff`, and carries on with everything else. That message is a
**warning, not an error** — the script does not stop for it.

★★ **So a role change cannot work by re-running.** Everything else would be set up for the new role
while the config stayed the old one. `install.sh` has a **`ROLE COLLISION`** guard for exactly this:
it compares the `# ROLE:` stamp inside your existing config against the role you asked for, and if
they differ it **refuses with exit 3 before doing any work** — no packages, no clone, no binaries,
no unit.

★★ **The dangerous case is a host with no role stamp.** The stamp was added later, so a config
written by an older `install.sh` does not carry one — and the guard only fires when it finds a stamp
to compare. On such a host the collision is **not** detected: the run completes, the banner reports
**the role you asked for**, and the config on disk is still the role you had. A machine that is
neither role, reported as the role it is not.

**Do you need to act?** Only if you actually need to change a host's role. The installer's own
refusal tells you the sequence, and it is the same by hand:

```bash
sudo systemctl disable --now hemis-ptx
mv ~/.Hemis/Hemis.conf ~/.Hemis/Hemis.conf.was-gamemaster
PTX_ROLE=wallet ./install.sh
```

★ **Better: do not get here.** A wallet host and a gamemaster host are different machines on
purpose. If you meant to build the other one, build it on the other box.

---

## Symptom: do I run `vps-install.sh` on my wallet machine too?

**Applies to:** all versions.

**The obvious reading is wrong.** The bootstrap is the first command in the quickstart, so it looks
like the way every machine starts. **A wallet machine never runs it.**

**What is actually happening.** `vps-install.sh` is the **gamemaster** bootstrap — its own first line
says so — and it is gamemaster-only **by construction, not by omission**: it invokes the installer as
a bare `bash ./install.sh`, passing **no role at all**, so the installer takes its default. There is
no wallet mode, no flag that gives it one, and no environment variable it forwards.

★★ **And the obvious repair does not work either, so do not start down it.** "Bootstrap first, then
re-run with the wallet role" **fails at both steps**: step one silently builds a *gamemaster* (a role
was never passed), and step two is refused outright, because the bootstrap leaves a
`# ROLE: gamemaster` stamp in the config and the installer's `ROLE COLLISION` guard exits 3 on a
stamp that disagrees with the role you asked for. There is no point in the sequence at which you
have a working wallet host.

★★ **A wallet machine takes a different path from the very beginning, and it is shorter:** clone the
repository and run the installer once, with the role given explicitly.

```bash
git clone -b <tag> https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
PTX_ROLE=wallet ./install.sh
```

★ **That is the first and only install on that host.** Do not bootstrap first and then "re-run for
the wallet role" — see the role-collision entry above for why that does not work.

**Do you need to act?** If you already bootstrapped a machine you meant to be your wallet host, read
the role-collision entry: the config is a gamemaster config and re-running will not convert it.

---

## Symptom: my PoSe penalty is `0` — does that mean my gamemaster is working?

**Applies to:** all versions.

**The obvious reading is wrong.** `PoSePenalty: 0` looks like the network confirming you are fine.
It is not a clean bill of health; on a small network it is **the absence of any verdict at all**.

**What is actually happening.** Two different quorum mechanisms run on this chain, with two
different sizes, and PoSe belongs to the one that is **not** PTX signing.

| | quorum size | what it does | is PoSe involved? |
|---|---|---|---|
| `llmq` (inherited) | **3** | the DKG whose commitments carry `validMembers` | ★ **yes — this is the only thing that moves PoSe** |
| PTXDKG (this project) | **11** | forms the quorums that sign PTX rolls | no |

`nPoSePenalty` is written in exactly one place, and only when an `llmq` commitment marks you an
invalid member. So:

★ **Below three registered gamemasters, no `llmq` commitment can form, so the counter cannot move
and `0` means nothing whatsoever.** It is not evidence, in either direction.

★★ **And even once it does move, it is a verdict on `llmq` DKG participation — not on whether your
PTX gamemaster is signing anything.** "PoSe is the network's opinion of you" is true and is
routinely over-read: a penalty of `0` alongside a PTX quorum you never got selected for is two
unrelated facts, not one reassuring one.

**Do you need to act?** No — but do not use `0` as proof. To judge whether your node is healthy, run
`./self-check.sh` and believe its exit code; a **non-zero** PoSe penalty is worth reporting because
it is the network's verdict rather than your machine's, but a zero one is not worth reading at all.

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
