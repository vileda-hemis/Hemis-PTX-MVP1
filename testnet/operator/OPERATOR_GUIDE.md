# PTX testnet — operator guide

**Follow this literally.** Where a step needs knowledge you might not have, it says so rather than
assuming. If a step seems to be telling you something obvious, that is deliberate — the steps that
look obvious are the ones that get skipped and then silently break a node.

---

## What you are setting up

**Two machines, and they do different jobs.** This is not optional bookkeeping — the split is the
whole point:

| | **Wallet machine** | **Node machine** |
|---|---|---|
| holds | your collateral coins, your wallet keys | the running daemon, your BLS **secret** key, `ptx_shares.dat` |
| runs | briefly, to register | 24/7, publicly reachable |
| exposure | local, but **P2P 29994 open inbound** | public IP, **P2P 29994 open inbound** |

★ **The wallet machine needs P2P 29994 open too, and this CHANGED — earlier drafts said it needed
nothing open.** It runs `PTX_ROLE=wallet`, which sets `listen=1` like any other node, so that it
returns peers to a network that has **no DNS seed** and a small peer count. What it still does *not*
do is advertise an address: it sets no `externalip` and registers nothing. ★ **Only 29994. RPC
29995 stays loopback-only on both roles** — do not open it on either machine.

**Your collateral never goes on the node machine.** If the node is compromised, the attacker gets the
node — not your coins.

★ **How many gamemasters you run is agreed with the coordinator before you start. This guide does
not prescribe a number.** What it does fix is the **shape**: **one wallet machine**, used once, from
which you register *all* of your gamemasters; and **one or more GM hosts**, one gamemaster each, the
node side repeated unchanged per host. Nobody runs a separate wallet per GM.

★ **What the network needs — which is why the coordinator asks for a count at all.** A quorum is
**11 members** (`ptx_formation.cpp:92-93`), drawn from the pool of registered, eligible, non-banned
gamemasters not already sitting in an active quorum. Below 11 the pool cannot form one and every
boundary is a deterministic skip — silently, with no error anywhere. At exactly 11 the *next* GM
lost stops formation, so a pool with no spare is one PoSe ban away from stopping. Spare capacity is
the whole point of the count, and it is a property of the **network total**, not of any one
operator's share of it — see ODC-094 for what a given total actually buys.

**Collateral: 100 HMS per gamemaster** — one exact output each, one GM per host. See "Funding the collateral" below — there are two routes and they
differ in how much back-and-forth they cost you.

---

## Ports — say them once, use them everywhere

| port | purpose | must be reachable from |
|---|---|---|
| **29994** | P2P | **the internet** — this is the one that matters |
| **29995** | RPC | **nothing. Loopback only.** Do not open it, do not forward it |

★★ **29994 being closed is the silent killer, and 29995 no longer is.** Signing requests arrive
over **P2P**, at the address you register on chain. A node with 29994 closed syncs perfectly (it
makes outbound connections), shows as registered and enabled, and **never signs anything** — it is
selected and then never contacted. Nothing in the ordinary status output tells you this.
`self-check.sh` section 5 is the check for it.

★ **This used to be the other way round, and it is worth knowing why it changed.** The signing
fan-out dialled each member's **RPC** directly, so 29995 had to be open to the caller, matched to a
shared credential, and bound on the right address family — four ways to end up registered, enabled
and silently unable to sign. KDD-085 deleted that path. **If you are following an older guide that
tells you to open 29995, stop: it is now a local admin interface, and exposing it publishes the
credentials in your config for no benefit at all.**

★★ **NO NODE DIALS ANOTHER NODE'S RPC ANY MORE.** The DKG ceremony has always run over P2P
(`src/ptx/ptx_dkg_net.cpp`), and the one remaining node-to-node RPC — the signing fan-out — was
deleted by KDD-085. Signing requests are now ordinary P2P messages to the address you register on
chain. So your config needs **no `rpcallowip` entry for anyone**, and there is **no credential**.

★ **What this replaced, because a lot of setup advice still assumes it.** The old model needed two
lines, and the second was the one people forgot: `rpcallowip` is enforced *before* authentication
(HTTP 403) and the credential *after* it (HTTP 401), and since `install.sh` generates a different
random `rpcpassword` on every host, the caller's credential never matched yours by default. Two
lines, both from the coordinator, both identical on every gamemaster, and four distinct ways to end
up registered, enabled and silently unable to sign. All of it is gone — not narrowed, **gone**.

**Open 29994 in TWO places** — the host firewall (`ufw`/`firewalld`/`iptables`) *and* the NAT router
or cloud security group. Opening only one is the most common setup failure. ★ Do **not** open 29995.

---

## ★★ One GM per host, one routable address per GM

**Each gamemaster means one host and one internet-routable address.** Not several datadirs on one
machine, and not one address with several port pairs. This changes what you provision, so it is
here rather than at registration.

| per operator, for **N** gamemasters | |
|---|---|
| gamemasters | **N** — agreed with the coordinator; not fixed by this guide |
| hosts | **N** — one GM each, no sharing |
| routable addresses | **N** — one per GM, reachable from the public internet |
| ports per host | **29994 P2P — open.** 29995 RPC exists but is **loopback-only**, never opened |
| collateral | **N × 100 HMS**, one exact unspent output each |
| wallet machines | **1** — whatever N is |

