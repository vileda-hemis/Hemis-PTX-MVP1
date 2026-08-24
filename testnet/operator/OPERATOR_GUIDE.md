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

★ **You will run FOUR gamemasters.** A quorum needs **11 members** and there are five operators, so
five nodes would never form a quorum at all — 5 × 4 = 20 covers 11 with nine spare. You repeat the
**node side four times**; the **wallet side is done once**, from one wallet, registering all four.
That is also the realistic pattern: nobody runs a separate wallet per GM.

★ **Why four and not three.** At three each, losing one operator entirely plus any single other
gamemaster puts the pool at exactly 11 — `ptx_formation.cpp:92-93`'s floor, where the next one lost
stops quorum formation silently and every boundary after it is a deterministic skip.

**Collateral: 4× per operator** — one per GM, one GM per host. See "Funding the collateral" below — there are two routes and they
differ in how much back-and-forth they cost you.

---

## Ports — say them once, use them everywhere

| port | purpose | must be reachable from |
|---|---|---|
| **29994** | P2P | the internet |
| **29995** | **RPC** | **the coordinator's caller node — one address, and only that one** |

★ **RPC being closed is the silent killer.** The PTX signing fan-out dials each member's **RPC**
directly to request a signature. A node with 29994 open and 29995 closed syncs perfectly, shows as
registered and enabled, and **never signs anything** — it is selected and then never successfully
contacted. Nothing in the ordinary status output tells you this. `self-check.sh` section 5 is the
check for it.

★★ **It is ONE address, not "your quorum peers", and this matters at twenty gamemasters.**
Gamemasters never dial each other's RPC. The DKG ceremony runs over P2P
(`src/ptx/ptx_dkg_net.cpp:419-427`), and the only node-to-node RPC in the daemon is the signing
fan-out, whose single caller is `ptx_roll` on the coordinator's caller node
(`src/rpc/ptx.cpp:325`). So your `rpcallowip` needs **one entry**, it is the same entry every
operator gets, and **it does not change when another operator joins**. If you were told to add a
line per peer, that was this guide being wrong.

★ **Two lines, not one — and the second is the one people forget.** The address check and the
credential check are separate: `rpcallowip` is enforced before authentication
(`src/httpserver.cpp:236`, HTTP 403) and the credential after it (`src/httprpc.cpp:157`, HTTP 401).
`install.sh` generates a *different* random `rpcpassword` on every host, so the coordinator's
credential does not match yours by default — you need the `rpcauth=` line as well. Both come from
the coordinator, and `install.sh` writes both when `PTX_CALLER` and `PTX_RPCAUTH` are set.

**Open both ports in TWO places** — the host firewall (`ufw`/`firewalld`/`iptables`) *and* the NAT
router or cloud security group. Opening only one is the most common setup failure.

---

## ★★ One GM per host, one routable address per GM

**Four gamemasters means four hosts and four internet-routable addresses.** Not four datadirs on
one machine, and not one address with four port pairs. This changes what you provision, so it is
here rather than at registration.

| per operator | |
|---|---|
| gamemasters | **4** |
| hosts | **4** — one GM each, no sharing |
| routable addresses | **4** — one per GM, reachable from the public internet |
| ports per host | **29994 P2P** and **29995 RPC**, the same pair on every host |
| collateral | **4 × 100 HMS**, one exact unspent output each |

★ **Why one address per GM, and it is not a preference.** The PTX signing fan-out dials each
member at the address it registered on-chain, paired with **one port number that is the same for
every member** — `PTX_FanoutRpcPort()` (`src/ptx/ptx_fanout.cpp:117-120`) takes no per-member
argument. So two GMs sharing a host at different RPC ports cannot both be reached by any single
configuration: whichever port the fan-out uses, the other GM never receives a signing request. It
registers, is selected into quorums, shows `ENABLED`, and silently never signs.

★ **Both ports are the same on every host** — 29994 and 29995 — because the hosts differ, not the
ports. There is no port table to keep straight any more.

★ **Do NOT reuse one BLS key across your four.** Each GM generates its own in step A4. A shared
key means the chain cannot tell your nodes apart.

★ **The port reservation 32000–33000 is host-wide** and `install.sh` sets it once per host. With
one GM per host that is simply once each.

