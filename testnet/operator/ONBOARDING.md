# PTX testnet — the onboarding packet

**Audience:** the coordinator. This is the short list of things that must exist **before the tag is
cut**, and the message each operator receives.

**Why this document exists:** a competent operator — someone who has run mainnet nodes for years —
still cannot guess an `addnode` address. Three values on this network are minted once, by the
coordinator, and are simply unavailable to anyone else. Without them an operator installs cleanly,
starts a healthy-looking daemon, and sits at height 0 with zero peers. That is the whole failure
this page prevents.

---

## The three values — and all three are public

★★ **There is no secret to distribute, and no operator has to be trusted to keep one.** That is the
plainest statement of what KDD-085 achieved, and it is worth reading before the table rather than
inferring it from one.

| value | minted by | when | consumed by |
|---|---|---|---|
| `addnode=` × 3 | coordinator, after standing up the three coordinator nodes | **before the tag** | `install.sh` `emit_conf`, via `PTX_SEEDS` |
| spork **public** key (hex) | coordinator, see below | ★ **DONE** — `03612ded…a4e1ff` landed `34e96e1` | `src/chainparams.cpp:828` |
| genesis `nTime` / nonce | a standalone miner, not `findGenesisPTXBea()` | ★ **DONE** — `1787443200` / `10954950` landed `34e96e1` | `src/chainparams.cpp:751-755` |

**The first ships as a documented placeholder.** `install.sh` writes commented placeholder lines and
warns loudly when `PTX_SEEDS` is empty, so an operator who installs before you have finalised the
seeds gets a working node and a named gap rather than a silent one.

**The last two are compiled in and gate the tag.** They cannot be changed after operators are
running without a new genesis or a new binary.

★★ **THERE IS NO AUTOMATION FOR THE SEED LIST, AND NO PATH TO ONE TODAY.** `PTX_SEEDS` is consumed
**once, at install time**. Nothing re-reads it, nothing distributes updates, and a running node
never learns you changed it. Growing the published set is a manual action in **two** places — the
recorded table in `GENESIS_BOOTSTRAP.md` and `exp01`'s own `addnode` list. ★ The explorer's Add
Nodes tab is a **view of exp01's connections**, not a registry: it lists peers exp01 has an
**outbound** connection to, and a node that only ever dials *in* arrives on an ephemeral source port
and is filtered out of it. So an operator becomes visible when **exp01 dials them** — because you
added them, or because address relay promoted them to an outbound slot. **The second does happen**
(`::9500` reached the list that way, unseeded) but is neither prompt nor guaranteed.

★★ **AND ONE THING YOU MUST TELL OPERATORS BEFORE THEY PROVISION: every host needs a global IPv6
address.** This is not a value you mint, it is a sentence you send — and it has to arrive *before*
they buy a machine, because it cannot be fixed afterwards without registering again at a new
address. `install.sh` enforces it for gamemasters and aborts before writing anything, so the worst
case is a wasted provisioning rather than a broken node; but an operator who hears it from the
installer has already paid for the wrong VM.

★ **Two coupled obligations of your own follow from it.** (1) **The seeds you mint above are IPv6**,
so an IPv4-only host has nothing to dial and sits at height 0 — the requirement applies to WALLET
hosts too, for that reason alone. (2) If the policy is ever widened to admit IPv4 hosts, **IPv4
seeds become a prerequisite, not a follow-up** — without them an IPv4 operator cannot bootstrap at
all, and the only symptom is `getconnectioncount: 0`. See KDD-110.

### ★★ There used to be five, and two of them were secrets — KDD-085 deleted both

This section is kept rather than removed, because *what changed* is the point.

`rpcallowip=<caller>` and `rpcauth=ptxcaller:<salt>$<hmac>` used to sit in the table above. They
were the **only two values requiring confidentiality**, they were **identical on every gamemaster**
(`install.sh` said so in as many words: "unchanged when another operator joins"), and the caller
transmitted that password in plaintext to all eleven members of every roll as HTTP Basic — so **any
member could read it off its own wire and present it to the other ten**.