★ **Why one address per GM — and the reason CHANGED, so read this rather than skimming it.**
It used to be mechanical: the signing fan-out dialled every member on **one RPC port shared by all
of them** (`PTX_FanoutRpcPort()` took no per-member argument), so two GMs on one host at different
RPC ports could not both be reached — one would register, be selected, show `ENABLED`, and silently
never sign. **KDD-085 deleted the fan-out.** Signing arrives over P2P at each GM's own registered
address *and* port, so that blocker no longer exists.

★ **Keep one GM per host regardless.** One host and one routable address per gamemaster. Co-hosting is now
*mechanically* possible and has never been tested — no fleet run, no ceremony, no PoSe observation
under it — and a launch testnet is the wrong place to discover what breaks. Treat this as a
supported-configuration boundary, not a technical impossibility, which is what it used to be.

★ **Both ports are the same on every host** — 29994 and 29995 — because the hosts differ, not the
ports. Only 29994 is ever opened.

★ **Do NOT reuse one BLS key across your gamemasters.** Each GM generates its own in step A4. A shared
key means the chain cannot tell your nodes apart.

★ **The port reservation 32000–33000 is host-wide** and `install.sh` sets it once per host. With
one GM per host that is simply once each.

★ **Behind CGNAT this cannot work.** If you cannot get an inbound-reachable address for each GM,
tell the coordinator before you start.

---

## Part A — Node machine (do this FIRST, once per GM)

You do the node first because Part B needs two values that only exist after this part.

### A1. Install

★★ **On the distro question, which every operator asks and this guide used to answer wrongly by
omission: Debian 12 is what we test; Ubuntu 24.04 is what we run, and it works.** `install.sh` gates
on **glibc ≥ 2.31 and x86_64/aarch64**, not on a distro name — it never reads `/etc/os-release` to
decide whether to proceed. Anything meeting the glibc and architecture bar is fine; Ubuntu 20.04+
and Debian 11+ both clear it comfortably. The Debian 12 references further down are describing the
box the figures were measured on (BDB 5.3, build timings), not a requirement.

★ **First, the three tools the installer needs and does not install.** On a minimal image you have
none of them, and `install.sh` stops at its first check with `missing required tool: git` — which is
correct but is a poor first thing to meet. On Debian/Ubuntu:

```bash
apt-get update && apt-get install -y --no-install-recommends git curl ca-certificates
```

```bash
git clone -b v0.3.1-testnet https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
./install.sh
```

★★ **`install.sh` builds a ROLE, and it prints which one.** The default is
**gamemaster**, which is what you want here — `PTX_ROLE=gamemaster ./install.sh` is the same
command. The wallet machine in Part B needs `PTX_ROLE=wallet`, and that one is **not** the default,
so it is the one you have to type. ★ The completion output names the role it built; if it says the
wrong one, **re-run with the other rather than editing the config by hand** — the roles differ in
three lines and hand-patching one of them is how you end up with a node that looks right and is
not.

★ **No datadir or port overrides, and repeat this unchanged on each of your GM hosts.** One GM
per host means the defaults are already right: datadir `~/.Hemis`, P2P 29994, RPC 29995.
`PTX_DATADIR` / `PTX_P2P_PORT` / `PTX_RPC_PORT` still exist for unusual deployments but are no
longer the documented path. ★ **The reason to leave the P2P port alone is now the only reason that
exists.** Signing requests arrive over **P2P at the address and port you registered on chain**, so
a mismatch between the registered port and the listening one makes a GM unreachable for signing
while it still syncs and still shows `ENABLED`. The RPC port no longer participates in signing at
all — the earlier text here said it did, citing a fan-out that **KDD-085 deleted**.

★★ **`-b <the release tag>` is required, and a BRANCH NAME HERE DEFEATS THE ENTIRE POINT.**
This line used to read `-b feature/ptx-dkg`. A branch moves: two operators running that command a
day apart get different source, different binaries and a different `install.sh`, which is exactly
what `install.sh`'s own `PTX_REF` default is pinned to a tag to prevent. Pinning the installer
while the instruction that fetches the installer floats is not pinning anything.

★ **It also has to be a `-b` of something.** The operator tooling is not on the default branch
(`main`); cloning without `-b` gives you a checkout with no `testnet/operator/` directory in it and
nothing in this guide will be found.

★ **If the coordinator names a different tag, use it in BOTH places** — `git clone -b <tag> …`
*and* `PTX_REF=<tag> ./install.sh`. The clone decides which guide and which installer you read;
`PTX_REF` decides which source that installer builds or fetches. They are two separate pins and
disagreeing about them is how you end up running one release's installer against another
release's source.

★ **You will end up with the repository in two places, and that is intended.** The copy you just
cloned is the one you run `install.sh` and `self-check.sh` from. `install.sh` keeps a second copy at
`/opt/hemis-ptx`, which is the one it updates and builds against. **Run the scripts from your own
clone; leave `/opt/hemis-ptx` to the installer.** When a new tag is cut, re-clone (or `git pull`)
*your* copy as well — the installer does not update it for you.

`install.sh` checks your environment by **glibc version and CPU architecture**, not by distro name —
so any reasonably modern Linux works, and you get told the real reason if it does not. It also
**installs the `Hemisd` / `Hemis-cli` binaries**, reserves ports 32000–33000 in the kernel (merging
with, never overwriting, any existing reservation) and writes a config with dual-stack `rpcbind`.

★★ **If it stops with "no PTX binaries, and no release artefact to fetch"** — you are on a plain
branch checkout, which is source only, and **until a release tag is cut this is where everyone
lands.** Build from source; the script does the whole thing:

