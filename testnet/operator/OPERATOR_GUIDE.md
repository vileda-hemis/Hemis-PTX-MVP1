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

★ **You will repeat the NODE side once per gamemaster you run.** A quorum needs 11 members. Ask the
coordinator how many nodes you are expected to run before you start, because it changes how much
collateral you need.

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

## Part A — Node machine (do this FIRST)

You do the node first because Part B needs two values that only exist after this part.

### A1. Install

```bash
git clone https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
PTX_REF=<the tag the coordinator gave you> ./install.sh
```

`install.sh` checks your environment by **glibc version and CPU architecture**, not by distro name —
so any reasonably modern Linux works, and you get told the real reason if it does not. It also
reserves ports 32000–33000 in the kernel (merging with, never overwriting, any existing reservation)
and writes a config with dual-stack `rpcbind`.

### A2. Find your external address — get this right

```bash
curl -4 https://ifconfig.co     # your IPv4
curl -6 https://ifconfig.co     # your IPv6, if you have one
```

★ **Pick ONE and use it consistently.** Whichever you register is the address every peer will dial.
If you register an IPv6 address, your daemon must be listening on IPv6; if IPv4, on IPv4. Mixing
them produces a node that looks completely healthy and is unreachable. This is the single most
common way a node fails on this network. `install.sh` binds both families to make it hard to get
wrong, but the address you *register* must match one your machine actually answers on.

If you are behind NAT, the address here is your **router's public address**, and the router must
forward 29994 and 29995 to this machine.

### A3. Generate your BLS key

```bash
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet generateblskeypair
```

Output has two halves:

```
{ "secret": "<BLS SECRET>", "public": "<BLS PUBLIC>" }
```

* **`secret`** — goes into `hemis.conf` on **this** machine, as `gamemasterblsprivkey=<secret>`.
  **It never leaves this machine.** Not in chat, not in email, not in a ticket.
* **`public`** — this is what you hand to the wallet operator.

```bash
# Add the secret to your config, then restart the daemon:
echo "gamemasterblsprivkey=<BLS SECRET>" >> $HOME/.hemis-ptxtestnet/hemis.conf
```

### ★ HANDOFF 1 — Node ➜ Wallet

Send the wallet operator exactly two things:

1. your **BLS PUBLIC key**
2. your **external address and P2P port**, e.g. `203.0.113.10:29994`

**Do not send the BLS secret.** Anyone asking you for it is either mistaken or attacking you.

---

## Part B — Wallet machine

### B1. Fund the collateral

Send the collateral amount, **as one single unspent output**, to an address in this wallet. Ask the
coordinator for the exact amount — do not guess, and do not split it across two transactions, because
the registration needs one output of exactly the right size.

Wait for confirmations, then find the output:

```bash
Hemis-cli listunspent
```

Note the `txid` and `vout` of the collateral output.

### B2. Register

```bash
Hemis-cli protx_register_fund <collateralAddress> <ipAndPort> <ownerAddress> <operatorPubKey> <votingAddress> <operatorReward> <payoutAddress>
```

* `<ipAndPort>` — the address from **Handoff 1**, e.g. `203.0.113.10:29994`
* `<operatorPubKey>` — the **BLS PUBLIC key** from Handoff 1

If you prefer to register against collateral you already hold, use `protx_register` (with the
`txid`/`vout` from B1) instead of `protx_register_fund`.

Record the returned **protx transaction id**.

### ★ HANDOFF 2 — Wallet ➜ Node

Send the node operator the **protx txid**. Nothing secret travels in this direction. The node
operator needs it to confirm the registration landed.

---

## Part C — Arm and verify (node machine)

```bash
cd Hemis-PTX-MVP1/testnet/operator
./self-check.sh
```

Work top to bottom and fix every `[FAIL]`. The sections that matter most:

* **Section 4 — bind coverage.** Catches the IPv4/IPv6 mismatch described in A2.
* **Section 5 — external reachability at your registered address.** ★ Note carefully what this
  proves: it connects *from your own machine* to *your own address*. On many NAT setups that
  succeeds via hairpin routing **even when nobody outside can reach you**. A `[FAIL]` is real; a
  `[ok]` is encouraging but **not proof**. Ask another operator to connect to you.
* **Section 6 — PoSe score.** This is the network's own verdict, not your machine's opinion of
  itself. A **non-zero PoSe score means peers are failing to reach you**, whatever section 5 said.

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
* **Registering via the RPC console is the supported path for launch.** A GM tab in the wallet UI is
  a deliberate fast-follow, built once we know what people actually struggled with here.