★ **They were not replaced by better-scoped credentials. They were deleted, because the mechanism
was answering the wrong question (KDD-105).** A gamemaster's real authorisation is the on-chain
payment gate — *"is there a funded commitment for this exact round?"* — which every GM answers
independently, from data it already holds, about a caller it has never heard of. A secret standing
in for a check the network can already perform is strictly weaker than that check: it can be stolen,
must be distributed, must be rotated, and forces mutual trust between operators who otherwise need
none.

★ **What you no longer have to do**, all of it gone rather than documented better:

- no caller credential to request from the coordinator, hold, or rotate;
- no `rpcallowip` entry for the caller — **RPC is loopback-only now**, a local admin interface;
- no inbound RPC port to open, forward, or get the address family right on;
- no `-ptxfanoutport` to match;
- and the four failure modes those produced — 401, 403, wrong fan-out port, RPC unreachable — which
  all shared one signature: **the gamemaster registers, syncs, shows ENABLED, and silently refuses
  every signing request.**

Signing requests now arrive over P2P at the address you already register on chain, on the port you
already advertise. See `doc/ptx/W4B_COST_AND_KDD085_SCOPE.md` §9, and KDD-105 for why per-operator
credentials were rejected rather than adopted as a mitigation.

---

## 1. Genesis

### ★ The launch date is fixed: **2026-08-23, 00:00:00 UTC** → `nTime = 1787443200`

Vileda's decision, 2026-08-23. Use this value; do not recompute it from "today" when you run the
generator, or two people will produce two different genesis blocks.

★★ **THIS IS NOT A COMMITMENT TO LAUNCH ON THAT DATE.** `nTime` is a *timestamp written into block
0*, not a trigger and not a deadline. The chain begins when **block 1** is mined, which may be days
or weeks later, and **a genesis `nTime` in the past needs no re-mine** — which is exactly why it is
safe to fix the value now and take the time you need over everything else. The only visible artefact
is a gap between block 0 and block 1 in an explorer, and that is cosmetic.

★ Midnight UTC is worth choosing rather than "now": 1787443200 is divisible by 86400 and therefore
by `nTimeSlotLength = 15` (`chainparams.cpp:776` for ptxtestnet), so the Time Protocol v2 timestamp
mask `nTime % nTimeSlotLength == 0` is satisfied for free. An arbitrary wall-clock second has a
14-in-15 chance of failing it.

### ★★ GENESIS IS ALREADY MINED AND COMMITTED — DO NOT RE-RUN THE MINER

Landed in `34e96e1`: **`nTime = 1787443200`, `nNonce = 10954950`**, hash
`00000094a6ac77ae3503093e9529981cc724c4410265f727bc762f713b0da6e2`, at
`src/chainparams.cpp:751-755` with the height-0 checkpoint and `dataPTXTestNet`'s timestamp updated
to match. **Re-running any miner would produce a different block and invalidate the tag.**

★ It was mined by a **standalone single-translation-unit miner**, not by the in-tree
`findGenesisPTXBea()` (still commented out at `:202-228`, its `found` flag live at `:24`). The
in-tree one runs from inside the daemon, so it costs one build to mine and a second to compile the
answer; the header hash depends on nothing but 80 bytes and `HashQuark`, so a standalone program
linking this tree's own quark primitives gets the same number for one build. That miner
**re-derived the PREVIOUS committed genesis from its own `(1779115621, 9801932)` and refused to run
unless the hash came back byte-identical** — the instrument verified against a known-correct answer
before being trusted with a new one. It ran single-threaded from nonce 0, so `10954950` is the
**minimal** nonce and is deterministically reproducible; the striped in-tree miner returns whichever
thread wins, which is not the same property.

★ Only ONE assert changed. `hashMerkleRoot` is invariant across all five networks — the coinbase
depends on `pszTimestamp`, the output script and the reward, never on `nTime` or `nNonce`.

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

### Run this on ptx01 — paste-ready, nothing to edit

**Host: `ptx01`.** It is already installed, already has a wallet, and you have direct access to it.