```bash
PTX_BUILD_FROM_SOURCE=1 ./install.sh
```

It installs the build dependencies, sizes `make -j` **from free RAM rather than core count** (a
`-j$(nproc)` on a small box gets OOM-killed two-thirds of the way through a ten-minute build),
compiles, verifies the binaries actually run, and strips them. **Measured: 5m16s** on a 6-core
allowance from a bare Debian 12 — not the "tens of minutes" this guide used to warn about, and there
is no Boost/BDB-4.8 fight to have: the build uses the system BDB via `--with-incompatible-bdb`, which
the script tells you about because it means wallets from this build are not readable by a stock
release binary.

Once a tag exists, prefer the artefact route the script also prints —
`PTX_BIN_URL=<url> PTX_BIN_SHA256=<sha256> ./install.sh`.

### A2. Start the daemon

Nothing below this line works until the daemon is running — including generating your BLS key,
which is an **RPC call**, not an offline command.

```bash
Hemisd -daemon
Hemis-cli getblockcount     # should answer within a few seconds
```

If `Hemisd` is not found, `install.sh` did not install binaries — go back to A1.
To stop it: `Hemis-cli stop`.

★ **This is a 24/7 node.** `-daemon` survives your shell but not a reboot. Arrange for it to start
at boot (a systemd unit, or `@reboot` in cron) before you report the node as ready — a GM that is
down after a reboot accrues PoSe penalties exactly as if it were firewalled.

### A3. Find your external address — get this right

```bash
curl -4 https://ifconfig.co     # your IPv4
curl -6 https://ifconfig.co     # your IPv6, if you have one
```

★ **Pick ONE and use it consistently.** Whichever you register is the address every peer will dial.
If you register an IPv6 address, your daemon must be listening on IPv6; if IPv4, on IPv4. Mixing
them produces a node that looks completely healthy and is unreachable. This is the single most
common way a node fails on this network. `install.sh` binds both families to make it hard to get
wrong, but the address you *register* must match one your machine actually answers on.

**If `curl -6` prints nothing, you have no IPv6 and there is no decision to make** — register the
IPv4 address. The warning above matters only when you have both and could pick the wrong one.

If you are behind NAT, the address here is your **router's public address**, and the router must
forward **29994 only** to this machine. ★ 29995 (RPC) is loopback-only since KDD-085 —
forwarding it exposes the credentials in your config and buys nothing.

★★ **PUT THAT ADDRESS IN `externalip=` BEFORE YOU ARM, OR THE GM NEVER STARTS SIGNING.**
`install.sh` writes it for you when this host has exactly one global address, and leaves a
commented placeholder when it cannot choose. **Behind NAT it cannot choose, and it will be wrong if
you leave it** — the daemon would advertise the private address.

```bash
grep externalip $HOME/.Hemis/Hemis.conf
# if it is commented out, set it to the SAME address you will register, under [ptxtestnet]:
echo "externalip=203.0.113.10" >> $HOME/.Hemis/Hemis.conf
```

Why it is not optional: `CActiveDeterministicGamemasterManager::Init` refuses to arm without a
discoverable external address in the family you registered
(`src/activegamemaster.cpp:152-157` — *"Can't detect valid external address"*), and refuses again
if the address it finds is not identical to the one in your ProTx (`:161-167`). Either way the
gamemaster registers, syncs, shows as enabled — and `getgamemasterstatus` never says `Ready`.
`self-check.sh` section 3 checks exactly this.

### A4. Generate your BLS key

```bash
Hemis-cli generateblskeypair
```

Output has two halves:

```
{ "secret": "<BLS SECRET>", "public": "<BLS PUBLIC>" }
```

* **`secret`** — goes into `Hemis.conf` on **this** machine, as `gmoperatorprivatekey=<secret>`.
  **It never leaves this machine.** Not in chat, not in email, not in a ticket.
* **`public`** — this is what you hand to the wallet operator.

★★ **The setting is spelled `gmoperatorprivatekey`, and nothing else works.** It is read at
`src/tiertwo/init.cpp:290` and registered in the daemon's own help at `:39`
(`-gmoperatorprivatekey=<bech32>`). ★ **A wrong spelling is not rejected — it is ignored**, and the
consequences are worse than a typo deserves. `gmoperatorkeyStr` comes back empty, `fDeterministic`
is therefore false (`:291`), and the daemon takes the **legacy** gamemaster branch (`:313-323`),
which wants an entirely different setting (`-gamemasterprivkey`, a pre-DIP3 key this network does
not use). What you see is:

```
Error: ERROR: Gamemaster priv key cannot be empty.
```

★ That message names **`gamemasterprivkey`**, not the setting you got wrong, and not the setting
you need. It is the third name in the story, it belongs to a system this chain does not run, and
following it leads nowhere. If you see it, the answer is almost always that the key line above is
misspelled or missing — check the spelling character by character before you change anything else.
`debug.log` settles it in one line: `IS DETERMINISTIC GAMEMASTER` means the name was read,
`IS GAMEMASTER` (no "DETERMINISTIC") means it was not (`:292`).

★★ **`gamemaster=1` goes in NOW, with the key, and not before.** `install.sh` leaves it commented
out on purpose. A config with `gamemaster=1` and no key does not start a limited node — the daemon
**refuses to start at all**, with `Error: ERROR: Gamemaster priv key cannot be empty.` And since
`generateblskeypair` is an RPC call, that would lock you out of the daemon you need in order to
produce the key. Both lines, together, after you have the key.

