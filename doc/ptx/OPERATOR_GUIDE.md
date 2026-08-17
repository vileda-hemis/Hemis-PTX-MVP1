# ptxbea Gamemaster Operator Guide

**The workflow is two machines.** Your wallet machine (Windows desktop, or Linux if you
prefer) is the control point: it holds the collateral and the owner key, and it is where
registration is authorised. The node (Linux VPS) runs the gamemaster: it holds only the
operator key — and, once you join a quorum, the DKG share. Exactly three things ever
travel between them, and no spending key is one of them.

```
   WALLET MACHINE (Windows)                    NODE (Linux VPS)
   collateral + owner key stay here            operator key + shares live here
   ─────────────────────────────               ──────────────────────────────
                         node address + ports ◄─── (you set up the node first)
   operator BLS SECRET key ───────────────────►  goes in the node config (§Arm)
   collateral signature (one message) ────────►  completes protx_register_submit
```

For operators who ran a Hemis testnet gamemaster before, the rhythm is familiar
(install → conf → sync → register → verify), but **three things changed**:

> **① You must open TWO ports on the node now, not one.**
> The old testnet needed only P2P (51474). ptxbea needs **P2P 29994 AND RPC 29995**, both
> reachable at the address you register. RPC being closed is the silent killer: the node
> registers fine, shows Ready, appears on the explorer — and **never signs a single
> roll**, because the signing fan-out reaches you over RPC at your registered address
> (`ptxbea-known-limitations.md` §13). KDD-085 (sign-over-P2P) will remove the RPC
> requirement; until it lands, open both.
>
> **② `protx_register` replaces `gamemasterprivkey` — and there is no "press start".**
> The old flow (`gamemaster=1`, `gamemasteraddr=`, `gamemasterprivkey=`, then *start from
> the main wallet*) is retired on ptxbea. Registration is an on-chain transaction;
> arming is a BLS key in the node's config. Once the ProRegTx confirms and the key is
> loaded, the node IS started — there is no button in the wallet.
>
> **③ `ptx_shares.dat` lives on the NODE and is NOT covered by your wallet backup.**
> "I backed up my wallet" does not protect it. It is secret material produced by a DKG
> ceremony, not derivable from anything, and a node-datadir backup taken *before* a
> ceremony forfeits that ceremony's share **permanently** (§Recovery).

---

## Ports — stated once

| Port | Purpose | Who must reach it |
|---|---|---|
| **29994** | P2P | everyone (definitionally public) |
| **29995** | RPC (signing fan-out) | the other gamemasters, at your **registered** address |

Both at the edge firewall (port-forward to the VPS) **and** on the host
(`sudo ufw allow 29994 && sudo ufw allow 29995`). RPC = P2P + 1, deliberately below the
kernel ephemeral range; the installer also *reserves* both against ephemeral allocation
(`/etc/sysctl.d/99-hemis-ptx.conf`) so a reboot can't hand your port to an outbound
connection first.

---

# NODE SIDE (Linux VPS) — do this first

You need the node's address and ports before the wallet side can register it.

**1. Install** — Ubuntu 22.04 LTS reference (any x86_64, glibc ≥ 2.31):

```
sudo apt-get update && sudo apt-get upgrade -y
wget https://raw.githubusercontent.com/vileda-hemis/Hemis-PTX-MVP1/<branch>/src/hemisd/contrib/ptx-operator/install.sh
bash install.sh            # --build to compile from source
```

The installer checks the environment (glibc by version, arch, disk), **verifies the
binary checksum** (refuses an unverified fetch), reserves the ports append-safely, and
writes a config with the standard ports and dual-stack `rpcbind`. Re-running is safe —
it never touches `wallet.dat` or `ptx_shares.dat`.

**2. Configure** — edit `~/.hemis-ptxbea/hemis.conf`:

```
externalip=YOUR.PUBLIC.IP          # the address the wallet side will register
rpcallowip=<the peer/fleet range>  # TIGHTEN the default — not 0.0.0.0/0
```

**3. Sync**: `Hemisd -ptxbea`, then `Hemis-cli -ptxbea getblockchaininfo` until
`initial_block_downloading: false` (the loop you know).

**4. Hand the wallet side**: `YOUR.PUBLIC.IP:29994` — that string is what gets
registered.

*(Return here for §Arm after the wallet side has run.)*

---

# WALLET SIDE (Windows, or any machine you control)

**1. Install the wallet.** The Windows build (`Hemis-qt.exe` / `Hemisd.exe` /
`Hemis-cli.exe`, same zip shape as mainnet) is a **full node**: used the classic way it
performs its own sync of the ptxbea chain. On a fresh testnet that is minutes, not days —
but it grows with chain age. (If a PTX Windows build is not yet published: WSL2 running
the Linux binary works today with zero build work.)

