# PTX testnet — the onboarding packet

**Audience:** the coordinator. This is the short list of things that must exist **before the tag is
cut**, and the message each operator receives.

**Why this document exists:** a competent operator — someone who has run mainnet nodes for years —
still cannot guess an `addnode` address or an `rpcauth` line. Five values on this network are
minted once, by the coordinator, and are simply unavailable to anyone else. Without them an
operator installs cleanly, starts a healthy-looking daemon, and sits at height 0 with zero peers.
That is the whole failure this page prevents.

---

## The five values

| value | minted by | when | consumed by |
|---|---|---|---|
| `addnode=` × 3 | coordinator, after standing up the three coordinator nodes | **before the tag** | `install.sh` `emit_conf`, via `PTX_SEEDS` |
| `rpcallowip=<caller>` | coordinator — the caller node's public address | **before the tag** | GM config, via `PTX_CALLER` |
| `rpcauth=ptxcaller:<salt>$<hmac>` | coordinator | **before the tag** | GM config, via `PTX_RPCAUTH` |
| spork **public** key (hex) | coordinator, see below | **before the tag** | `src/chainparams.cpp:786` |
| genesis `nTime` / nonce | `findGenesisPTXBea()` | **before the tag** | `src/chainparams.cpp:740-744` |

**The first three ship as documented placeholders.** `install.sh` writes commented placeholder
lines and warns loudly when `PTX_SEEDS`, `PTX_CALLER` or `PTX_RPCAUTH` is empty, so an operator who
installs before you have finalised them gets a working node and a named gap rather than a silent
one. Set them at cut time and they are baked into the script the operators clone.

**The last two are compiled in and gate the tag.** They cannot be changed after operators are
running without a new genesis or a new binary.

---

## 1. Genesis

`findGenesisPTXBea()` is commented out at `src/chainparams.cpp:202-228`; the `found` flag it needs
is live at `:24`. Uncomment, set `nTime` to the intended launch date at 00:00:00 UTC, run once,
record the nonce and hash, update **both** asserts at `:740-744`, and add the genesis-only
`mapCheckpoints` entry.

★ The merkle root will be **unchanged** (`93ad7b45…`) — it derives from the shared genesis coinbase
message. That is expected, not a mistake.

★ Budget seconds, not hours. At `nBits = 0x1e00ffff` a genesis is ~16.8 million hashes; ptxbea's
winning thread found its nonce after ~136,000 iterations.

---

## 2. The spork keypair

The public half goes in the repository. **The private half must never be in this repository, and
the coordinator is the only person who ever sees it.**

Generate on any node of a network sharing `base58Prefixes[SECRET_KEY] = 239` — ptxtestnet
(`:893`), ptxbea (`:1104`), Hemis testnet (`:515`) and regtest (`:662`) all do; **mainnet is 212
and will produce a WIF this chain cannot decode**.

### Run this on px1 — paste-ready, nothing to edit

**Host: px1 (`192.168.99.85`), reachable by key from node1.** Chosen over `ptx01` for three
reasons, all measured 2026-08-23: it carries binaries built from **current source** (`ad51d1c`), so
`validateaddress` and `dumpprivkey` behave exactly as the launch build will; the datadir below is
**brand new**, so the wallet holding the spork key can never be the wallet holding the 193,800 HMS;
and a ptxtestnet node was started there minutes before this was written and served wallet RPC
(`Creating HD Wallet`, `ActivateSaplingWallet : sapling spkm setup completed`). `ptx01` could not
be verified — it is not a container on node1 and the px1 guests do not answer the guest agent, so
nothing here can confirm it comes up far enough to serve wallet RPC.

The chain state is irrelevant: this needs the wallet only. Height 0 with no peers is fine.

