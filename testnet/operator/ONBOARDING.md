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

```bash
ADDR=$(Hemis-cli -datadir=<datadir> getnewaddress "ptxtestnet-spork")
Hemis-cli -datadir=<datadir> validateaddress "$ADDR"   # -> "pubkey": <hex>, "iscompressed"
Hemis-cli -datadir=<datadir> dumpprivkey     "$ADDR"   # -> the WIF. NEVER LEAVES THIS HOST.

umask 077
printf '%s\n' "<the WIF>" > /path/to/ptxtestnet_spork.key
chmod 600 /path/to/ptxtestnet_spork.key
```

Put the **`pubkey` hex verbatim** at `src/chainparams.cpp:786`, replacing the shared-with-Hemis-
testnet key the `TODO(launch)` marker sits on.

★★ **Both halves must be the same compression form.** Verification recovers a pubkey from the
signature and compares `CKeyID` — the HASH160 (`messagesigner.cpp:88-104`) — and
`CKey::SignCompact` sets the recovery header from `fCompressed` (`key.cpp:243`). A compressed WIF
with an uncompressed hex pubkey does not match. Use the `pubkey` field verbatim and do not convert
it by hand; `validateaddress` reports `iscompressed` so you can see which you have.

**Prove the pair before launch — the daemon does it for you, and it is fatal if wrong:**

```bash
Hemisd -datadir=<disposable> -sporkkey="$(cat /path/to/ptxtestnet_spork.key)" -daemon
grep -E "Successfully initialized as spork signer|wrong key" <disposable>/ptxtestnet/debug.log
```

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
