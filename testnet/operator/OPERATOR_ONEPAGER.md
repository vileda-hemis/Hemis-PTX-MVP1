# Hemis PTX Testnet — Operator Onboarding

★ **This is the short path.** The long one, with every failure mode named, is
`OPERATOR_GUIDE.md` in this directory. Where the two ever disagree, **the guide is authoritative**
— tell the coordinator, because that is a bug in one of them.

Two kinds of machine. **One wallet host** (holds collateral, registers your GMs, holds all funds)
and **one host per gamemaster**, each with its own internet-routable address.

> **If you have run a Hemis mainnet gamemaster, this is different.** The key is generated on the
> **gamemaster**, not the wallet, and only the *public* half travels. There is no
> `gamemasteraddr`, no "start" from the wallet, and the network flag is `ptxtestnet=1` — **not**
> `testnet=1`, which is a different chain.

---

## Every machine

**1. Provision** — Ubuntu 24.04 or Debian 12, 2 vCPU, 2 GB RAM, 20 GB disk. ★ The chain is tiny
(~30 MB today); the space is for the OS, the Sapling parameters and headroom.

★★ **AND A GLOBAL IPv6 ADDRESS. Check this before you buy the machine.** IPv4 as well is fine —
it is IPv6 that must be there, on **both** roles. `install.sh` refuses a host without one, and
refuses *before writing anything*, so a wrong machine stays clean. A gamemaster needs it because
signing is point-to-point: the caller connects directly to the address you register and **no relay
bridges that connection**, so an IPv4-only gamemaster is invisible to the network while still
syncing and reporting `Ready`. A wallet host needs it because the peer list is IPv6 and this
network has no peer discovery at all — an IPv4-only wallet host has nothing to dial.
★ Addresses starting `fd` are ULA. Linux calls their scope "global"; they are **not** routable and
do not count.

**2. Firewall** — open **TCP 29994 inbound** (router/NAT/cloud security group *and* the host
firewall). **29995 stays closed** — RPC is loopback-only on both roles.

**3. Clone** — same on every machine.

```bash
sudo apt-get update && sudo apt-get install -y git curl ca-certificates
git clone -b v0.3.5-testnet https://github.com/vileda-hemis/Hemis-PTX-MVP1.git
cd Hemis-PTX-MVP1/testnet/operator
```

Then run the installer for **this machine's role** — see below. `PTX_ROLE` is required and the
script aborts if it is unset. It also refuses to run against an existing config, so pick the role
before you install.