★ **Its binaries are old, and that is fine — verified, not assumed.** ptx01 runs `v0.1.0-testnet`
(`1591450`), which predates the Gate 0 cut. What matters for a key is the WIF prefix, and in
`1591450`'s own tree `base58Prefixes[SECRET_KEY]` for ptxtestnet is **239** — byte-identical to
current source (`chainparams.cpp:893`), as are regtest (`:662`) and ptxbea (`:1104`), and the
address prefix `PUBKEY_ADDRESS` is 139 in both. **So a key generated on ptx01 today decodes on the
launch build.** The Gate 0 cut changed `UPGRADE_V6_0` and the magic; it did not touch the base58
prefixes.

★ **A fresh, disposable datadir — not ptx01's node datadir, and not the default.** Whichever wallet
this runs in ends up holding the spork key, so it must not be the wallet that will hold 193,800 HMS
and it must not be the node's own. The commands below therefore pass `-datadir` explicitly, which
is deliberate: everywhere else in these documents `-datadir` has just been removed, because the
installed node now lives in the daemon's default directory. This is the one place a separate
directory is the point.

The ports below are deliberately outside every default this chain uses, so this cannot collide with
the node already running on ptx01. ★ The defaults, from source rather than memory: ptxtestnet is
P2P **29993** (`chainparams.cpp:886`) and RPC **29995** (`chainparamsbase.cpp:69`) — and on the
binary ptx01 actually runs, `1591450`, the RPC default is **29902** (`chainparamsbase.cpp:49` in
that tree; the 29902→29995 change landed later the same day). 29974/29975 clears all three, and
29994 as well, which is **ptxbea's** P2P port (`chainparams.cpp:1097`), not ptxtestnet's. None of
this actually reaches the daemon below — the conf sets both ports explicitly — but the reason for
the choice should be the true one.

The chain state is irrelevant — this needs the wallet only, and height 0 with zero peers is fine.

```bash
# --- a disposable datadir, used for nothing else, ever ---
mkdir -p $HOME/.hemis-spork
cat > $HOME/.hemis-spork/Hemis.conf <<'EOF'
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

# --- start it ---
Hemisd -datadir=$HOME/.hemis-spork -daemon

# ★ VERIFY BY OUTCOME. `-daemon` forks and exits 0 even when startup then fails,
#   so "Hemis server starting" and $?=0 mean nothing. This is the real check:
sleep 30
Hemis-cli -datadir=$HOME/.hemis-spork getblockcount
# -> a number (0 is correct here). If instead it says it cannot connect, the
#    daemon died: check $HOME/.hemis-spork/ptxtestnet/debug.log. The usual cause
#    is the sapling params; they live in $HOME/.Hemis-params and ptx01's install
#    already put them there, but you can point at them with -paramsdir=<dir>.

# --- the three commands ---
ADDR=$(Hemis-cli -datadir=$HOME/.hemis-spork getnewaddress "ptxtestnet-spork")
echo "$ADDR"

Hemis-cli -datadir=$HOME/.hemis-spork validateaddress "$ADDR"
# -> read TWO fields off this: "pubkey" (the hex) and "iscompressed"

# --- the private half: straight to a 0600 file, NEVER to the terminal ---
umask 077
Hemis-cli -datadir=$HOME/.hemis-spork dumpprivkey "$ADDR" > $HOME/ptxtestnet_spork.key
chmod 600 $HOME/ptxtestnet_spork.key
export PTXTESTNET_SPORK_KEY=$HOME/ptxtestnet_spork.key
# ★★ THAT FILE IS THE WIF. IT DOES NOT LEAVE THIS HOST.
#    Not into chat, not into a report, not into the standup, not into git.
#    The ONLY value that travels is the "pubkey" hex from validateaddress.
# ★ It is redirected, not printed, DELIBERATELY: the earlier form ran a bare
#   `dumpprivkey` and then asked you to paste the result back in, which put the
#   one value that must not travel into your scrollback and your shell history
#   before it ever reached the file. `umask 077` means the redirect creates it
#   0600 already; the chmod is belt-and-braces.
# Confirm it landed WITHOUT echoing it — a length and a mode, not the value:
wc -c < $HOME/ptxtestnet_spork.key   # -> 53 (52-char WIF + newline)
stat -c '%a' $HOME/ptxtestnet_spork.key   # -> 600

# --- stop it ---
Hemis-cli -datadir=$HOME/.hemis-spork stop
```