★ **Behind CGNAT this cannot work.** If you cannot get an inbound-reachable address for each GM,
tell the coordinator before you start.

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
git clone -b v0.1.2-testnet https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
./install.sh
```

★ **No datadir or port overrides, and repeat this unchanged on each of your four hosts.** One GM
per host means the defaults are already right: datadir `~/.Hemis`, P2P 29994, RPC 29995.
`PTX_DATADIR` / `PTX_P2P_PORT` / `PTX_RPC_PORT` still exist for unusual deployments but are no
longer the documented path — and **the RPC port in particular must not be changed**, because the
signing fan-out dials one port number for every member (`src/ptx/ptx_fanout.cpp:117-120`), so a GM
on a non-standard RPC port is never contacted.

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
forward 29994 and 29995 to this machine.

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

1. install the wallet-machine binaries the same way as the node (Part A, A1);
2. **start it once and let it create `wallet.dat`, then stop it and start it again** — the second
   start is the one that proves the file opens;
3. **only then** give the coordinator an address.

★ **Your four gamemaster hosts also create a `wallet.dat`**, because the shipped config leaves the
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

You need **one collateral output per GM**, so **four**. ★ You do **not** have to create them by
hand — see the procedure below, which creates each one as part of registering.

★★ **The amount is 100 HMS per GM, and it is EXACT.** Source, not folklore:
`CPTXTestNetParams` sets `consensus.nGMCollateralAmt = 100 * COIN`
(`src/chainparams.cpp:757`; the class starts at `:733` and its `strNetworkID` is `"ptxtestnet"` at
`:738`). So four GMs need **400 HMS**, and five operators need **2000 HMS** across the network.

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
consolidates is no longer a collateral. ★ `protx_register_fund` below builds this output itself, so
there is nothing for you to split and no `listunspent` check to pass first.

**You hold your own collateral.** The coordinator sends you testHMS — that is the entirety of their
involvement. They do not hold your coins, do not receive your BLS public key, and do not register
your gamemasters. **Nothing secret leaves your machines at any point in this guide.**

**How much:** ask for **500 HMS**. 400 is the four collaterals; the rest covers fees, a re-send and
a mistyped address.

### B2. Register — `protx_register_fund`, four times

★★ **This is the procedure. There is nothing to pre-split.** `protx_register_fund` creates the
exact 100 HMS collateral output itself as part of the registration transaction —
`src/rpc/rpcevo.cpp:713-718`:

```cpp
const CAmount collAmt = Params().GetConsensus().nGMCollateralAmt;   // 100 * COIN
tx.vout.emplace_back(collAmt, collateralScript);
FundSpecialTx(pwallet, tx, pl);
```

Your wallet just needs ~100 HMS plus fees spendable, in any denomination, at each call.

```bash
COLL=$(Hemis-cli getnewaddress "gm1-collateral")
PAY=$(Hemis-cli getnewaddress "gm1-payout")

Hemis-cli protx_register_fund \
  "$COLL" "203.0.113.10:29994" "$PAY" "<BLS PUBLIC>" "" "$PAY" 0 "" "$PAY" "yourname-1"
```

| position | argument | value |
|---|---|---|
| 1 | `collateralAddress` | a fresh address of **your own** wallet |
| 2 | `ipAndPort` | the address from Handoff 1, e.g. `203.0.113.10:29994` |
| 3 | `ownerAddress` | yours |
| 4 | `operatorPubKey` | the **BLS PUBLIC** key from Handoff 1 |
| 5 | `votingAddress` | `""` — defaults to `ownerAddress` (`rpcevo.cpp:427-430`) |
| 6 | `payoutAddress` | yours |
| 7 | `operatorReward` | **`0`** |
| 8 | `operatorPayoutAddress` | **`""`** |
| 9 | `ptxPaymentAddress` | yours — see below, this one is not optional |
| 10 | `ptxNodeId` | `yourname-1` … `yourname-4` |

★★ **Arguments 7 and 8 look optional and are not.** They are positional, so you cannot reach
`ptxPaymentAddress` (9) or `ptxNodeId` (10) without passing them. `0` and `""` is the accepted
pair — a non-empty payout address with a zero reward is refused with *"operatorPayoutAddress must
be empty when operatorReward is 0"* (`rpcevo.cpp:437-455`).

★ **Your collateral is protected from your own staker, automatically.** The moment the
registration transaction lands in your wallet it is locked (`CWallet::LockIfMyCollateral`, called
from `AddToWalletIfInvolvingMe`, `src/wallet/wallet.cpp:1102`), and every DGM collateral is
re-locked at each startup (`src/tiertwo/init.cpp:258-266`, gated only by `-gmconflock`, default
on). You do not need to do anything, and you should not need to worry about it — it is the obvious
worry and the answer is good.

★ **The help text says the wrong number.** `Hemis-cli help protx_register_fund` claims the
transaction "will move 10000 HMS" (`src/rpc/rpcevo.cpp:687`). It moves `nGMCollateralAmt`, which is
**100** here. The help string is inherited and wrong; the code is right.

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

Use a distinct `ptxNodeId` per GM (`yourname-1` … `yourname-4`).

Record the returned **protx transaction id** for each GM.

### ★ HANDOFF 2 — Wallet ➜ Node

Copy back to each gamemaster host: the **protx txid** and the echoed **`ptxNodeId`
(label:suffix)**. Nothing secret travels in this direction, and again this is your own two
machines.

---

## Part C — Arm and verify (node machine)

```bash
cd Hemis-PTX-MVP1/testnet/operator
./self-check.sh          # on each of your four hosts
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
→ Almost always RPC 29995 unreachable at the registered address. Re-run `self-check.sh` sections 5
and 6. Check the NAT/security group as well as the host firewall.

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
