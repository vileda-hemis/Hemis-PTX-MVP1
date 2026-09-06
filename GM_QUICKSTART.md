# PTX testnet — gamemaster quickstart


★★ **Before anything: your host needs a global IPv6 address.** IPv4 as well is fine — it is IPv6
that must be there. Signing is point-to-point: the caller connects directly to the address you
register and no relay bridges it, so an IPv4-only gamemaster is invisible to the network while
still syncing and reporting `Ready`. `install.sh` checks this and aborts before writing anything.
★ Addresses starting `fd` are ULA and do **not** count, whatever `ip` calls their scope.

**One command on a fresh VPS.** This is the short path. The long path, with every step explained
and every failure mode named, is `testnet/operator/OPERATOR_GUIDE.md` — the bootstrap below clones
it onto your machine.

```bash
wget https://raw.githubusercontent.com/vileda-hemis/Hemis-PTX-MVP1/v0.4.1-testnet/vps-install.sh
bash vps-install.sh
```

★★ **THE TAG IN THAT URL IS LOAD-BEARING AND IT USED TO SAY `main`.** `raw.githubusercontent.com`
serves whatever a ref points at right now, so a branch in that path is not a pin. Worse than
unpinned, in this repository specifically: **`main` carries a file called `vps-install.sh` that is
the UPSTREAM HEMIS MAINNET bootstrap** — 18 lines that fetch
`Hemis-Blockchain/Hemis/releases/latest`, unzip it into `/usr/local/bin`, and write
`~/.Hemis/Hemis.conf` containing `daemon=1`. It is not a stale PTX bootstrap and it does not fail;
it succeeds, at installing a different blockchain's mainnet node, and reports
`Hemis successfully configured.` while doing it. Every operator who ran the previous version of
this line got that. Fetch by tag.

★ **`wget` then `bash`, deliberately not `curl … | bash`.** Two commands instead of one buys you the
chance to read what you are about to run as root. It is ~170 lines and most of them are comments.
The first thing to check when you read it is the `TAG=` line: it must name a release tag, and it
must be the same one that appears in the URL above.

---

## What you need before you start

| | |
|---|---|
| **Two machines** | a **node** (public IP, 24/7, install with the default `PTX_ROLE=gamemaster`) and a **wallet** machine (local, install with **`PTX_ROLE=wallet`** — it also needs **P2P 29994 open inbound**, so that it returns peers to a network with no DNS seed; it advertises no address and registers nothing). Your collateral never goes on the node. ★ The roles write different configs and `install.sh` prints which one it built — check that line. |
| **Node OS** | any Linux with **glibc ≥ 2.31** and x86_64 or aarch64. Ubuntu 20.04+, Debian 11+, and most others. The installer checks glibc and CPU, **not** the distro name. ★ **Debian 12 is what we test; Ubuntu 24.04 is what we run, and works.** |
| **Node resources** | 2 GB RAM, 20 GB disk. ★ The chain itself is tiny (~30 MB today); the space is for the OS, the Sapling parameters and headroom. |
| **Collateral** | **100 HMS per gamemaster**, one exact unspent output each, on the **wallet** machine. ★ **100, not 1000** — 1000 is mainnet and the old Hemis testnet; ptxtestnet is `nGMCollateralAmt = 100 * COIN` (`src/chainparams.cpp:757`). The check is exact equality, and neither the RPC nor the consensus rejection tells you the number you should have used. See `OPERATOR_GUIDE.md` B1. |

★★ **One gamemaster per host, each with its own routable address. How many you run is agreed
with the coordinator before you start — this guide does not prescribe a number.**
A quorum needs **11 members**, drawn from the pool of registered, eligible, non-banned GMs not
already in an active quorum. Below 11 no quorum forms and every boundary is a silent skip; at
exactly 11, the next GM lost stops formation. The spare capacity that avoids that is a property of
the **network total**, not of your share — which is why the coordinator sets the count.

★ **One GM per host — keep doing this, but the old reason no longer applies.** The reason given
here used to be that the signing fan-out dialled every member on the *same* RPC port, so two GMs
sharing a host could not both be reached. **KDD-085 deleted the fan-out**; signing arrives over P2P
at each GM's own registered address *and port*, so that specific blocker is gone.
★ **Run one GM per host anyway** — one host and one routable address each, as the guide describes.
Whether co-hosting is now supportable is an open question nobody has tested, and a testnet is not
where to find out. Run the bootstrap once on each host.

---

## Ports — the same two on every host

| | port | who must reach it |
|---|---|---|
| **P2P** | **29994** | **everyone** — it is how you sync, relay, *and receive signing requests* |
| **RPC** | **29995** | **nobody. Loopback only.** Do not open it, do not forward it |

**Every host uses the same pair.** There is no per-GM port table any more: one GM per host means
the ports never collide and never change.

Open **29994** in the host firewall (`ufw`/`firewalld`/`iptables`) **and** in the NAT router or
cloud security group. Opening only one of the two places is the most common setup failure.