★ **Send back the `pubkey` hex, and only that.** Not the address, not the WIF, not the whole
`validateaddress` object — the one hex string from the `pubkey` field.

★★ **Compression must match, and this is the one that fails silently.** `validateaddress` will
almost certainly report `"iscompressed": true` and a **66-character** pubkey. **Paste it verbatim.**
Do not convert it to the 130-character uncompressed form to make it look like the `047F4B27…` it
replaces. Verification recovers a pubkey from the signature and compares the `CKeyID` — the HASH160
(`messagesigner.cpp:88-104`) — and `CKey::SignCompact` encodes the compression flag in the recovery
header (`key.cpp:243`), which `RecoverCompact` reconstructs from. A compressed WIF against an
uncompressed hex pubkey yields a different HASH160 and the signature never verifies.

★ **Before launch, move `$HOME/ptxtestnet_spork.key` into the real secret store.** The ptxbea
precedent is the same shape: `chainparams.cpp:1042` records that its private half *"lives in
`$PTXBEA_SPORK_KEY` in the environment, never in this repo"*.


**Prove the pair before launch — the daemon does it for you, and it is fatal if wrong:**

★★ **DO NOT RUN THIS YET, AND DO NOT READ ITS FAILURE AS A PROBLEM.** Against the binaries that
exist today it is **worthless**: they still carry the shared Hemis-testnet spork pubkey, so a
freshly generated pair *will* be rejected — correctly, and it tells you nothing about your pair.
The check only becomes meaningful once a build has **your** pubkey compiled into
`chainparams.cpp:828`. Run it after that rebuild, not before.

```bash
# AFTER a build that has the new pubkey compiled in. Same disposable datadir.
Hemisd -datadir=$HOME/.hemis-spork \
    -sporkkey="$(cat $HOME/ptxtestnet_spork.key)" -daemon
sleep 30
grep -E "Successfully initialized as spork signer|wrong key" \
    $HOME/.hemis-spork/ptxtestnet/debug.log
Hemis-cli -datadir=$HOME/.hemis-spork stop
```

`SetPrivKey` signs a test message and verifies it against the compiled-in pubkey
(`src/spork.cpp:266-289`); a mismatch makes the daemon refuse to start
(`src/init.cpp:1246-1249`). A pubkey with no matching private half is exactly the failure the
ptxbea comment at `:1037-1046` records, and it was not discovered until someone needed a spork.

★ **The grep can see BOTH branches — checked, because a grep that only ever matches the pass is
not a test.** Success writes `Successfully initialized as spork signer` via `LogPrintf`
(`spork.cpp:283`). Failure returns `UIError(_("Unable to sign spork message, wrong key?"))`
(`init.cpp:1249`), and `UIError` raises `MSG_ERROR` **without** the `SECURE` flag
(`guiinterfaceutil.h:12`), which is the flag that would suppress logging — so `noui.cpp:39`
`LogPrintf`s it to `debug.log` as well as stderr. Both outcomes leave a line; **empty output means
neither happened**, i.e. the daemon never got that far, which is a third thing and not a pass.

★ On the failure branch the daemon is already gone, so the trailing `stop` will answer *"couldn't
connect to server"*. That is the expected consequence of the refusal, not a second fault.

---

## 3. The three coordinator nodes

See `GENESIS_BOOTSTRAP.md` step 2. Their addresses are the `PTX_SEEDS` value.

★ **This is seven machines, not five:** the mining/caller host, two more coordinator nodes, and
four gamemaster hosts. All three coordinator nodes must be publicly reachable on **29994** — they
are the only bootstrap peers this network has.

---

## 4. The caller credential — DELETED, nothing to mint

★★ **This section used to tell you how to generate a shared secret. There is no longer one to
generate, and that is KDD-085's whole result.**

