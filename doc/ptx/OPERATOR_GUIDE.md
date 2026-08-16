# ptxbea Gamemaster Operator Guide

For operators who have run a Hemis testnet gamemaster before: the overall rhythm is the
same (install → conf → sync → register → verify), but **three things changed**. Read these
first; everything else follows the flow you already know.

> **① You must forward TWO ports now, not one.**
> The old testnet needed only P2P (51474). ptxbea needs **P2P 29994 AND RPC 29995**, both
> reachable at the address you register. RPC being closed is the silent killer: the node
> registers fine, shows Ready, appears on the explorer — and **never signs a single roll**,
> because the signing fan-out reaches you over RPC at your registered address
> (see `ptxbea-known-limitations.md` §13). KDD-085 (sign-over-P2P) will remove the RPC
> requirement; until it lands, forward both.
>
> **② `protx_register` replaces `gamemasterprivkey` — and there is no "press start".**
> The old flow (`gamemaster=1`, `gamemasteraddr=`, `gamemasterprivkey=`, then *start from
> the main wallet*) is retired on ptxbea. Registration is an on-chain transaction
> (`protx_register`); arming is a BLS key in the config (`gamemasterblsprivkey=`). Once
> the ProRegTx confirms and the key is loaded, the node IS started — there is no button.
>
> **③ `ptx_shares.dat` is secret material you cannot recover.**
> Treat it exactly like `wallet.dat`. A datadir backup taken *before* a DKG ceremony
> forfeits the shares from that ceremony **permanently** (§Recovery below).

---

## Ports — stated once

| Port | Purpose | Who must reach it |
|---|---|---|
| **29994** | P2P | everyone (definitionally public) |
| **29995** | RPC (signing fan-out) | the other gamemasters, at your **registered** address |

Both at the edge firewall (port-forward to the host) **and** on the host
(`sudo ufw allow 29994 && sudo ufw allow 29995`). RPC = P2P + 1, deliberately below the
kernel ephemeral range — the installer also *reserves* both ports against ephemeral
allocation (`/etc/sysctl.d/99-hemis-ptx.conf`) so a reboot can't hand your port to an
outbound connection first.

## Prerequisites

- Ubuntu 22.04 LTS (reference platform) or any x86_64 Linux with glibc ≥ 2.31.
  The installer checks glibc **by version**, not distro name, and fails early if too old.
- 20 GB free disk; python3 (the installer checks).
- **Collateral**: the ptxbea collateral amount (100 HMS on the dev net — confirm the
  current figure for the testnet you are joining) in a wallet you control, in a single
  UTXO you will not spend.
- Your public IP, static or stable.
- If you have Docker, a container path exists that skips the glibc/arch questions
  entirely — ask for the current image tag.

## Install

```
sudo apt-get update && sudo apt-get upgrade -y
wget https://raw.githubusercontent.com/vileda-hemis/Hemis-PTX-MVP1/<branch>/src/hemisd/contrib/ptx-operator/install.sh
bash install.sh            # add --build to compile from source
```

The installer: checks the environment, **verifies the binary checksum** (it refuses an
unverified fetch), reserves the ports (append-safe — it will not clobber existing
`ip_local_reserved_ports` entries), writes a config with the standard ports and
dual-stack RPC bind, and finishes by running the self-check. Re-running is safe: it
never touches `wallet.dat` or `ptx_shares.dat`.

Then edit `~/.hemis-ptxbea/hemis.conf`:

```
externalip=YOUR.PUBLIC.IP          # the address you will register
rpcallowip=<the peer/fleet range>  # TIGHTEN the default — not 0.0.0.0/0
gamemasterblsprivkey=...           # after §Arm below
```

Start and sync (the loop you know):

```
Hemisd -ptxbea
Hemis-cli -ptxbea getblockchaininfo     # repeat until initial_block_downloading: false
```

## Register and arm

1. **Collateral**: send the collateral amount to a fresh address in this wallet; note
   `txid` and `vout`.
2. **Operator key**: `Hemis-cli -ptxbea bls generate` → the *secret* goes in the config
   as `gamemasterblsprivkey`, the *public* key goes into registration.
3. **Register** (this replaces the old start-from-wallet):
   ```
   Hemis-cli -ptxbea protx_register <collateralTxid> <collateralVout> \
       YOUR.PUBLIC.IP:29994 <ownerAddr> <operatorPubKey> <votingAddr> <payoutAddr>
   ```
4. **Arm**: confirm `gamemasterblsprivkey=` is in the config, then
   `Hemis-cli -ptxbea stop && Hemisd -ptxbea` (the restart ritual still works).

## What healthy looks like

Run the self-check any time:

```
bash contrib/ptx-operator/self-check.sh
```

A healthy node: every line PASS — synced, peers, registered (DGM advertises
`YOUR.IP:29994`), **Ready**, RPC bound beyond loopback, share held for each quorum it is
a member of, ports reserved. `getgamemasterstatus` says `status: Ready`. The explorer
shows your collateral in the gamemaster list.

**Then verify RPC from another machine** (the one check that cannot be fully proven from
the node itself — a local probe can hairpin and false-pass):

```
Hemis-cli -ptxbea -rpcconnect=YOUR.PUBLIC.IP -rpcport=29995 \
          -rpcuser=<u> -rpcpassword=<p> getblockcount
```

If this times out, you are in the silent-failure state: healthy on-chain, never signing.

## When it isn't healthy — the failure modes we have actually seen

| Symptom | Meaning | Fix |
|---|---|---|
| Registered but status ≠ Ready | **Unarmed** — operator key not loaded | `gamemasterblsprivkey=` in conf, restart |
| Ready but never appears in signing / rolls fail against your quorum | **Unreachable RPC** — bound to localhost, firewalled, or wrong address family | `rpcbind=[::]`, open 29995 at edge + ufw, verify from another host |
| Height stuck / behind peers | **Unsynced or partitioned** | check peers; `stop && Hemisd` (if in doubt, repeat); resync if wedged. If you ever see mass peer bans after a fork: `clearbanned`, and note a stale `banlist.dat` **survives a resync wipe** — delete it too |
| `ptx_quorum_health` shows `member: true, share_current: false` | **Share lost** | see §Recovery — there is no recovery; keep the node Ready |

## Recovery — share loss (ODC-071)

Your DKG share (`ptx_shares.dat`) is produced by an interactive ceremony. It is **not
derivable from the chain, not recomputable, not held by peers**. If it is lost, that
membership cannot sign again — ever. What you can and cannot get back:

- **Cannot**: the share itself, by any action. Do not re-register; do not reindex —
  neither helps (a plain restart or `-reindex` does NOT lose shares; deleting or
  restoring the datadir from an old backup DOES).
- **Can**: future shares. Keep the node running and Ready; it stays eligible, and the
  next ceremony it is selected into produces a fresh share. Your dead membership ages
  out when its quorum rotates/reforms on schedule.
- **Backups**: if you snapshot the datadir, the snapshot must **post-date the newest
  ceremony** this node completed — restoring an older one forfeits the newer shares
  permanently. Treat `ptx_shares.dat` with the same discipline as `wallet.dat`.

## Two facts to not build around

- **Quorum selection is advisory, not consensus-enforced** (`ptxbea-known-limitations.md`
  §12): the tip-hash routing runs only in the honest RPC; consensus binds
  *active-at-height*, not *routed*. Do not design anything that assumes a caller cannot
  aim a roll at a chosen quorum.
- **The RPC port convention is interim** (§13): KDD-085 (sign-over-P2P) removes the RPC
  exposure requirement. Until then, 29995 open at your registered address is what makes
  you a functioning signer.
