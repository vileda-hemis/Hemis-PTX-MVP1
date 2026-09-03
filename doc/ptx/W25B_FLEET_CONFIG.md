# W2.5b Fleet Configuration — REFERENCE ONLY (15 GM)

> ★★ **W2.5 IS REFERENCE ONLY AS OF 2026-09-03, AND THE FLEET IS 15 NODES.**
> This document is kept as the portable recipe, not as a plan of record.
>
> ★ **15 GMs means `floor(15 / 11) = 1` active quorum**, so the **multi-quorum
> behaviour this drill was built to exercise is not reachable at this size** —
> L>1 routing, concurrent-quorum load, and the reform-headroom arithmetic below
> all need N ≥ 22 and really N ≥ 33 (ODC-094). That is a consequence of the
> decision, not an oversight: at 15 the fleet is a **single-quorum functional
> environment**, which is what it is now for.
>
> ★ **The L=1 caveat comes with it:** ODC-094 records that at L=1 a retirement
> window is **total outage**, because no second quorum exists to carry it. On a
> reference fleet that is acceptable and expected; it would not be on anything
> load-bearing.
>
> The original 98 GM / 8 quorum / 4 caller topology is preserved below the line
> for when it is next needed.

*Written 2026-07-28 around the Step-0 chainparams commit (`db52acf`). This is the
operational doc for standing up the W2.5b scale-validation fleet. Register
context: KDD-079 §8 (the env-gated set this fleet exists to run), ODC-054,
KDD-080. The append-only session log is the untracked `PTX_LE_STANDUP.md` —
this doc is the portable recipe; the standup is the live record.*

---

## 1. The params are COMPILED, not configured — read this first

`Consensus::PTXFormationParams` is a consensus-params struct set per network in
`src/chainparams.cpp`. **There is no runtime flag, .env entry, or compose knob
for any of it.** Changing the fleet's formation shape means: edit chainparams →
rebuild binaries → rebuild the image → fresh chain. The W2.5b shape landed at
`db52acf` on **ptxbea** (the fleet's chain — the entrypoint runs `-ptxbea`, and
the GMAUTH carve-out `IsPTXBeaFleetAddr` is ptxbea-chain + `172.31.0.0/24`
subnet gated, net.cpp:219):

```
consensus.ptxFormation = {"ptxbea", 30, 1440, 80, 8, 200, 1, 40};
//                         name      B   R    bud L  retire grace rate
```

| Param | Value | Why |
|---|---|---|
| `nBoundaryInterval` (B) | 30 | V11 boundary cadence; must exceed the ceremony floor M≈27 blocks (drill-measured formation→connect) |
| `nRotationInterval` (R) | 1440 | KDD-045's key-compromise bound; capacity R/B = 48 rotation slots per interval |
| `nCeremonyBudget` | 80 | Guard 3 (KDD-079 §5): the ODC-050 stall-out must NOT track the cadence — a ~27-block ceremony under a 30-block boundary lives on this separation |
| `nSupportedQuorums` (L) | 8 | Guard 1's check target — the declared count. 48 ≥ 8 (hard) and 48 ≥ 16 (2× advisory margin, quiet) |
| `nRetireWindow` | 200 | KDD-074 idle arm. ★ ODC-052 sparsity bound: must exceed L × mean_roll_interval → **demand generation must sustain mean roll interval < 25 blocks** or sparsity reads as idleness and healthy quorums reform |
| `nReformGrace` | 1 | ★ REQUIRED > 0 at L > 1 — ODC-054's coupling check **hard-rejects the daemon at startup** otherwise (Guard 2 is safe only alongside the KDD-076 yield) |
| `nReformRateWindow` | 40 | KDD-074 limiter: one reform per 40 blocks, LRA-first — correlated-eligible sets drain one-per-window |

**Untouched:** main/test/ptxtest/regtest keep their L=1 shapes (the
defaults-preserve discipline; the W4f structural pin enforces the gate-tail
property on regtest/ptxbea only).

## 2. Startup validation — the config validates itself

`InitSanityCheck` runs `PTX_Formation_CheckParams` on the selected network's
params. The W2.5b shape passes by construction and is pinned at unit level by
`G4b_FleetShapeValidates` (real shipped params accepted; the same shape with
`nReformGrace=0` refused naming the param — ODC-054 on the real config) and
G1a's five-network sweep. A daemon built at `db52acf` was smoke-started on
`-ptxbea` with a fresh datadir: starts clean, no rejection. **If a future param
edit makes the daemon refuse to start with "PTX multi-quorum config requires
the forced-reform gate" or "cadence cannot serve the declared quorum count",
the guards are doing their job — fix the params, don't bypass the check.**

## 3. Image rebuild recipe