```bash
ssh root@192.168.99.85

# --- a disposable datadir, used for nothing else, ever ---
mkdir -p /root/ptx-spork/.hemis-ptxtestnet
cat > /root/ptx-spork/.hemis-ptxtestnet/Hemis.conf <<'EOF'
ptxtestnet=1
[ptxtestnet]
rpcuser=sporkgen
rpcpassword=sporkgen
rpcport=29975
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
port=29974
listen=1
EOF

# --- start; the sapling params are already at /root/.Hemis-params ---
/root/ptx-build/bin/Hemisd -datadir=/root/ptx-spork/.hemis-ptxtestnet -daemon

# ★ VERIFY BY OUTCOME. `-daemon` forks and exits 0 even when startup then fails,
#   so "Hemis server starting" and $?=0 mean nothing. This is the real check:
sleep 30
/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet getblockcount
# -> 0    (a number, not an error, is what you need; 0 is correct here)

# --- the three commands ---
ADDR=$(/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet \
        getnewaddress "ptxtestnet-spork")
echo "$ADDR"

/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet \
        validateaddress "$ADDR"
# -> read TWO fields off this: "pubkey" (the hex) and "iscompressed"

/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet \
        dumpprivkey "$ADDR"
# ★★ THIS IS THE WIF. IT DOES NOT LEAVE THIS HOST.
#    Not into chat, not into a report, not into the standup, not into git.
#    The ONLY value that travels is the "pubkey" hex from validateaddress.

# --- store the private half ---
umask 077
printf '%s\n' "<paste the WIF here>" > /root/ptxtestnet_spork.key
chmod 600 /root/ptxtestnet_spork.key
export PTXTESTNET_SPORK_KEY=/root/ptxtestnet_spork.key

# --- stop it ---
/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet stop
```

★ **Send back the `pubkey` hex, and only that.** Not the address, not the WIF, not the whole
`validateaddress` object — the one hex string from the `pubkey` field.

★ **Before launch, move `/root/ptxtestnet_spork.key` into the coordinator's real secret store and
shred the px1 copy.** px1 is a build host; it is where this was convenient to make, not where a
production credential should live. The ptxbea precedent is the same shape:
`chainparams.cpp:1042` records that its private half *"lives in `$PTXBEA_SPORK_KEY` in the
environment, never in this repo"*.

Put the **`pubkey` hex verbatim** at `src/chainparams.cpp:786`, replacing the shared-with-Hemis-
testnet key the `TODO(launch)` marker sits on.

★★ **Both halves must be the same compression form.** Verification recovers a pubkey from the
signature and compares `CKeyID` — the HASH160 (`messagesigner.cpp:88-104`) — and
`CKey::SignCompact` sets the recovery header from `fCompressed` (`key.cpp:243`). A compressed WIF
with an uncompressed hex pubkey does not match. Use the `pubkey` field verbatim and do not convert
it by hand; `validateaddress` reports `iscompressed` so you can see which you have.

**Prove the pair before launch — the daemon does it for you, and it is fatal if wrong:**

```bash
# On px1, AFTER a build that has the new pubkey compiled in:
/root/ptx-build/bin/Hemisd -datadir=/root/ptx-spork/.hemis-ptxtestnet \
    -sporkkey="$(cat /root/ptxtestnet_spork.key)" -daemon
sleep 30
grep -E "Successfully initialized as spork signer|wrong key" \
    /root/ptx-spork/.hemis-ptxtestnet/ptxtestnet/debug.log
/root/ptx-build/bin/Hemis-cli -datadir=/root/ptx-spork/.hemis-ptxtestnet stop
```

★ This step is **worthless against the current binaries** — they still carry the shared
Hemis-testnet key, so any freshly generated pair will "fail" correctly and tell you nothing about
your pair. Run it only after a build with the new pubkey in `chainparams.cpp:786`.

`SetPrivKey` signs a test message and verifies it against the compiled-in pubkey
(`src/spork.cpp:266-289`); a mismatch makes the daemon refuse to start
(`src/init.cpp:1241-1244`). A pubkey with no matching private half is exactly the failure the
ptxbea comment at `:1037-1046` records, and it was not discovered until someone needed a spork.

---

## 3. The three coordinator nodes

See `GENESIS_BOOTSTRAP.md` step 2. Their addresses are the `PTX_SEEDS` value.

★ **This is seven machines, not five:** the mining/caller host, two more coordinator nodes, and
four gamemaster hosts. All three coordinator nodes must be publicly reachable on **29994** — they
are the only bootstrap peers this network has.