★★ **29994 being closed is the silent killer.** Signing requests arrive over **P2P**, at the
address you register on chain. A node with 29994 closed still syncs (it dials out), shows as
registered and enabled, and **never signs anything** — it is selected and then never contacted.
Nothing in the ordinary status output tells you this. `self-check.sh` section 5 is the check for it.

★★ **RPC is closed, full stop — and this REVERSED.** Earlier versions of this page said 29995 had
to be reachable by the other gamemasters, configured with `rpcallowip`, and that a closed RPC port
was the thing that stopped you signing. **All of that is now wrong.** KDD-085 deleted the RPC
signing path: there is no credential, no caller allow-list entry, and nothing dials your RPC. It is
a local admin interface. **If you are following an older copy of this page, opening 29995 does not
help you sign and exposes the credentials in your config.**

---

## What the bootstrap does

It is a wrapper, not a second installer. In order:

1. **Installs prerequisites** — `git`, `curl`, `ca-certificates` (apt-based distros; on anything
   else it tells you what to install and carries on).
2. **Clones the pinned release tag** into `~/Hemis-PTX-MVP1`.
3. **Runs `testnet/operator/install.sh` once per GM**, with that GM's datadir and port pair. That
   script is the real installer: it checks glibc and architecture, downloads the release tarball and
   **verifies its sha256 against the published `SHA256SUMS`**, installs the binaries, installs the
   Sapling parameters, and writes each `Hemis.conf` with a generated RPC password at mode 600.
   ★ **You do not pass `PTX_ROLE` here and do not need to.** `install.sh` defaults to
   `gamemaster`, which is what this document is building; it prints a five-second notice saying
   so, which is informational, not an error. The wallet machine is the one that needs
   `PTX_ROLE=wallet` typed explicitly — see `OPERATOR_ONEPAGER.md`.

**Binaries** land in `/opt/hemis-ptx/bin`; `Hemisd` and `Hemis-cli` are symlinked into
`/usr/local/bin` so they are on your PATH (`Hemis-tx` is not — call it by full path if you need it).
**Your copy of the scripts** stays at `~/Hemis-PTX-MVP1/testnet/operator` — run `self-check.sh` from
there.

### What it deliberately does **not** do

★ **It does not start the daemon.** The upstream mainnet `vps-install.sh` starts Hemisd and stops it
again to generate a config. This one does not, because a PTX node needs its **BLS key** in
`Hemis.conf` before there is any point in it running — and starting a node that cannot do its job
teaches you to ignore it.

★ **It does not touch your wallet or collateral.** That is Part B, on your other machine.

★ **It does not open your firewall.** It cannot see your cloud security group, and a script that
opened ports on a box it does not understand would be worse than one that tells you to.

---

## After it finishes — four steps, in order

Full text for each is in `~/Hemis-PTX-MVP1/testnet/operator/OPERATOR_GUIDE.md`.

### 1. Firewall
Open each GM's two ports, in both places. See the table above.

### 2. Start the daemon and generate the BLS key — **per GM**

The BLS key is an **RPC call**, so the daemon has to be running first — and ★★ **nothing has
started it.** This section used to say `install.sh` had; it has not. `install.sh` writes the
systemd unit and deliberately leaves a gamemaster **stopped**, because `gamemaster=1` with no key
refuses to start and the key does not exist yet. `vps-install.sh` says so in its own banner:
*"NOTHING HAS BEEN STARTED."*

```bash
sudo systemctl start hemis-ptx       # ★ NOT `Hemisd` -- a hand-started daemon is not
Hemis-cli -rpcwait getblockcount     #   owned by systemd and dies at the next reboot
Hemis-cli generateblskeypair
```

* the **`secret`** goes into *this* machine's config, **together with `gamemaster=1`**. `install.sh`
  ships both lines already present and **commented out** — uncomment them and fill in the key:

  ```bash
  # in ~/.Hemis/Hemis.conf, under the [ptxtestnet] header, change:
  #     # gamemaster=1
  #     # gmoperatorprivatekey=<the BLS key you generate in the OPERATOR_GUIDE>
  # to:
  #       gamemaster=1
  #       gmoperatorprivatekey=<BLS SECRET>
  Hemis-cli stop                             # ★ REQUIRED -- see below
  sudo systemctl enable --now hemis-ptx
  ```

★★ **The `Hemis-cli stop` is not optional and it fails half-silently without it.** The daemon you
hand-started still holds the datadir lock, so `enable --now` **partly succeeds**: `enable` works,
`--now` fails with *"Cannot obtain a lock on data directory"*. You are left with an **enabled unit
that is not running** beside a **manual daemon that is** — everything looks fine, and the node does
not come back after a reboot. If you have already hit it: `Hemis-cli stop`, then
`sudo systemctl reset-failed hemis-ptx` (the retry limit will have tripped), then enable again.

  ★★ **`enable`, not just `restart`.** From here the unit runs the node, and `enable` is what
  brings it back after a reboot. Measured on the coordinator's own hosts, 2026-09-02: all four
  were running hand-started daemons with this unit present and disabled — including the one
  holding the entire float. A gamemaster down after a reboot accrues PoSe penalties exactly as
  if it were firewalled.


  ★ **Uncomment rather than append.** Appending works — `[ptxtestnet]` is the only section header
  and it is near the top, so a line added at the end is still inside it — but it leaves the
  commented originals sitting beside your real ones, which is confusing to read later and to
  support. `OPERATOR_GUIDE.md` and `OPERATOR_ONEPAGER.md` both describe uncommenting; this now
  matches them.