```
# 1. Build binaries at HEAD (canonical builder invocation, incremental):
docker run --rm -v /mnt/pve/Node14TB/hemis-ptx/src/hemisd:/build/hemisd \
  ptx-builder-local:latest bash -c "cd /build/hemisd/src && make -j\$(nproc) Hemisd Hemis-cli"
# 2. Place as docker-w2/binaries/Hemisd-ptx-w2 and Hemis-cli-ptx-w2 (copy, then md5sum both).
# 3. docker build -t hemis-ptx-w2:<commit> -f Dockerfile <docker-w2 dir>
#    NEVER tag over hemis-ptx-bea:* (the PoC image stays untouched).
# 4. ★ VERIFY MD5 of the binary INSIDE the built image against step 2
#    (the stale-container-sync trap, SG-1a lesson).
```

Autotools note: the tree's generated build system was rebased in-container on
2026-07-28 (libtool 2.4.7). If a build spontaneously re-runs configure/aclocal,
see the memory recipe: full ordered touch-defusal, never a blind re-autogen.

## 4. Topology — 15 GM / 1 quorum / 1 caller

- **Arithmetic:** 1 quorum × 11 = 11 seated + 4 spare pool. ★ The pool is
  deliberately thin: at this size the fleet is for function, not for reform
  headroom, and a reform draws fresh from those 4.
- **Network / ports:** as below, sized for the larger fleet and left alone —
  a 15-node fleet fits inside allocations made for 103 containers.
- **Fit:** 15 GMs at the 400 MiB cap is **6 GB**, comfortable against ~21 GB
  available RAM and 18 GB free disk. This is the size that fits without
  argument.

### The original 98 GM topology, kept for reference

- **Arithmetic:** 8 quorums × 11 = 88 seated + 10 spare pool (top-up-free
  reform headroom; KDD-080 — reform draws fresh from the pool, so the pool
  must stay ≥ 0 after 8 formations and ideally ≥ 11 for one reform cycle).
  4 callers (coordinator redundancy + demand generation) + 1 observer.
- **Network:** `172.31.0.0/24` (the GMAUTH carve-out subnet — 103 containers
  fit; do not move off this subnet without touching `IsPTXBeaFleetAddr`).
- **Ports:** host RPC from `--port-base 31000` upward (31000..31102).
- **Host:** node1 measured at 78 containers / 31 GB of 94 GB **root disk** during
  KDD-079 sizing. ★ **Re-measured 2026-09-03: root disk is 109 GB with 18 GB
  free, and RAM is 39 GB with ~21 GB available.** The disk figure was sound; the
  sizing was **silent about RAM**, which is the binding constraint — at the
  400 MiB container cap, 88 GMs need 34.4 GB and do not fit. Keep
  `-nodebuglogfile` (the disk-hygiene rule).

## 5. Launch recipe

```
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
python3 -u run_bootstrap.py --n 15 --callers 1 \
  --reg-out /mnt/pve/Node14TB/hemis-ptx/w2-fleet/registration-N98.json
```

(`run_bootstrap.py` wraps `gen_fleet.py` → generated compose at
`docker-w2/docker-compose.generated.yml`, env-subst credentials host-side —
the GMAUTH pattern; registration JSON is regenerated, NOT scaled from N60.)

★ **Caveats before first launch:**
- `gen_fleet.py --n` help says "22 floor, **60 ceiling tested**" — 98 exceeds
  the tested ceiling. First run should be a dry `gen_fleet.py` output review
  (IP/port collisions past .70, compose size) before `up`.
- `--callers 4`: verify gen_fleet's caller arm actually parameterizes beyond 1
  (default 1; the flag exists, its N>2 path is untested).
- Bank/restore: `restore_fleet.sh` restores the BANKED compose image tag —
  re-point at the new image after any restore (standing trap).
- BUG-019: relock wallets after every gm01 start (`(a)`-arm harness step until
  `(d)` lands).
- A fresh chain is REQUIRED: the new params change formation behavior from
  genesis; do not resume the banked N22 chain under the new image.

## 6. What this fleet exists to validate (the KDD-079 §8 env-gated set)

1. Staggering capacity at scale (48 slots / 8 quorums, contended boundaries).
2. ★ Guard 2 under GENUINE competition (the fairness floor's real validation —
   its unit RED forced the loss artificially at L=2; KDD-079's
   KDD-045-preservation claim is unit-proven-not-scale-validated until this).
3. Correlated departures under scale + demand (the limiter drain,
   IH_CorrelatedDrain's producer proof exercised live).
4. ODC-052's fix under demand (routing distribution; the sparsity bound above).

Demand generation is the work: sustained rolls at mean interval < 25 blocks
(the §1 sparsity bound), from ≥ 2 coordinators (the SG-3
coordinator-independence pattern).
