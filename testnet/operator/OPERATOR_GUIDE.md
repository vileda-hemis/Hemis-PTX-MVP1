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
| exposure | keep it OFFLINE / local | public IP, two open ports |

**Your collateral never goes on the node machine.** If the node is compromised, the attacker gets the
node — not your coins.

★ **You will run THREE gamemasters.** A quorum needs **11 members** and there are five operators, so
five nodes would never form a quorum at all — 5 × 3 = 15 covers 11 with four spare. You repeat the
**node side three times**; the **wallet side is done once**, from one wallet, registering all three.
That is also the realistic pattern: nobody runs a separate wallet per GM.

**Collateral: 3× per operator.** See "Funding the collateral" below — there are two routes and they
differ in how much back-and-forth they cost you.

---

## Ports — say them once, use them everywhere

| port | purpose | must be reachable from |
|---|---|---|
| **29994** | P2P | the internet |
| **29995** | **RPC** | your quorum peers |

★ **RPC being closed is the silent killer.** PTX fan-out dials each member's **RPC** directly to
request a signature. A node with 29994 open and 29995 closed syncs perfectly, shows as registered and
enabled, and **never signs anything** — because it is selected and then never successfully contacted.
Nothing in the ordinary status output tells you this. `self-check.sh` section 5 is the check for it.

**Open both ports in TWO places** — the host firewall (`ufw`/`firewalld`/`iptables`) *and* the NAT
router or cloud security group. Opening only one is the most common setup failure.

---

## Running three GMs on one host

You will almost certainly put more than one GM on the same machine. That is fine and expected, but
**each one needs its own datadir and its own port pair** — two daemons cannot share either.

| GM | datadir | P2P | RPC |
|---|---|---|---|
| 1 | `~/.hemis-ptxtestnet-1` | 29994 | 29995 |
| 2 | `~/.hemis-ptxtestnet-2` | 29996 | 29997 |
| 3 | `~/.hemis-ptxtestnet-3` | 29998 | 29999 |

```bash
PTX_DATADIR=~/.hemis-ptxtestnet-1 PTX_P2P_PORT=29994 PTX_RPC_PORT=29995 ./install.sh
PTX_DATADIR=~/.hemis-ptxtestnet-2 PTX_P2P_PORT=29996 PTX_RPC_PORT=29997 ./install.sh
PTX_DATADIR=~/.hemis-ptxtestnet-3 PTX_P2P_PORT=29998 PTX_RPC_PORT=29999 ./install.sh
```

Run `self-check.sh` the same way, one GM at a time:

```bash
PTX_DATADIR=~/.hemis-ptxtestnet-2 ./self-check.sh
```

★ **Every port in the table must be open at your registered address** — all six, in both the host
firewall and the NAT/security group. It is easy to open the first pair, verify GM 1, and forget the
other four; GMs 2 and 3 then look healthy and never sign.

★ **Do NOT reuse one BLS key across the three.** Each GM generates its own in step A4. A shared key
means the chain cannot tell your nodes apart.

★ **The port reservation 32000–33000 is host-wide** — `install.sh` sets it once and all three GMs use
it. Running the installer three times does not reserve it three times, and the merge logic means it
will not clobber itself.

---

## Part A — Node machine (do this FIRST, once per GM)

You do the node first because Part B needs two values that only exist after this part.

### A1. Install

★ **First, the three tools the installer needs and does not install.** On a minimal image you have
none of them, and `install.sh` stops at its first check with `missing required tool: git` — which is
correct but is a poor first thing to meet. On Debian/Ubuntu:

```bash
apt-get update && apt-get install -y --no-install-recommends git curl ca-certificates
```

```bash
git clone -b v0.1.0-testnet https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
PTX_DATADIR=~/.hemis-ptxtestnet-1 PTX_P2P_PORT=29994 PTX_RPC_PORT=29995 ./install.sh
```

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