* the **`public`** half goes to the wallet operator. **Send the public half only.**

★ **Save your own copy of the secret somewhere off this machine before you move on.** This host has
no wallet, so un-banning it later runs from your wallet machine and needs the secret there. If the
only copy is on a node you cannot reach, the gamemaster cannot be recovered. See
`OPERATOR_GUIDE.md` A4.

★★ **`gamemaster=1` goes in with the key and not before.** `install.sh` ships it commented out
deliberately: with the role enabled and no key the daemon **refuses to start** — `Error: ERROR:
Gamemaster priv key cannot be empty.` — and `generateblskeypair` is an RPC call, so you would be
locked out of the daemon you need to produce the key.

★ **Start-at-boot is already handled — `install.sh` writes a systemd unit and enables it.** You do
not need to add one. Confirm with `systemctl is-enabled hemis-ptx`; it should say `enabled`. A GM that is down after a reboot accrues
PoSe penalties exactly as if it were firewalled.

### 3. Register — on the **wallet** machine
`protx_register` with the collateral txid/vout, your `[<gm-ipv6>]:29994` (**bracketed**), and the BLS **public** key.
`OPERATOR_GUIDE.md` sections B1–B2, and read **"The last two arguments are optional to the RPC and
NOT optional to you"** before you send it.

### 4. Verify

```bash
cd ~/Hemis-PTX-MVP1/testnet/operator && ./self-check.sh
```

Ten sections: build identity, role, local RPC, chain sync, registration, IPv6 bind coverage,
**external reachability at the registered address**, PoSe, `ptx_shares.dat` custody, quorum
membership. ★ The **external reachability** section — section 5 in the script's own numbering —
is the one that catches the closed-**P2P** failure above. (RPC is loopback-only on both roles and is
not meant to be reachable.) Count carefully: the script numbers its sections `0`, `0b`, then `1`-`8`,
so the fifth item in this list is not section 5.

---

## Troubleshooting the bootstrap

**`missing required tool: git`** — the prerequisite step could not run (not an apt distro, or no
network). Install `git` and `curl` yourself and re-run.

**`glibc 2.28 is too old (need >= 2.31)`** — the release binaries will not run on this OS. Either
use a newer OS, or build from source on the box:
`PTX_BUILD_FROM_SOURCE=1 PTX_DATADIR=$HOME/.Hemis ./install.sh` (tens of minutes, ~8 GB
of disk, and it installs build dependencies).

**`COULD NOT RESERVE PORTS 32000-33000`** — you are in an unprivileged container, and this is not a
permission you can grant yourself from inside it. **The node still works, and on this network the reservation buys nothing** — it was for the
signing fan-out, which KDD-085 deleted; nothing binds 32000-33000 any more. You can ignore this
message. If you want it gone, the installer prints the exact two commands to run on the **host**,
not in the guest.

**The daemon exits immediately with no obvious error** — check for the Sapling parameters:
`ls -la ~/.Hemis-params` should show `sapling-spend.params` (~46 MB) and `sapling-output.params`
(~3.5 MB). Without them the daemon logs the cache-configuration lines and then `Shutdown requested.
Exiting.` with exit 1. Re-running `install.sh` installs and verifies them (section 3c).

**Re-running is safe.** The bootstrap and the installer are both idempotent: an existing clone is
updated to the tag, an existing `Hemis.conf` is left alone (a reference template is written beside
it instead), and parameters already present are verified rather than re-copied.

---

## Knobs

All optional; the defaults are what the coordinator expects.

| variable | default | why you would change it |
|---|---|---|
| `PTX_GM_COUNT` | `1` | ★ **leave it.** One gamemaster per host is the documented deployment, stated twice above. This exists for a coordinator-directed exception only |
| `PTX_TAG` | the release tag | testing an untagged fix, on instruction |
| `PTX_CLONE_DIR` | `~/Hemis-PTX-MVP1` | you keep sources elsewhere |
| `PTX_DATADIR` / `PTX_P2P_PORT` / `PTX_RPC_PORT` | the defaults | still accepted, but **not the documented path** — one GM per host means the defaults are correct. ★ The RPC port is free to change now — the fan-out that dialled one shared port for every member was deleted by KDD-085, and signing arrives over P2P at the address you register. Keeping 29995 is convention, not a requirement |

★ **Do not point `PTX_TAG` at `main`.** The operator tooling is not on the default branch; a clone of
`main` has no `testnet/operator/` directory in it. The bootstrap checks for this and stops with that
message rather than failing four steps later.