It described minting `rpcauth=ptxcaller:<salt>$<hmac>` with `share/rpcauth/rpcauth.py`, shipping the
hash to every operator and the password to the caller node — *"one credential, shared with every
operator, and the only secret that crosses between the coordinator and anyone else."*

★ **What was wrong with it was not its scope, it was its existence.** The section itself recorded
the damage plainly: this daemon has **no per-method restriction** — `jreq.authUser` is never read
and there is no `-rpcwhitelist` — so the credential could call **any** RPC on any gamemaster that
accepted it, `stop` and the wallet included. It also noted that a dedicated fan-out credential
*"would remove this trade"*, recorded as an item and not built.

★★ **It was not built, because narrowing it was never the fix (KDD-105).** The credential answered
*"do you know a secret?"* when the question a gamemaster actually needs answered is *"is there a
funded commitment for this exact round?"* — and that one every GM answers **independently, from
chain data it already holds, about a caller it has never heard of.** A secret standing in for a
check the network can already perform is strictly weaker than the check: it can be stolen, must be
distributed, must be rotated, and forces mutual trust between operators who otherwise need none.
Issuing five credentials instead of one would have narrowed the blast radius and left the model
intact — *a mitigation that scales the damage without changing the question is a delay, not a
remedy.*

**So: nothing to mint here, nothing to send, nothing to rotate, and nothing an operator can leak.**
Signing requests arrive over P2P at the address each gamemaster already registers on chain, and the
on-chain payment gate decides. Section 3's three values are all public.

---

## The message each operator gets

★★ **Everything below is public, without qualification.** There is no longer a sentence here about
one line that should not be posted where strangers can find it, because there is no longer such a
line. That is the shortest description of what KDD-085 changed.

```
Tag:            v0.3.5-testnet          (use it in BOTH places: git clone -b <tag>, and PTX_REF=<tag>)
Repository:     https://github.com/vileda-hemis/Hemis-PTX-MVP1.git

Gamemasters:    <N>, one per host, each host with its own routable address
                (I have set N for you; it is not a figure from the guide)
Machines:       your GM hosts install with   ./install.sh            (role: gamemaster)
                your ONE wallet machine with PTX_ROLE=wallet ./install.sh
                install.sh PRINTS the role it built -- check that line. A GM
                built as a wallet registers, looks healthy, and never signs.
Ports:          29994 P2P -- open to the internet, ON EVERY MACHINE INCLUDING THE
                WALLET ONE. The wallet host listens too, so that it returns peers
                to a network that has no DNS seed; it still advertises no address
                and registers nothing.
                29995 RPC -- LOOPBACK ONLY, both roles. Do not open it. Do not
                forward it.
Collateral:     100 HMS per gamemaster, EXACTLY. Not 1000.
Funding:        I will send you <(N x 100) + 500> HMS once you send me an address (see the
                guide: install, start, RESTART ONCE, then send the address -- in that
                order). That is your N collaterals plus 100 of margin.

These are already baked into the installer; you should not need to type them, but if
install.sh warns that any is missing, these are the values:

  PTX_SEEDS      = [2a07:244:46:6400::9100]:29994 [2a07:244:46:6400::9200]:29994 \
                   [2a07:244:46:6400::9300]:29994 [2a07:244:46:6400::9010]:29994 \
                   [2a07:f1c0::4001:0:0:1]:29994
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
4. ~~Mint the caller credential.~~ ★ **Step removed by KDD-085 — there is no credential to mint.**
   Kept struck through rather than deleted so anyone working from an older copy of this list can
   see that the step is gone deliberately, not overlooked.
5. Set `PTX_SEEDS` in `install.sh` and commit.
6. Run `testnet/operator/install-test.sh` — a non-zero exit blocks the tag (`PTX_TESTNET_RELEASE.md` step 0).
7. Cut the tag; verify the artefacts against `SHA256SUMS`.
8. Run `GENESIS_BOOTSTRAP.md` end to end.
9. Send the message above to each operator, with their funding — `(N x 100) + 500` HMS for the
   N you agreed with them. ★ Fill in `<N>` per operator before sending; the message template
   deliberately carries a placeholder, not a number.