★ **If you see `Cannot start deterministic gamemaster before enforcement`, your binaries are too
old.** It means `UPGRADE_V6_0` is not active on the network you are running, which on `ptxtestnet`
was true only before `4e1c9e6` ("land the Gate 0 cut — V6_0 on", 2026-08-21). The message is
accurate: enforcement really is off in that build, and `NO_ACTIVATION_HEIGHT` short-circuits to
`UPGRADE_DISABLED` (`src/consensus/upgrades.cpp:99-100`) at every height. Do **not** follow its
advice to remove `-gmoperatorprivatekey` — that starts you as a legacy gamemaster, which is not
what this network runs. Get binaries built from the current tag instead.

★ **`Hemisd -daemon` prints `Hemis server starting` and exits 0 even when startup then fails.**
The parent forks and returns before the child reaches either check above, so a script that tests
`$?` sees success on a daemon that is not running. Measured 2026-08-23 on both refusals. This is
why the check below is `getblockcount` and not the exit status of the start command.

★ **Both lines must land under the `[ptxtestnet]` section**, not above it — `Hemis.conf` has a
section header and settings above it are ignored on this network. `>>` appends to the end of the
file, which is inside the section, so the commands below are correct as written.

```bash
# Add the secret AND enable the gamemaster role, then restart the daemon:
echo "gmoperatorprivatekey=<BLS SECRET>" >> $HOME/.Hemis/Hemis.conf
echo "gamemaster=1"                      >> $HOME/.Hemis/Hemis.conf
Hemis-cli stop
Hemisd -daemon

# Prove it took -- this answers only if the daemon came back up:
Hemis-cli getblockcount
```

### ★ HANDOFF 1 — Node ➜ Wallet

**Both machines are yours.** This is you copying two values from a gamemaster host to your own
wallet machine, not a message to anybody. Nothing here goes to the coordinator or to another
operator.

Copy to your wallet machine, per gamemaster:

1. that host's **BLS PUBLIC key**
2. that host's **external address and P2P port**, e.g. `203.0.113.10:29994`

**Do not send the BLS secret — to anywhere, including your own wallet machine.** It belongs on the
node that uses it and nowhere else. Anyone asking you for it is either mistaken or attacking you.
(The one exception is a PoSe recovery, which is documented at the end of this guide and tells you
exactly why and how.)

---

## Part B — Wallet machine

### B0. Install, then create the wallet, then ask for coins — in that order

★★ **Do not get coins sent to a wallet you have not yet proved you can open.** The order is:

1. install the wallet-machine binaries as in Part A, A1 — but **with the wallet role**:

   ```bash
   PTX_ROLE=wallet ./install.sh
   ```

   ★ **This is not the same config as a gamemaster, and the difference is exactly one line.**
   A wallet host sets **no `externalip`** — that field advertises an address for *registration*, and
   this machine registers nothing. It **does** set `listen=1`, the same as a gamemaster, so **open
   P2P 29994 inbound here too**. That is not an oversight: ptxtestnet has no DNS seed and a small
   peer count, so a node that takes connectivity and returns none is a real cost. ★ **RPC 29995
   stays loopback-only on both roles** — never open that one;
2. **start it once and let it create `wallet.dat`, then stop it and start it again** — the second
   start is the one that proves the file opens;
3. **only then** give the coordinator an address.

★ **Your gamemaster hosts each also create a `wallet.dat`**, because the shipped config leaves the
wallet on. Those wallets hold nothing and you do not need to back them up — but keep **a few HMS**
in one of them if you want the on-node PoSe recovery route to work without moving your BLS secret.
See "If your GM is PoSe-banned".