**4. Peers** — the installer writes none. Get the current list from
[ptx-explorer.lnky.uk/network](https://ptx-explorer.lnky.uk/network) → **Add Nodes**, and paste the
`addnode=` lines into `~/.Hemis/Hemis.conf` **under the `[ptxtestnet]` section header**. Restart.

> Lines outside `[ptxtestnet]` are silently ignored. Without peers your node sits at zero
> connections and nothing tells you why.

---

## Wallet host

**5. Install.** Started and enabled automatically.

```bash
PTX_ROLE=wallet ./install.sh
Hemis-cli getblockcount
Hemis-cli getconnectioncount
```

**6. Restart once** — this proves the wallet file opens cleanly — then create your addresses:

```bash
sudo systemctl restart hemis-ptx && sleep 10
Hemis-cli getnewaddress "collateral"        # one, reused for all your gamemasters
Hemis-cli getnewaddress "owner-1"           # one per gamemaster, must be unused
Hemis-cli getnewaddress "payout-1"          # one per gamemaster
Hemis-cli getnewaddress "funding"           # where the coordinator sends coins
```

Ask in **#testnet** for enough ptxtestnet HMS to run your gamemasters —
**(N × 100) + 500 HMS** for N gamemasters — and post the **funding** address.

★ **The extra 500 is for staking, not slack.** Operators are expected to stake; without you the
coordinator is the only block producer. Do not leave it idle.

Wait for one confirmation, about a minute. Run `Hemis-cli getbalance` — when it shows the
coordinator's amount, you are ready.

> `getbalance` counts only confirmed coins. If it shows `0.00`, the transfer has not been mined yet
> even though `listtransactions` already lists it.

> **One collateral address is fine.** Collateral is identified on-chain by the transaction *output*,
> not the address, so several 100 HMS sends to one address give you several distinct collaterals.
>
> **Use a fresh payout address per gamemaster.** Reuse works, but merges the reward streams — you
> could not tell which gamemaster earned what, and changing it later needs a
> `protx_update_registrar` per GM.
>
> **The owner address must differ from the collateral address.** That one the chain enforces.
>
> The funding address is unrelated to either — registration funds itself from your wallet balance.

**7. Back up, once funded** — RPC, not `cp`. Copy it off the machine.

```bash
Hemis-cli backupwallet ~/wallet-backup.dat
```

---

## Each gamemaster host

**8. Install**, then **start the daemon**, then generate the BLS keypair **here**. The installer
does not generate it — and it does not start a gamemaster either.

```bash
PTX_ROLE=gamemaster ./install.sh
Hemisd -daemon
Hemis-cli -rpcwait getblockcount     # 0 is CORRECT here, and it proves the daemon answers
Hemis-cli generateblskeypair
```

★★ **The order matters and this step used to have it wrong.** `install.sh` writes the systemd unit
but deliberately does **not** start a gamemaster — `gamemaster=1` with no key refuses to start, so
there is nothing to run until step 9. But `generateblskeypair` is an **RPC call**: it needs a daemon
listening. Run `install.sh` and then reach for the key without starting anything and you get
`couldn't connect to server`.

★ **`getblockcount` returning `0` is not a fault.** You have no `addnode` lines yet, so no peers, so
nothing to sync from — the node has only its genesis block. It is a liveness check: it proves the
RPC answers before you depend on it. ★ `-rpcwait` is not padding: `-daemon` forks before the RPC
server is up, so an immediate call can fail on a node that is starting perfectly well.

Keep the **secret** on this machine. Only the **public** half goes to the wallet host.

★★ **And keep your own copy of the secret somewhere off this machine, now.** A gamemaster has no
wallet, so it cannot pay a fee and cannot recover itself. Un-banning it later runs **from your
wallet machine** and needs this secret passed to it. If the only copy lives on a node you cannot log
into, the gamemaster cannot be recovered. A password manager or an offline note is enough.

**9. Edit** `~/.Hemis/Hemis.conf` under `[ptxtestnet]`. ★ **Two of these three lines are already
in the file and only need uncommenting; the third is already correct and you do not touch it.**

```
gmoperatorprivatekey=<secret from step 8>   # present, COMMENTED -- uncomment and fill in
gamemaster=1                                # present, COMMENTED -- uncomment
externalip=<already written by install.sh>  # ★ DO NOT ADD -- see below
```

★★ **`externalip` is not yours to set.** `install.sh` selects this host's global IPv6 address and
writes the line for you; a second copy you add by hand is either a duplicate or a contradiction, and
the duplicate is the one that is hard to spot later. If it is missing, the install did not complete
— re-run it rather than patching around it.

```bash
Hemis-cli stop                             # ★ REQUIRED -- see below
sudo systemctl enable --now hemis-ptx
```

★★ **The `Hemis-cli stop` is not optional and it fails half-silently without it.** The daemon you
hand-started still holds the datadir lock, so `enable --now` **partly succeeds**: `enable` works,
`--now` fails with *"Cannot obtain a lock on data directory"*. You are left with an **enabled unit
that is not running** beside a **manual daemon that is** — everything looks fine, and the node does
not come back after a reboot. If you have already hit it: `Hemis-cli stop`, then
`sudo systemctl reset-failed hemis-ptx` (the retry limit will have tripped), then enable again.

> **`enable` matters.** Without it the daemon runs until the next reboot and then silently does not
> come back.
>
> `ptxnodeid=` comes later — you cannot know it until after registration.

★ **Before registering, confirm this host has peers:**

```bash
Hemis-cli getconnectioncount         # must be > 0
```

> ★ Zero here means your `addnode` lines are missing, wrong, or above the `[ptxtestnet]` header.
> Registering an unreachable gamemaster succeeds and then fails later, quietly — the chain records
> the address you gave it whether or not anything answers there.

**10. Register**, from the **wallet host**, once per gamemaster.

> **Use the walkthrough:** [ptx-explorer.lnky.uk/v2/register](https://ptx-explorer.lnky.uk/v2/register)
> — it composes the command for you, names which machine each step runs on, and validates the label
> before you find out from the chain. The manual form is below if you prefer it.

**First: send exactly 100 HMS** to your collateral address, wait one confirmation, then find the
txid and `vout`:

```bash
Hemis-cli listunspent 1 9999999 '["<collateral-addr>"]'
```

Take the entry showing `100.00000000`. Then, seven required arguments and four optional:

```bash
OWNER=$(Hemis-cli getnewaddress "gm1-owner")

Hemis-cli protx_register \
  "<collateral txid>" \
  <vout> \
  "[<GM-ipv6>]:29994" \
  "$OWNER" \
  "<BLS PUBLIC from step 8>" \
  "" \
  "<payout-addr>" \
  0 \
  "" \
  "<payout-addr>" \
  "gm1"
```

| # | argument | value |
|---|---|---|
| 1 | `collateralHash` | txid of your 100 HMS send |
| 2 | `collateralIndex` | its `vout` — **a number, not quoted** |
| 3 | `ipAndPort` | **the gamemaster's** IPv6 address, **bracketed**, then the port — not this machine's |
| 4 | `ownerAddress` | fresh, unused, one per gamemaster, ≠ collateral address |
| 5 | `operatorPubKey` | the **public** half from step 8 |
| 6 | `votingAddress` | `""` — defaults to owner |
| 7 | `payoutAddress` | where rewards go |
| 8 | `operatorReward` | `0` |
| 9 | `operatorPayoutAddress` | `""` |
| 10 | `ptxPaymentAddress` | **set this** — see below |
| 11 | `ptxNodeId` | your label, e.g. `gm1` |

> **Arguments 8 and 9 look optional and are not.** They are positional: you cannot reach 10 or 11
> without passing them. `0` and `""` is the accepted pair.
>
> **Argument 10 cannot be added later.** A gamemaster registered without `ptxPaymentAddress` earns
> block rewards normally but is **not eligible for PTX lottery payouts**, and re-registering is the
> only fix. Using your payout address here is fine and is what most operators should do.
>
> **Argument 11 is a bare label** — 3–24 chars, `[a-zA-Z0-9_-]`, no leading or trailing `-`/`_`, not
> all-numeric, not a reserved word. **Keep the response**: it returns the compound id
> (`gm1:a1b2c3d4`), which you need in step 11.
>
> ★ **Do not use `protx_register_fund`.** It looks more convenient and **cannot be used with
> `ptxNodeId`** — the chain derives the id's suffix from the collateral outpoint, which that RPC
> does not know until after it has committed the wrong one. Registration fails with
> `bad protx id suffix`. `OPERATOR_GUIDE.md` §B2 has the full reason.

Collateral auto-locks against the staker and stays on the wallet host. **The gamemaster never holds
funds and ships with no wallet at all.**

**11. Add the compound node id**, on the **gamemaster host**. The registration response returned
something like `gm1:a1b2c3d4` — that, not the bare label, goes in `~/.Hemis/Hemis.conf` under
`[ptxtestnet]`:

```
ptxnodeid=gm1:a1b2c3d4
```

```bash
sudo systemctl restart hemis-ptx
Hemis-cli -rpcwait getconnectioncount    # ★ -rpcwait: the RPC server lags the restart
```

> Lost the response? `Hemis-cli protx_list true true` on the wallet host lists the ProTxs involving
> your keys, with the `ptxNodeId` in each — no need to re-register.

**12. Verify:**

```bash
grep -c '^externalip=' ~/.Hemis/Hemis.conf   # must be exactly 1
./self-check.sh                              # exit 0, no [????]
Hemis-cli -rpcwait getgamemasterstatus       # status: Ready
```

★ **The `externalip` count is first because it is the one this document used to get wrong.** Two
lines means the daemon advertises one of them, and if it is not the one you registered the
gamemaster syncs, reports `Ready`, and then refuses to arm. `self-check.sh` catches it from
`v0.3.5-testnet` onward; on an earlier tag it does not, which is why the `grep` is here.

---

## Upgrading later

★ **`install.sh` never overwrites your `Hemis.conf`.** So an upgrade keeps every edit you made —
and equally, **a setting introduced by the new tag never arrives.** You end up running new binaries
on the previous tag's configuration, and nothing reports it.

Stop the service, move `~/Hemis-PTX-MVP1` aside, clone the new tag, re-run `install.sh` with the
same `PTX_ROLE`, restart. Then **diff your config against the `~/.Hemis/Hemis.conf.template` the
installer writes** — that diff is everything the upgrade would have changed and did not.

★ Full sequence and the post-upgrade checklist: `OPERATOR_GUIDE.md` **Part D**.

## Notes

- **Never send anyone your BLS secret.** Only the public half leaves the gamemaster host — and your
  own offline copy, which is yours alone.
- **No quorums below 11 registered GMs.** Registered and armed with nothing happening is expected
  until the network reaches that.
- **PoSe:** an unreachable gamemaster is banned within ~40 minutes. **Recovery runs from your wallet
  host** with `protx_update_service`, passing your BLS secret as argument 4 — a banned gamemaster
  cannot supply its own key, and it has no wallet to pay the fee. The collateral is not spent and is
  not lost. `OPERATOR_GUIDE.md` "If your GM is PoSe-banned" has the exact command.
  **Check reachability before registering.**
- **Explorer:** `https://ptx-explorer.lnky.uk` — blocks, addresses, transactions. `/v2` verifies a
  roll from its raw bytes; `/v2/register` walks you through registration.
- `getinfo` reports `testnet: false`. That is correct — it means "not the Hemis testnet".