**2. Collateral.** Create a fresh receiving address in this wallet and send exactly the
collateral amount to it (100 HMS on the dev net — confirm the current figure for the
testnet you are joining), as a single UTXO you will not touch. Note its `txid` and
`vout`. **The collateral and this wallet's keys never leave this machine.**

**3. Operator BLS key**: `bls generate` (Debug console in Hemis-qt, or `Hemis-cli`).
Two halves:
- the **secret** half → travels to the NODE (config, §Arm). This is the only secret that
  ever leaves the wallet machine, and it can only *operate* the GM — it cannot spend
  the collateral.
- the **public** half → used in registration below.

**4. Register.** Two ways:

**(a) Direct — wallet machine does it all** (needs this wallet synced and holding the
collateral):
```
protx_register <collateralTxid> <collateralVout> YOUR.PUBLIC.IP:29994 \
    <ownerAddr> <operatorPubKey> <votingAddr> <payoutAddr>
```

**(b) Split — collateral key never signs a transaction, only a message** (the
cold-collateral pattern; also works before the wallet finishes syncing):
- on the **node** (its wallet needs a small balance for the fee):
  `protx_register_prepare <collateralTxid> <collateralVout> YOUR.PUBLIC.IP:29994 …`
  → returns a serialized tx and a `signMessage` string
- on the **wallet machine**: `signmessage <collateralAddress> <signMessage>` — a plain
  message signature; no sync required, nothing spendable is exposed
- on the **node**: `protx_register_submit <tx> <sig>`

**5. That's registration done.** There is no "press start" — do not look for one.

---

# NODE SIDE — arm and verify

**§Arm**: put the operator **secret** key from wallet-side step 3 into the node's
config — `gamemasterblsprivkey=...` — then `Hemis-cli -ptxbea stop && Hemisd -ptxbea`
(the restart ritual still works).

**Verify**:
```
bash contrib/ptx-operator/self-check.sh
```
Healthy = every line PASS: synced, peers, registered (DGM advertises
`YOUR.IP:29994`), **Ready**, RPC bound beyond loopback, share held for each quorum
you're a member of, ports reserved. `getgamemasterstatus` says `Ready`; the explorer
shows your collateral in the GM list.

**Then verify RPC from another machine** (the one check that cannot be fully proven from
the node itself — a local probe can hairpin and false-pass). From your wallet machine or
anywhere else:
```
Hemis-cli -ptxbea -rpcconnect=YOUR.PUBLIC.IP -rpcport=29995 \
          -rpcuser=<u> -rpcpassword=<p> getblockcount
```
A timeout here = the silent-failure state: healthy on-chain, never signing.

---

## When it isn't healthy — the failure modes we have actually seen

| Symptom | Meaning | Fix |
|---|---|---|
| Registered but status ≠ Ready | **Unarmed** — operator key not on the node | `gamemasterblsprivkey=` in the node conf, restart |
| Ready but never signs / rolls against your quorum fail | **Unreachable RPC** — localhost-bound, firewalled, wrong address family | `rpcbind=[::]`, open 29995 at edge + ufw, verify from another host |
| Height stuck / behind peers | **Unsynced or partitioned** | check peers; `stop && Hemisd` (if in doubt, repeat). After a fork + mass bans: `clearbanned` — and note a stale `banlist.dat` **survives a resync wipe**; delete it too |
| `ptx_quorum_health`: `member: true, share_current: false` | **Share lost** (node-side) | §Recovery — no recovery exists; keep the node Ready |

## Recovery — share loss (ODC-071)

The DKG share (`ptx_shares.dat`, **on the node**) is produced by an interactive
ceremony. It is **not derivable from the chain, not recomputable, not held by peers —
and not in any wallet backup**. If lost, that membership cannot sign again, ever.

- **Cannot get back**: the share, by any action. Do not re-register; do not reindex —
  neither helps (a plain restart or `-reindex` does NOT lose shares; deleting or
  restoring the node datadir from an old backup DOES).
- **Can get back**: future shares. Keep the node running and Ready; it stays eligible,
  and the next ceremony it is selected into produces a fresh share. The dead membership
  ages out when its quorum rotates or reforms on schedule.
- **Backups, both machines**: wallet machine backs up `wallet.dat` (collateral + owner
  key). Node backs up its datadir **only with snapshots that post-date the newest
  ceremony** — an older snapshot restored over the datadir forfeits the newer shares
  permanently. Two different files, two different machines, two different disciplines.

## Two facts not to build around

- **Quorum selection is advisory, not consensus-enforced**
  (`ptxbea-known-limitations.md` §12): a caller *can* aim a roll at a chosen active
  quorum. Do not design anything assuming otherwise.
- **The RPC port convention is interim** (§13): KDD-085 (sign-over-P2P) removes the RPC
  exposure requirement. Until then, 29995 open at your registered address is what makes
  you a functioning signer.