---

## 4. The caller credential

One credential, shared with every operator, and the **only** secret that crosses between the
coordinator and anyone else.

Use the canonical generator, which is in this repository:

```bash
python3 share/rpcauth/rpcauth.py ptxcaller
```

```
String to be appended to bitcoin.conf:
rpcauth=ptxcaller:566bdb5e...f52$d4054cb7...43f8
Your password:
v-PxXJClW_7SyS_2oyHifHDhLRgHLZzRXtFwzhzFPsM=
```

★ It lives at `share/rpcauth/rpcauth.py`. The `-rpcauth` help text says `share/rpcuser`
(`src/init.cpp:562`), which is not a directory that exists — ignore the help, use the path above.
The hash it produces is `HMAC-SHA256(key=salt, msg=password)`, which is exactly what the daemon
recomputes at `src/httprpc.cpp:112-117`.

* the **`rpcauth` line** goes to every operator (`PTX_RPCAUTH`) — it contains only a hash;
* the **password** goes in the caller node's own `Hemis.conf` as `rpcuser`/`rpcpassword`, because
  the fan-out sends the dialling node's own credentials (`src/ptx/ptx_fanout.cpp:612-616`).

★ **What it grants, stated plainly.** There is no per-method restriction in this daemon —
`jreq.authUser` (`src/httprpc.cpp:157`) is never read and there is no `-rpcwhitelist` — so this
credential can call **any** RPC on any gamemaster that accepts it, including `stop` and the wallet.
It is paired with `rpcallowip=<caller>` so only the caller's address can present it. Treat it as a
network-wide secret: rotating it means every operator edits one line and restarts.

★ **A dedicated `-ptxfanoutuser` / `-ptxfanoutpassword` pair would remove this trade** by letting
the fan-out authenticate as something other than the caller's own RPC identity. It does not exist;
recorded as an item, not built.

---

## The message each operator gets

Everything below is public. Nothing here is secret except in the sense that the `rpcauth` line
should not be posted where strangers can find it.

```
Tag:            v0.1.0-testnet          (use it in BOTH places: git clone -b <tag>, and PTX_REF=<tag>)
Repository:     https://github.com/vileda-hemis/Hemis-PTX-MVP1.git

Gamemasters:    4, on 4 separate hosts, each with its own routable address
Ports:          29994 P2P (open to the internet), 29995 RPC (open to the caller address below only)
Collateral:     100 HMS per gamemaster, EXACTLY. Not 1000.
Funding:        I will send you 500 HMS once you send me an address (see the guide: install,
                start, RESTART ONCE, then send the address -- in that order)

These are already baked into the installer; you should not need to type them, but if
install.sh warns that any is missing, these are the values:

  PTX_SEEDS      = <addr1> <addr2> <addr3>
  PTX_CALLER     = <caller address>
  PTX_RPCAUTH    = ptxcaller:<salt>$<hmac>
  PTX_EXTERNALIP = <your own host's public address -- one per host, you set this>

Read testnet/operator/OPERATOR_GUIDE.md and follow it literally.
Report a node as ready ONLY when `getgamemasterstatus` says "status": "Ready"
and ./self-check.sh exits 0.

Expect ZERO quorums and zero rolls until eleven gamemasters are registered. That is
the system working, not a fault.
```

---

## Order of operations

1. Regenerate genesis; update the asserts. *(gates the tag)*
2. Generate the spork keypair; put the public half in chainparams; prove it starts a daemon. *(gates the tag)*
3. Stand up the three coordinator nodes; record their addresses.
4. Mint the caller credential.
5. Set `PTX_SEEDS`, `PTX_CALLER`, `PTX_RPCAUTH` in `install.sh` and commit.
6. Run `testnet/operator/install-test.sh` — a non-zero exit blocks the tag (`PTX_TESTNET_RELEASE.md` step 0).
7. Cut the tag; verify the artefacts against `SHA256SUMS`.
8. Run `GENESIS_BOOTSTRAP.md` end to end.
9. Send the message above to each operator, with their 500 HMS.