★ **Pass the datadir and ports even for your first GM.** You are running three, and a bare
`./install.sh` would write `~/.hemis-ptxtestnet` — a name that is not in the table above and that
you would then have to translate in every command for the rest of this guide. Name it `-1` now.

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
PTX_BUILD_FROM_SOURCE=1 PTX_DATADIR=~/.hemis-ptxtestnet-1 PTX_P2P_PORT=29994 PTX_RPC_PORT=29995 ./install.sh
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
Hemisd -datadir=$HOME/.hemis-ptxtestnet-1 -daemon
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 getblockcount     # should answer within a few seconds
```

If `Hemisd` is not found, `install.sh` did not install binaries — go back to A1.
To stop it: `Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 stop`.

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
forward 29994 and 29995 to this machine.

### A4. Generate your BLS key

```bash
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 generateblskeypair
```

Output has two halves:

```
{ "secret": "<BLS SECRET>", "public": "<BLS PUBLIC>" }
```

* **`secret`** — goes into `Hemis.conf` on **this** machine, as `gamemasterblsprivkey=<secret>`.
  **It never leaves this machine.** Not in chat, not in email, not in a ticket.
* **`public`** — this is what you hand to the wallet operator.

★★ **`gamemaster=1` goes in NOW, with the key, and not before.** `install.sh` leaves it commented
out on purpose. A config with `gamemaster=1` and no key does not start a limited node — the daemon
**refuses to start at all**, with `Error: ERROR: Gamemaster priv key cannot be empty.` And since
`generateblskeypair` is an RPC call, that would lock you out of the daemon you need in order to
produce the key. Both lines, together, after you have the key.

★ **Both lines must land under the `[ptxtestnet]` section**, not above it — `Hemis.conf` has a
section header and settings above it are ignored on this network. `>>` appends to the end of the
file, which is inside the section, so the commands below are correct as written.

```bash
# Add the secret AND enable the gamemaster role, then restart the daemon:
echo "gamemasterblsprivkey=<BLS SECRET>" >> $HOME/.hemis-ptxtestnet-1/Hemis.conf
echo "gamemaster=1"                      >> $HOME/.hemis-ptxtestnet-1/Hemis.conf
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 stop
Hemisd -datadir=$HOME/.hemis-ptxtestnet-1 -daemon

# Prove it took -- this answers only if the daemon came back up:
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 getblockcount
```

### ★ HANDOFF 1 — Node ➜ Wallet

Send the wallet operator exactly two things:

1. your **BLS PUBLIC key**
2. your **external address and P2P port**, e.g. `203.0.113.10:29994`

**Do not send the BLS secret.** Anyone asking you for it is either mistaken or attacking you.

---

## Part B — Wallet machine

### B0. Install, then create the wallet, then ask for coins — in that order

★★ **Do not get coins sent to a wallet you have not yet proved you can open.** The order is:

1. install the wallet-machine binaries the same way as the node (Part A, A1);
2. **start it once and let it create `wallet.dat`, then stop it and start it again** — the second
   start is the one that proves the file opens;
3. **only then** give the coordinator an address.

★ **The reason is Berkeley DB, and it is specific to this build.** `install.sh` compiles with
`--with-incompatible-bdb` (`install.sh` section 3b, and it says so out loud on every source build:
*"this build uses the SYSTEM Berkeley DB, not 4.8: wallets it creates are NOT readable by a stock
release binary"*). A `wallet.dat` created here is written by the system BDB — 5.3 on Debian 12 —
and a stock Hemis release binary, which is built against BDB 4.8, **will not open it**. Every
machine on this testnet runs this same build, so nothing bites while that stays true. It bites the
day someone swaps in a release binary, and by then the wallet has coins in it.

A funded wallet that later will not open is a bad way to learn this. An empty one is free to throw
away.

### B1. Funding the collateral — two routes

You need **one collateral output per GM**, so **three**.

★★ **The amount is 100 HMS per GM, and it is EXACT.** Source, not folklore:
`CPTXTestNetParams` sets `consensus.nGMCollateralAmt = 100 * COIN`
(`src/chainparams.cpp:757`; the class starts at `:733` and its `strNetworkID` is `"ptxtestnet"` at
`:738`). So three GMs need **300 HMS**, and five operators need **1500 HMS** across the network.

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
consolidates is no longer a collateral. Confirm with `listunspent` that you can see three separate
outputs of exactly `100.00000000` before you register anything.

**Route 1 — the coordinator sends you the coins (simplest, recommended).**
The coordinator pays the collateral to **an address in your own wallet**. From then on it is ordinary
collateral that you own, and everything below works with no extra steps. This is a faucet payment,
nothing more.

**Route 2 — the coordinator keeps the collateral key.**
Also supported, and genuinely: `protx_register_prepare` requires only that the collateral be *an
unspent output* — **not** that your wallet can spend it. (`protx_register` and `protx_register_fund`
*do* require the output be "spendable by this wallet"; `_prepare` deliberately does not.) The flow is:

1. you run `protx_register_prepare` with the coordinator's collateral `txid`/`vout`
2. it returns an **unsigned** ProTx
3. **the coordinator signs it with the collateral key** and returns the signature
4. you run `protx_register_submit` to broadcast

★ **Route 2 costs one extra round trip per GM — three per operator, fifteen across the network.**
Choose it only if the coordinator needs to retain the ability to reclaim collateral. For a testnet,
**Route 1 is almost always the right answer.**

Once funded, find the outputs:

```bash
Hemis-cli listunspent
```

Note the `txid` and `vout` of each collateral output.

### B2. Register

```
protx_register "collateralHash" collateralIndex "ipAndPort" "ownerAddress" "operatorPubKey" \
               "votingAddress" "payoutAddress" ( operatorReward "operatorPayoutAddress" \
               "ptxPaymentAddress" "ptxNodeId" )
