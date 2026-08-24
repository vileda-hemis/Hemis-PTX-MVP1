# PTX roll verifier

One page. Paste a PTX payload as hex, get it decoded with byte boundaries marked and
independently re-derived. **No node, no index, no database, no dependencies** — stdlib only.

```
python3 app.py                 # http://127.0.0.1:8710
python3 test_decoder.py        # the suite that licenses the above
```

## Why it exists

Consensus does **not** check that a roll's results follow from its beacon, nor that its beacon
follows from a real threshold signature. `PTX_MapBeacon` appears in `src/rpc/ptx.cpp` only
(`:406`, `:1273`) and **never** in `validation.cpp`; `quorum_sig` appears in `validation.cpp`
**zero** times. The header says as much itself (`src/primitives/transaction.h:540-542`).

★ Until W4-b lands, this page is the only thing performing those checks — which is why the
verifier was built before any indexer.

## What it checks

| | claim | needs |
|---|---|---|
| **P** | `ptx_params_hash == SHA256d(count ‖ low ‖ high ‖ unique ‖ excludes)` — `ptx_seed.cpp:44-51` | payload only |
| **A** | `round_seed == SHA256(game_id ‖ LE32(height) ‖ caller_pubkey ‖ nonce ‖ params_hash)` — `ptx_seed.cpp:24-40` | payload only |
| **B** | `beacon == SHA256(quorum_sig)` — `ptx_bls.cpp:777-782` | payload only |
| **C** | `results == PTX_MapBeacon(...)` — `ptx_output_mapping.cpp:50-100` | payload only |
| **D** | signature verifies under the quorum's group key at `quorum_hash` | **not performed** — needs a synced node |

P is not in the original A/B/C set and belongs there: A folds `params_hash` into the seed, so
without P an attacker could put any `params_hash` in the payload and A would still pass.

D is stated on the page as not performed, with the reason. A page that verifies P/A/B/C and says
so is more credible than one that quietly omits the distinction.

★ **`caller_pubkey` is not a seed input, and on a commitment it is not a pubkey either.** Measured,
not assumed: across 141 real ptxbea `PTXROLLCOMMIT` payloads the `round_seed` reproduces 141/141
with an **empty** `caller_pubkey` and 0/141 with the value the payload carries. `src/rpc/ptx.cpp:225`
passes `{}` into the seed while `:300` stores the caller **salt** in that field; the salt reaches
the seed only through the nonce (`PTX_BuildNonce`, `:218`). Check A tries the field first, falls
back to empty, and says which reproduced.

## The decoder is generated, not hand-written

`ptx_payload_spec.py` reads the field order out of `SERIALIZE_METHODS` in
`src/primitives/transaction.h` — the same declaration consensus serialises with. `ptx_decode.py`
knows wire primitives and nothing about layout.

This is not stylistic. The predecessor (`explorer/proxy.py:162-195`) hand-rolled a second
serialiser, and it was **one field stale after one payload change**: it silently dropped
`quorum_hash`, the field that makes "this quorum produced this output" chain-computable
(KDD-074/076). It survived only because that field was appended *last* — a field inserted
mid-struct would have mis-decoded everything after it and produced plausible wrong numbers with
no error at all.

`test_decoder.py` pins the extracted spec against `payload_spec.golden` and **fails loudly** when
the header changes. That test is the point of the exercise, not a nicety. Regenerate deliberately:

```
python3 ptx_payload_spec.py > payload_spec.golden
```

## PTX_MapBeacon is ported, not delegated

Check C could have called a node's RPC — safer against porting error, but it would cost the
property that makes this page worth having: verification from hex alone, with no infrastructure.

The port is justified by evidence, not confidence. `test_decoder.py` replays it against real
settled ptxbea payloads and requires an **exact** match on `results`, across the unique and
non-unique paths and with non-empty exclusion lists. A subtly-wrong reimplementation produces
confident wrong verdicts, which is worse than no verifier.

The suite also shows every check **failing** against deliberately corrupted payloads. Without that
limb, "281/281 pass" is consistent with checks that cannot fail — the vacuity trap the cold-sync
harness exists to expose, pointed at this verifier.

Harvest your own fixtures from a node you own: `python3 harvest_fixtures.py --help`.

## What it will never show

Per-round gamemaster participation — who was asked to sign and stayed silent — and
commitment→response timing are **not derivable from chain data by anyone**, including us. They
live in `g_ptx_rounds`, an in-memory map (`src/ptx/ptx_commit_reveal.cpp:12`) filled by a node's
own fan-out participation, lost on restart and empty on any node that did not take part.

The honest on-chain proxy is `quorum_members`, which names the members whose shares composed the
signature. The page renders that and says why the rest is absent, rather than sourcing a
participation column from a fleet node — which would be a privileged answer wearing a public label.

An on-chain non-responder set would make liveness auditable, at a payload-budget cost. That is a
question for a W-arc, not a plan.

## Data path

The page owns everything it touches. Txid lookup is **optional** and off unless `PTX_NODE_RPC`
(with `PTX_NODE_USER` / `PTX_NODE_PASS`) names a node the operator owns; the checks themselves
never use it. It reaches into no fleet container, no shared credential, and no node belonging to
anyone else.

The test to apply: *could an unrelated third party stand this up and get identical results?* For
P/A/B/C, yes — with nothing but Python.