★ **The reason is Berkeley DB, and it is specific to this build.** `install.sh` compiles with
`--with-incompatible-bdb` (`install.sh` section 3b, and it says so out loud on every source build:
*"this build uses the SYSTEM Berkeley DB, not 4.8: wallets it creates are NOT readable by a stock
release binary"*). A `wallet.dat` created here is written by the system BDB — 5.3 on Debian 12 —
and a stock Hemis release binary, which is built against BDB 4.8, **will not open it**. Every
machine on this testnet runs this same build, so nothing bites while that stays true. It bites the
day someone swaps in a release binary, and by then the wallet has coins in it.

A funded wallet that later will not open is a bad way to learn this. An empty one is free to throw
away.

### B1. Funding the collateral

You need **one collateral output per GM**. ★ You do **not** have to create them by
hand — see the procedure below, which creates each one as part of registering.

★★ **The amount is 100 HMS per GM, and it is EXACT.** Source, not folklore:
`CPTXTestNetParams` sets `consensus.nGMCollateralAmt = 100 * COIN`
(`src/chainparams.cpp:757`; the class starts at `:733` and its `strNetworkID` is `"ptxtestnet"` at
`:738`). So **N gamemasters need N × 100 HMS**, and the network total is the sum over all
operators — the coordinator tracks that, you do not.

★★ **1000 HMS IS THE WRONG NUMBER AND IT WILL BE OFFERED TO YOU.** 1000 is what mainnet
(`src/chainparams.cpp:254`) and the *old* Hemis testnet (`:426`) use, so it is what anyone with
prior Hemis experience will assume, and it is what earlier drafts of this guide's ancestors said.
ptxtestnet is not either of those chains.

★ **"Exact" means exact — the check is `!=`, not "at least".** A collateral output of any other
size is refused twice over: the RPC rejects it up front with
`collateral <txid>-<n> with invalid value <amount>` (`src/rpc/rpcevo.cpp:569`), and the consensus
rule behind it rejects the transaction as `bad-protx-collateral-amount`
(`src/evo/specialtx_validation.cpp:119`). Neither of those messages contains the number you were
supposed to use, which is why it is written above.

★ **One output, not a total.** Each collateral must be a **single unspent output** of exactly
100 HMS. Two payments of 50 do not combine into one, and a 100-HMS payment that your wallet later
consolidates is no longer a collateral. ★ You create this output yourself with a plain `sendtoaddress`, before registering — §B2 below. The
`protx_register_fund` RPC would build it for you, but it cannot be used here — §B2 explains why.

**You hold your own collateral.** The coordinator sends you testHMS — that is the entirety of their
involvement. They do not hold your coins, do not receive your BLS public key, and do not register
your gamemasters. **Nothing secret leaves your machines at any point in this guide.**

**How much:** for **N** gamemasters, ask for **(N × 100) + 100 HMS** — the collaterals, plus a
round 100 of margin that covers fees, a re-send and a mistyped address. Two GMs is 300, four is 500.
★ Ask once, for the whole amount: each top-up is a round trip through a human.

### B2. Register — `protx_register`, once per gamemaster

★★ **Use `protx_register`, and fund the collateral yourself first.** Its sibling
`protx_register_fund` looks more convenient — it creates the collateral output as part of the
registration — but **it cannot be used together with `ptxNodeId`, and `ptxNodeId` is not optional
here.** The reason is structural, not a matter of taste:

* The node id's `:suffix` is `SHA256(serialize(collateral outpoint))[0:4]`
  (`src/rpc/rpcevo.cpp:478-487`).
* In the `_fund` path the collateral is created *by the registration transaction itself*, so that
  outpoint's hash is `tx.GetHash()` — read at `src/rpc/rpcevo.cpp:745`, **before**
  `SignAndSendSpecialTx` writes the completed `node_id` into the transaction's `extraPayload`.
* Writing it there changes `tx.GetHash()`. The suffix committed on chain is therefore derived from
  a transaction id the final transaction does not have.
* Consensus recomputes the suffix from the *resolved* outpoint and rejects the mismatch:
  `bad-protx-node-id-suffix`, `DoS(100)` (`src/evo/specialtx_validation.cpp:170-179`).

★ **This is why registration failed for everyone who tried it.** The error surfaces as
`bad protx id suffix` and says nothing about which of the two RPCs you used. `_fund` is fine if you
omit `ptxNodeId` entirely — but then your gamemaster has no PTX identity, and unlabelled
gamemasters **share a single entry** in the pose tracker, which is keyed by node id.

**So: send the collateral, wait one confirmation, then register.**

#### Step 1 — send the collateral, first

★★ **Do this before anything else.** `protx_register` requires a collateral output that is already
**confirmed**; if you leave it to last you will sit at the final command with nothing to do but
wait. Start it now and it matures while you generate the keys.

```bash
COLL=$(Hemis-cli getnewaddress "gm1-collateral")
echo "$COLL"
Hemis-cli sendtoaddress "$COLL" 100
```

Exactly 100 — see the `!=` note above.

#### Step 2 — generate the keys while it confirms

Nothing here touches the collateral, so do it during the wait.

```bash
OWNER=$(Hemis-cli getnewaddress "gm1-owner")     # WALLET HOST
PAY=$(Hemis-cli getnewaddress "gm1-payout")      # WALLET HOST
Hemis-cli generateblskeypair                      # ★ GAMEMASTER HOST, not this one
```

★ The BLS **secret** stays on the gamemaster host and goes in that host's `Hemis.conf`. Only the
**public** half is used below.

#### Step 3 — find the collateral outpoint

Once the send has at least one confirmation:

```bash
Hemis-cli listunspent 1 9999999 "[\"$COLL\"]"
```

An empty list `[]` means the block has not arrived yet — wait and run it again. Take `txid` and
`vout` from the entry showing exactly `100.00000000`.

#### Step 4 — register

```bash
Hemis-cli protx_register \
  "<collateral txid>" <vout> "203.0.113.10:29994" "$OWNER" "<BLS PUBLIC>" \
  "" "$PAY" 0 "" "$PAY" "yourname-1"
```

| position | argument | value |
|---|---|---|
| 1 | `collateralHash` | the `txid` from step 3 |
| 2 | `collateralIndex` | the `vout` from step 3 — a bare number, no quotes |
| 3 | `ipAndPort` | the address from Handoff 1, e.g. `203.0.113.10:29994` |
| 4 | `ownerAddress` | yours — **must differ from the collateral address** |
| 5 | `operatorPubKey` | the **BLS PUBLIC** key from step 2 |
| 6 | `votingAddress` | `""` — defaults to `ownerAddress` (`rpcevo.cpp:427-430`) |
| 7 | `payoutAddress` | yours |
| 8 | `operatorReward` | **`0`** |
| 9 | `operatorPayoutAddress` | **`""`** |
| 10 | `ptxPaymentAddress` | yours — see below |
| 11 | `ptxNodeId` | `yourname-1` … `yourname-N` |

★★ **Eleven arguments, and 8 and 9 look optional but are not.** They are positional, so you cannot
reach `ptxPaymentAddress` (10) or `ptxNodeId` (11) without passing them. `0` and `""` is the
accepted pair — a non-empty payout address with a zero reward is refused with *"operatorPayoutAddress
must be empty when operatorReward is 0"* (`rpcevo.cpp:437-455`).

★ **The owner address must differ from the collateral address**, and must not already be registered.
It does *not* have to differ from your payout address — the help text's own example uses one address
for owner, voting, payout and operator payout.

★ **Your collateral is protected from your own staker, automatically.** The moment the registration
transaction lands in your wallet it is locked (`CWallet::LockIfMyCollateral`, called from
`AddToWalletIfInvolvingMe`, `src/wallet/wallet.cpp:1102`), and every DGM collateral is re-locked at
each startup (`src/tiertwo/init.cpp:258-266`, gated only by `-gmconflock`, default on). You do not
need to do anything — it is the obvious worry and the answer is good.

### ★★ The last two arguments are optional to the RPC and NOT optional to you

They are easy to leave off because they are in the optional group. Do not.

* ★ **`ptxPaymentAddress`** — where PTX lottery rewards are paid. The RPC's own help says: **"GMs
  without this set are ineligible for PTXPAYOUT lottery wins. Must be set at registration time to
  participate in the lottery."** Omit it and your GM runs perfectly, signs correctly, and wins
  nothing. **It cannot be changed later — re-registering is the only fix.** Setting it to the same
  value as your payout address is fine, and is what most operators should do.
* **`ptxNodeId`** — a human-readable label for the PTX pose-tracker, e.g. `gm01`. Supply the **label
  only**; the chain appends the collateral-derived `:suffix` itself. Rules: 3–24 chars,
  `[a-zA-Z0-9_-]`, no leading/trailing `-`/`_`, not all-numeric, not a reserved word. The full
  `label:suffix` is echoed back in the RPC response — **record it**, it is what you put in the
  gamemaster's `ptxnodeid=` and it is how your node is identified in quorum output.

Use a distinct `ptxNodeId` per GM (`yourname-1` … `yourname-N`).

Record the returned **protx transaction id** and **`ptxNodeId`** for each GM.

★ **There is a guided version of this section** that composes the commands for you and checks the
label rules before you run anything: **https://explorer.hemis.tech/v2/register**. It is the same
sequence in the same order; it runs entirely in your browser and sends nothing anywhere.

### ★ HANDOFF 2 — Wallet ➜ Node

Copy back to each gamemaster host: the **protx txid** and the echoed **`ptxNodeId`
(label:suffix)**. Nothing secret travels in this direction, and again this is your own two
machines.

---

## Part C — Arm and verify (node machine)

```bash
cd Hemis-PTX-MVP1/testnet/operator
./self-check.sh          # on each of your GM hosts
```

Work top to bottom and fix every `[FAIL]`. ★ **There are three outcomes, not two.** Besides `[ok]`
and `[FAIL]` there is `[????]` — *this check could not run*. It is **not** a pass, and the script
exits **2** when any appear, precisely so that "nothing failed" cannot be mistaken for "the node is
ready". Exit codes: **0** every check ran and passed · **1** something failed · **2** nothing failed
but something could not be checked.

### ★★ The acceptance criterion is one line: `status: Ready`

```bash
Hemis-cli getgamemasterstatus | grep '"status"'
```

**`"Ready"` and nothing else means armed.** `CActiveDeterministicGamemasterManager::GetStatus()`
(`src/activegamemaster.cpp:60-71`) returns it only after *every* gate in `Init()` passes: the
upgrade active, `listen=1`, your ProTx on-chain, not PoSe-banned, an external address discoverable
in the family you registered, that address equal to the one in your ProTx, and a successful
connection to your own registered service. Nothing else in this guide covers the last three.
`self-check.sh` section 3 asserts it. **Do not report a node as ready on any other evidence.**

The other states you may legitimately see:

| status | meaning |
|---|---|
| `Waiting for ProTx to appear on-chain` | normal for the first minutes after registering |
| `Error. Can't detect valid external address…` | `externalip=` missing or wrong — A3 |
| `Error. Local address … does not match the address from ProTx` | you registered a different address than the node advertises |
| `Gamemaster was PoSe banned` | see the PoSe section below — this one does **not** clear by itself |

The sections that matter most:

* **Section 4 — bind coverage.** Catches the IPv4/IPv6 mismatch described in A3.
* **Section 5 — external reachability at your registered address.** ★ Note carefully what this
  proves: it connects *from your own machine* to *your own address*. On many NAT setups that
  succeeds via hairpin routing **even when nobody outside can reach you**. A `[FAIL]` is real; a
  `[ok]` is encouraging but **not proof**. Ask another operator to connect to you.
* **Section 6 — PoSe penalty.** This is the network's own verdict, not your machine's opinion of
  itself. A **non-zero PoSe penalty means peers are failing to reach you**, whatever section 5 said.
  It is read from *your* on-chain record (`dgmstate.PoSePenalty`), not from the network-wide list —
  the network-wide list is every operator's score, and reading the first entry of it tells you about
  a stranger. ★ **Below three registered gamemasters a zero here proves nothing** — the number is
  structurally incapable of moving that early, and the script now says so instead of passing you.
* **Section 3 also checks `ptxPaymentAddress`.** If it FAILs there, your GM is registered but can
  never win a PTXPAYOUT — and no config change fixes it. See B2.

---

## ★★ If your GM is PoSe-banned

**This is the one failure on this network that does not fix itself, and it arrives fast.**

### What happens, and how quickly

PoSe penalty has exactly one cause: your gamemaster was a member of an LLMQ session that produced a
final commitment marking you invalid (`src/evo/deterministicgms.cpp:828`). In practice that means
**your peers could not reach you when it mattered**. Each such failure costs
`CalcPenalty(66)` — 66 points of a maximum of `max(100, registered GM count)` (`:272-278`) — and
the score decays by **1 per block** (`:605`).

Sessions run every `dkgInterval = 20` blocks (`src/chainparams.cpp:75-84`). So two failures twenty
blocks apart is 132 against a limit of 100:

> **A gamemaster that peers cannot reach goes from clean to banned in roughly forty minutes.**

★ It cannot happen at all below **three** registered gamemasters — a session needs `minSize = 2`
(`src/llmq/quorums_dkgsession.cpp:98`, `src/llmq/quorums_commitment.cpp:71`) — which is why an
early clean score is not evidence of anything.

### It does not decay back

Once `nPoSeBanHeight` is set you are out of the eligible set, and the decay does not apply: the ban
clears **only** when a `ProUpServTx` lands on chain (`src/evo/deterministicgms.cpp:693-700`). Until
then your node is not selected, not paid, and not part of any quorum.

### Fix the cause first

A revival with the fault still present is banned again in another forty minutes. Work section 5 of
`self-check.sh`, check `externalip=`, check the firewall **and** the NAT rule, and confirm
`getgamemasterstatus` can reach `Ready`.

### Then recover — and read this before you copy anything

```bash
# ON THE GAMEMASTER HOST, which already has the BLS secret in its Hemis.conf.
# Needs a few HMS in this node's own wallet to pay the fee.
Hemis-cli protx_update_service \
    "<your protx txid>" "" "" "<YOUR BLS SECRET>"
```

★★ **The BLS secret must be passed explicitly, as argument 4, and there is no way around it.**
Left empty, the RPC falls back to the node's own active gamemaster key — but that path calls
`GetValidGM`, which returns `nullptr` for a banned gamemaster
(`src/evo/deterministicgms.cpp:114-121`), and the manager is in `GAMEMASTER_POSE_BANNED` rather
than `Ready` (`src/activegamemaster.cpp:146-147`). **A banned gamemaster cannot supply its own key
to un-ban itself.** This is the single exception to "the BLS secret never moves" in Handoff 1 — and
running the command *on the gamemaster host* is what keeps it from moving at all, which is why this
guide leaves the wallet enabled there and suggests keeping a few HMS in it.

★ **The second argument is `""` and that is deliberate.** `Hemis-cli help protx_update_service`
says *"If the IP is changed for a gamemaster that got PoSe-banned, the ProUpServTx will also revive
this gamemaster"* (`src/rpc/rpcevo.cpp:921`), which reads as though you must change your address.
**You do not.** The revival code (`src/evo/deterministicgms.cpp:693-700`) fires on any
`ProUpServTx` once all keys are set and never compares the address; passing `""` keeps your
existing one (`src/rpc/rpcevo.cpp:955-957`). The help text is more restrictive than the code.

Confirm:

```bash
./self-check.sh          # section 3 must read: status: Ready
```

---

## ★ `ptx_shares.dat` — read this even if everything works

**This is the one thing on this page that is counter-intuitive enough to be got wrong by default.**

`ptx_shares.dat` lives in the node's datadir. It holds your share of every DKG ceremony you have
taken part in.

* ★ **It is NOT covered by a wallet backup.** Your wallet backup protects your coins. It contains
  nothing about your DKG shares, and no amount of wallet recovery reproduces them.
* ★ **It is rewritten at every ceremony.** The file you backed up last week is not the file you have
  today.
* ★ **Restoring a datadir snapshot from before the newest ceremony PERMANENTLY FORFEITS those
  shares** (ODC-071). The quorum cannot re-issue them to you. There is no recovery path — not from
  the coordinator, not from other members, not from the chain.

**What to do:** back up `ptx_shares.dat` **after every ceremony**, or accept that any datadir restore
loses the shares issued since your last backup. If you rebuild a node from a snapshot, assume the
shares are gone and expect to sit out until the next ceremony.

---

## Troubleshooting

**`debug.log` appears empty, or `grep` finds nothing after a crash**

★ If the daemon was hard-killed (OOM, power loss, `kill -9`), `debug.log` can contain **NUL bytes**.
GNU grep then treats the file as **binary** and prints `Binary file debug.log matches` — or, in a
pipeline, **silently suppresses every match**. You conclude your node logged nothing, when in fact it
logged everything.

```bash
grep -a "PTX" ~/.Hemis/debug.log      # -a = treat as text. ALWAYS use this.
```

Use `grep -a` by default on any `debug.log` from a daemon that did not shut down cleanly.

**Node shows ENABLED but never signs**
→ Almost always **P2P 29994** unreachable at the registered address. Re-run `self-check.sh` sections
5 and 6. Check the NAT/security group as well as the host firewall.
★ Note this answer CHANGED: it used to be "RPC 29995 unreachable". Signing no longer arrives over
RPC, so a closed 29995 cannot cause this and an open one cannot fix it.

**Zero peers**
→ This network has **no peer discovery at all** — no DNS seeds, no fixed seeds
(`src/chainparams.cpp:887`, `:898`). Peers come only from `addnode=` lines, which the coordinator
supplies. Check that they are in `Hemis.conf` **under the `[ptxtestnet]` header** (above it they are
silently ignored — `addnode` is a network-only setting, `src/util/system.cpp:329`), then check that
outbound 29994 is allowed. `getconnectioncount`.

**PoSe score climbing**
→ Peers cannot reach you. This is the authoritative signal; trust it over a local test that passed.
★ You have roughly **forty minutes** before it becomes a ban that does not clear by itself — see
"If your GM is PoSe-banned" above, and fix the reachability before you revive.

**`Error: ERROR: Gamemaster priv key cannot be empty.`**
→ `gamemaster=1` is uncommented and `gmoperatorprivatekey=` is missing **or misspelled**. Both
lines go in together — A4. ★ Do not go looking for `gamemasterprivkey`, which is what this message
literally names: that is the pre-DIP3 legacy setting, the daemon only asks for it because your
operator key was not read, and setting it will not arm this gamemaster. Check the spelling of
`gmoperatorprivatekey` first; `grep DETERMINISTIC debug.log` tells you in one line whether it was
read.

**The daemon refuses to start with "Enabling Gamemaster support requires turning on transaction
indexing"**
→ Something set `txindex=0`. Do not: `DEFAULT_TXINDEX = true` (`src/validation.h:70`) so it is on
unless you turn it off, and a gamemaster will not start without it
(`src/tiertwo/init.cpp:273-276`). Mainnet habits sometimes include disabling it to save disk; on
this chain it costs nothing and is required. If you already did, the daemon tells you to add
`txindex=1` and restart with `-reindex`, and it means it.

**`Hemis-cli: command not found`**
→ The binaries are not installed or not on your PATH. Re-run `install.sh`: it either fetches the
release artefact or tells you exactly what it needs. `install.sh` symlinks `Hemisd` and `Hemis-cli`
into `/usr/local/bin`; if that failed it says so, and the binaries are under `/opt/hemis-ptx/bin`.

**`self-check.sh` says "could not run" / exits 2**
→ A load-bearing check had no evidence to work with — most often because the node is not registered
yet (sections 5 and 6 need the on-chain record). Finish Part B, then re-run. Do not report the node
as ready while any `[????]` remains.

**Re-running `install.sh` does not seem to pick up a new version**
→ It does now (it fast-forwards `/opt/hemis-ptx` and refuses to continue on the wrong commit), but
it does **not** touch the clone you are standing in. Update that one yourself, or re-clone at the
new tag.

**`could not determine glibc version`**
→ You are probably on Alpine or another musl distro. These binaries need glibc; build from source or
use a glibc-based distro.

---

## Things that are NOT what they look like

* ★ **Quorum selection is advisory, not consensus-enforced** (§12). The selection you see via
  `ptx_quorum_list` is *not* binding on other nodes and is not validated by consensus. **Do not build
  tooling or monitoring that assumes it is** — in particular, do not treat "I am in the selected set"
  as a guarantee that other nodes agree.
* **A registered, ENABLED node is not necessarily a working node.** Registration proves you paid
  collateral and published an address. It proves nothing about whether that address answers.
* ★★ **Registered and armed, and nothing appears to be happening, is very often CORRECT.** A
  gamemaster that is waiting for the next key-generation round to include it looks — in every
  command you have — **identical to a broken one**: `getgamemasterstatus` says the same thing,
  `self-check.sh` passes, the logs are quiet, and no share file appears. Nothing in the node's
  output distinguishes "not selected yet" from "selected and silently failing", because the node
  does not know which it is either.

  **What to do:** run `./self-check.sh` and believe it. If it exits **0**, you are done, and the
  correct next action is to wait. Report it as a problem only if a check FAILs, a check returns
  `[????]`, or your **PoSe penalty goes non-zero** — that last one is the network's verdict rather
  than your machine's, and it is the only one of the three that can tell you peers are failing to
  reach you.

  ★ This is written here because the alternative is five operators reporting the same non-bug in
  the same week, and a coordinator who then has to distinguish those five reports from the one
  that is real.
* **Registering via the RPC console is the supported path for launch.** A GM tab in the wallet UI is
  a deliberate fast-follow, built once we know what people actually struggled with here.
* ★★ **`commitment input N not in the confirmed UTXO set` (error -32050) is YOUR wallet, not the
  network, and it clears itself.** It means the roll could not be funded because your **confirmed**
  coins are momentarily used up — every roll spends one and returns its change UNCONFIRMED, so
  until a block confirms that change the coin is not spendable again. It is not a quorum failure,
  not a peer failure, and nothing was charged: the roll stops **before** the commitment is
  broadcast, so no service fee is paid.

  **The sustainable rate is exactly "confirmed non-dust coins you hold" — one coin per roll,
  measured 1:1.** Roll faster than your coins replenish and you will see this; it clears in a block
  or two. If you need a higher sustained rate, hold more coins, not bigger ones — a single large
  UTXO funds exactly one roll at a time, the same as a small one.

  ★ Check with `listunspent 1` and count outputs above dust, **not** `getbalance`. A wallet showing
  26,000 HMS can legitimately have zero spendable coins, because the balance is sitting in
  unconfirmed change.
* ★ **`install.sh` failing at the clone with "Temporary failure in name resolution" is a DNS
  hiccup, not a broken script.** Observed once on VLAN 2, where the immediate retry succeeded. Run
  it again before reporting it. If it recurs, check the resolver on that host — the installer needs
  nothing from DNS except reaching github.com.