```

* `collateralHash` / `collateralIndex` — the `txid`/`vout` from B1
* `ipAndPort` — the address from **Handoff 1**, e.g. `203.0.113.10:29994`
* `operatorPubKey` — the **BLS PUBLIC key** from Handoff 1

Use `protx_register_fund "collateralAddress" …` instead if you want the wallet to create the
collateral output for you in the same transaction (same trailing arguments).

### ★★ The last two arguments are optional to the RPC and NOT optional to you

They are easy to leave off because they are in the optional group. Do not.

* ★ **`ptxPaymentAddress`** — where PTX lottery rewards are paid. **"GMs without this set are
  ineligible for PTXPAYOUT lottery wins. Must be set at registration time to participate in the
  lottery."** Omit it and your GM runs perfectly, signs correctly, and **can never win anything** —
  and it cannot be fixed by editing a config file, because it is registration-time state.
* **`ptxNodeId`** — a human-readable label for the PTX pose-tracker, e.g. `gm01`. Supply the **label
  only**; the chain appends the collateral-derived `:suffix` itself. Rules: 3–24 chars, `[a-zA-Z0-9_-]`,
  no leading/trailing `-`/`_`, not all-numeric, not a reserved word. The full `label:suffix` is echoed
  back in the RPC response — **record it**, it is how your node is identified in quorum output.

Use a distinct `ptxNodeId` per GM (`yourname-1`, `yourname-2`, `yourname-3`).

Record the returned **protx transaction id** for each GM.

### ★ HANDOFF 2 — Wallet ➜ Node

Send the node operator the **protx txid** and the echoed **`ptxNodeId` (label:suffix)**, per GM.
Nothing secret travels in this direction.

---

## Part C — Arm and verify (node machine)

```bash
cd Hemis-PTX-MVP1/testnet/operator
PTX_DATADIR=~/.hemis-ptxtestnet-1 ./self-check.sh     # then -2, then -3
```

Work top to bottom and fix every `[FAIL]`. ★ **There are three outcomes, not two.** Besides `[ok]`
and `[FAIL]` there is `[????]` — *this check could not run*. It is **not** a pass, and the script
exits **2** when any appear, precisely so that "nothing failed" cannot be mistaken for "the node is
ready". Exit codes: **0** every check ran and passed · **1** something failed · **2** nothing failed
but something could not be checked.

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
  a stranger.
* **Section 3 also checks `ptxPaymentAddress`.** If it FAILs there, your GM is registered but can
  never win a PTXPAYOUT — and no config change fixes it. See B2.

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
grep -a "PTX" ~/.hemis-ptxtestnet/debug.log      # -a = treat as text. ALWAYS use this.
```

Use `grep -a` by default on any `debug.log` from a daemon that did not shut down cleanly.

**Node shows ENABLED but never signs**
→ Almost always RPC 29995 unreachable at the registered address. Re-run `self-check.sh` sections 5
and 6. Check the NAT/security group as well as the host firewall.

**Zero peers**
→ Outbound 29994 blocked, or the seed addresses are wrong. Check `getconnectioncount`.

**PoSe score climbing**
→ Peers cannot reach you. This is the authoritative signal; trust it over a local test that passed.

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
