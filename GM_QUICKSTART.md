# PTX testnet — gamemaster quickstart

**One command on a fresh VPS.** This is the short path. The long path, with every step explained
and every failure mode named, is `testnet/operator/OPERATOR_GUIDE.md` — the bootstrap below clones
it onto your machine.

```bash
wget https://raw.githubusercontent.com/vileda-hemis/Hemis-PTX-MVP1/main/vps-install.sh
bash vps-install.sh
```

★ **`wget` then `bash`, deliberately not `curl … | bash`.** Two commands instead of one buys you the
chance to read what you are about to run as root. It is ~170 lines and most of them are comments.

---

## What you need before you start

| | |
|---|---|
| **Two machines** | a **node** (public IP, 24/7) and a **wallet** machine (offline/local). Your collateral never goes on the node. |
| **Node OS** | any Linux with **glibc ≥ 2.31** and x86_64 or aarch64. Ubuntu 20.04+, Debian 11+, and most others. The installer checks glibc and CPU, **not** the distro name. |
| **Node resources** | 2 GB RAM, 10 GB disk. |
| **Collateral** | **3× per operator** — see `OPERATOR_GUIDE.md` "Funding the collateral". |

★ **You will run THREE gamemasters, not one.** A quorum needs **11 members** and there are five
operators; five nodes would never form a quorum at all. 5 × 3 = 15 covers 11 with four spare. The
bootstrap installs all three.

---

## Ports — open these in two places

| GM | datadir | P2P | RPC |
|---|---|---|---|
| 1 | `~/.hemis-ptxtestnet-1` | 29994 | 29995 |
| 2 | `~/.hemis-ptxtestnet-2` | 29996 | 29997 |
| 3 | `~/.hemis-ptxtestnet-3` | 29998 | 29999 |

Open **both** ports for **each** GM in the host firewall (`ufw`/`firewalld`/`iptables`) **and** in
the NAT router or cloud security group. Opening only one of the two places is the most common setup
failure.

★★ **RPC being closed is the silent killer.** PTX fan-out dials each member's **RPC** directly to
request a signature. A node with 29994 open and 29995 closed syncs perfectly, shows as registered
and enabled, and **never signs anything** — because it is selected and then never successfully
contacted. Nothing in the ordinary status output tells you this. `self-check.sh` section 5 is the
check for it.

---

## What the bootstrap does

It is a wrapper, not a second installer. In order:

1. **Installs prerequisites** — `git`, `curl`, `ca-certificates` (apt-based distros; on anything
   else it tells you what to install and carries on).
2. **Clones the pinned release tag** into `~/Hemis-PTX-MVP1`.
3. **Runs `testnet/operator/install.sh` once per GM**, with that GM's datadir and port pair. That
   script is the real installer: it checks glibc and architecture, downloads the release tarball and
   **verifies its sha256 against the published `SHA256SUMS`**, installs the binaries, installs the
   Sapling parameters, reserves the fan-out ports, and writes each `Hemis.conf` with a generated RPC
   password at mode 600.

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

The BLS key is an **RPC call**, so the daemon has to be running first.

```bash
Hemisd -datadir=$HOME/.hemis-ptxtestnet-1 -daemon
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 getblockcount      # answers within a few seconds
Hemis-cli -datadir=$HOME/.hemis-ptxtestnet-1 generateblskeypair
```

* the **`secret`** goes into *this* machine's config:
  `echo "gamemasterblsprivkey=<BLS SECRET>" >> $HOME/.hemis-ptxtestnet-1/Hemis.conf`, then restart
  that daemon;
* the **`public`** half goes to the wallet operator. **Send the public half only.**

★ **`-daemon` survives your shell but not a reboot.** Arrange start-at-boot — a systemd unit or
`@reboot` in cron — *before* you report the node as ready. A GM that is down after a reboot accrues
PoSe penalties exactly as if it were firewalled.

### 3. Register — on the **wallet** machine
`protx_register` with the collateral txid/vout, your `ip:29994`, and the BLS **public** key.
`OPERATOR_GUIDE.md` sections B1–B2, and read **"The last two arguments are optional to the RPC and
NOT optional to you"** before you send it.

### 4. Verify

```bash
cd ~/Hemis-PTX-MVP1/testnet/operator && ./self-check.sh
```

Eight sections: local RPC, chain sync, registration, IPv6 bind coverage, **external reachability at
the registered address**, PoSe, `ptx_shares.dat` custody, quorum membership. Section 5 is the one
that catches the closed-RPC failure above.

---

## Troubleshooting the bootstrap

**`missing required tool: git`** — the prerequisite step could not run (not an apt distro, or no
network). Install `git` and `curl` yourself and re-run.

**`glibc 2.28 is too old (need >= 2.31)`** — the release binaries will not run on this OS. Either
use a newer OS, or build from source on the box:
`PTX_BUILD_FROM_SOURCE=1 PTX_DATADIR=$HOME/.hemis-ptxtestnet-1 ./install.sh` (tens of minutes, ~8 GB
of disk, and it installs build dependencies).

**`COULD NOT RESERVE PORTS 32000-33000`** — you are in an unprivileged container, and this is not a
permission you can grant yourself from inside it. **The node still works**; it is a latent fault
where the kernel may hand out a PTX fan-out port as an ephemeral source port. Fix it on the
**host**, not in the guest — the installer prints the exact two commands.

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
| `PTX_GM_COUNT` | `3` | the coordinator told you to run fewer |
| `PTX_TAG` | the release tag | testing an untagged fix, on instruction |
| `PTX_CLONE_DIR` | `~/Hemis-PTX-MVP1` | you keep sources elsewhere |
| `PTX_DATADIR` / `PTX_P2P_PORT` / `PTX_RPC_PORT` | per the table | passed to `install.sh` directly, for a one-off GM |

★ **Do not point `PTX_TAG` at `main`.** The operator tooling is not on the default branch; a clone of
`main` has no `testnet/operator/` directory in it. The bootstrap checks for this and stops with that
message rather than failing four steps later.
