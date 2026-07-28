# PTX Roadmap — to first testnet

*Tracked, portable. This file is the fix for the "roadmap lived only in the untracked standup" finding (KDD-065 class): the program plan must survive a clone. Status labels below are source-checked; where a claim rests only on live fleet state or the untracked `PTX_LE_STANDUP.md`, that is called out.*

*Last reconciled: 2026-07-28 (the W2.3/W2.4/W2.5a/W2.6 reconciliation — the 2026-07-23 "Remaining (6)" list predated four shipped arcs and one register decision; this pass moves them to Completed, re-scopes Remaining around the W2.5b fleet, and marks the 2026-07-23 fleet snapshot historical. Prior reconciliation note kept below.)*

*Previous: 2026-07-23. Prior working roadmap ("17 completed, 6 remaining") corrected here — the count was wrong (7 were listed), three completed labels were stale, two "remaining" items had already landed, and the signing repoint had no line at all.*

---

## Completed (26)

1. Phase 0 — randomness validation (NIST suite, final pass)
2. Phase 1 core — threshold BLS beacon on the PoC (trusted-dealer, `ptx_roll` API)
3. Lottery live on the PoC
4. PoSe + tooling (explorer, client setup)
5. 11-GM PoC fleet (docker-bea, live demo)
6. DKG design phase (design doc, implementation plan, KDD register)
7. W1.2 — GJKR ceremony crypto core (in-process)
8. **W1.3 — PTXDKG validation: V1–V11 + block-level one-per-block** *(the old "V1–V8" understated it; the tree ships V9 duplicate-formation `specialtx_validation.cpp:710`, V10 committed-member containment `:759` (KDD-061), V11 formation-boundary `:697` (SG-1b), and the one-PTXDKG-per-block rule `:1092`)*
9. W2.0a — W2 fleet infrastructure (generator, harness, bank/restore)
10. W2.0b — transport
11. W2.1 — quorum registry/store *(CPTXQuorumStore::ProcessBlock persists group_pk/share_index/in_qual, `ptx_quorum_store.cpp:98–119`)*
12. **W2.2 — formation model locked + KDD-065 GMAUTH member-connection wiring (built, commit 7391617)** *(more than a "design arc" — the member-connection pseudo-type and formation-thread hooks are committed)*
13. SG-0 — fleet prerequisites (GMAUTH live, gm44 closed)
14. SG-1a — formation caller (selection, KDD-040 exclusion)
15. SG-1b — boundary/schedule (cadence N set, V11 boundary-validation, first schedule-valid quorum formed live)
16. SG-1c — session start + reorg re-arm (acting trigger, re-arm proven, POS-B)
17. **Closes SG-1 — BUG-019(d) fixed; BUG-020 reclassified as a dev-net-only artifact** *(not "fork-seal fixed": BUG-020 was downgraded and reclassified as a symptom of an already-fatal fork; the durable fix was BUG-019(d). Fleet stable two-producer.)*
18. **★ Signing repoint — the beacon now consumes DKG output instead of the trusted dealer** (commits **6bd17e8** index-space reconciliation / repoint, **6eca8ba** roll observability, **0101a44** quorum-scoped threshold; ODC-036, KDD-066/067/068; closed 2026-07-23)
19. **★ KDD-069 — trusted dealer RETIRED; DKG signing is the only path** (commits **3c616d9** tests off-dealer, **8a2200e** removal of `PTX_BLS_Init`/`gm_bls_keyset`/`PTX_AssignQuorum`/etc., **51894b4** register; 2026-07-24). Item 18 repointed the beacon onto DKG output; this *removed* the dealer stack entirely. Capability loss recorded: a quorumless fleet can no longer be rolled at all (must form a DKG quorum first). No consensus surface (coordinator-side only).
20. **★ KDD-070 — GM sk-share slot mechanism, P1–P5 built (unit-verified)** (commits **51fd2dd** P1 keyed store, **068ec4a** P2 evoDb persistence + reconcile + wipe, **e2bfaed** P3 PENDING/promotion/TTL, **9ecc76c** P4 SUPERSEDED retention/depth-discard/undo revert, **a86a549** P5 structural §1 check + close-out; ~37 tests, 312 test_ptx green; 2026-07-24). ★ **Mechanism COMPLETE but with ZERO production callers** — `PTX_BLS_Promote`, `PTX_BLS_ExpirePending`, `PTX_BLS_DiscardSuperseded`, `PTX_BLS_UndoPromote`, and `StoreSkShare(…, PENDING)` all have no call site. Unit-verified only; fleet verification bound to W2.4. Slot-side undo only (record-side revert owed to KDD-063 / W2.4). "Built" here means the mechanism exists and is tested, NOT that it runs on the fleet — see Remaining #1 (W2.3 writes the call sites).

21. **★ W2.3 — rotation + handover-at-accept, SHIPPED** (the KDD-072 P-a/P-b arc: versioned payload + signed predecessor, V12 rotation validation + the store-guard third site, KDD-070's call sites wired, KDD-063 superseded-at-connect swap; rotation drill-proven on the fleet). The 2026-07-23 "Remaining #2" in full.
22. **★ W2.4 — retirement/reform, SHIPPED** (KDD-074 idle arm + KDD-075 yield + KDD-076 forced-reform, the W4a–W4L packages; producer `MaybeReformAtBoundary` live, drills run; the lineage clock + age anchor findings landed en route). ★ Note the scope shift recorded at KDD-080: W2.4's "disband / top-up" title resolved to retirement/reform — disband was decided OUT (see item 26), top-up subsumed (reform's fresh draw).
23. **SG-5 — abort/exclusion, closed as scoped by KDD-077** (the threat model — PTX's first written adversary model; complaint-suppression accepted as a reliability bound → ODC-051; ODC-050 stall-out as a liveness bound; the Byzantine arc explicitly NOT built).
24. **★ W2.5a — multi-quorum core, COMPLETE 2026-07-28** (KDD-079: §7.4 routing distribution `7a9ccdc`, the params decouple P1 `a374a25` + Guard 3 + the fourth-conflation fix, Guard 1 `2ddd2c5` + `nSupportedQuorums`, Guard 2 fairness floor `d9c2afd`; KDD-040 relaxation `cf162aa`/`3697d5d` — multi-quorum posture, one-per-GM KEPT; interaction hardening `fd9a041` — the ODC-054 Guard-2×gate coupling, sixth seam; register amendments `5daa9e7`/`92064d3`). Suite 404.
25. **W2.5b Step 0 — the fleet chainparams** (`db52acf`: ptxbea = B=30/R=1440/budget=80/L=8, gate live, guard-validated at startup; fleet recipe `doc/ptx/W25B_FLEET_CONFIG.md`, `0b1a8a6`).
26. **★ W2.6 — disband DECIDED C, a register act not a build** (KDD-080 `420f250`: retirement suffices — under fail-safe, failure is only ever visible as absence; fork-B permanently deferred against KDD-077 §3; the §3(b) provable-resolution precondition registered; ODC-034 closed; KDD-071 resolved; DISBANDED=3 reserved-unused permanently).

> **Numbering note — SG-3 and SG-4 as originally named were already satisfied before 2026-07-23.** "SG-3 ceremony over the wire (first live ceremony)" and "SG-4 ClosePhase5 → persist (first complete formation)" describe work that landed under the SG-1/SG-2 arc: **ceremonies ran at h960 (→ quorum 57e7c7b4) and h1040 (→ quorum fc8e0f0d)**, and both quorums are **persisted in CPTXQuorumRecord** with group_pk_bytes, share_index and in_qual. What actually closed on **2026-07-23** was *unnamed* work — the signing repoint — now recorded as item 18. The old SG-3/SG-4 labels are retired **on the record, not silently**: they were satisfied earlier, and the 2026-07-23 milestone was mislabelled as them.

---

## Remaining (2) — re-scoped 2026-07-28 around the W2.5b fleet

Sizing remains relative to KDD-070 = 1.0. The 2026-07-23 list's six items resolved: W2.4 shipped (Completed #22), W2.3 shipped (#21), W2.5's code half complete (#24, W2.5a), SG-5 closed as scoped (#23), KDD-071 resolved (KDD-080 §6 — rotation-as-heartbeat + idle retirement IS the signal), leaving SG-2b and the W2.5b fleet run.

1. **★ W2.5b — the env-gated scale validation (THE fleet run)** — *sizing: orchestration + demand-generation work, not source work* (KDD-079 §8). The set: staggering capacity at 48 slots / 8 quorums; ★ **Guard 2 under genuine competition** (the load-bearing one — KDD-079's KDD-045-preservation claim is unit-proven-not-scale-validated until this); correlated departures under scale + demand; ODC-052's routing fix under demand (mean roll interval < 25 blocks or sparsity reads as idleness — the nRetireWindow=200 bound). Recipe: `doc/ptx/W25B_FLEET_CONFIG.md` (98 GM / 8 quorum / 4 caller; params COMPILED at `db52acf`; fresh chain required). Gates: image rebuild + gen_fleet dry-review past its 60-GM tested ceiling.
2. **SG-2b — over-the-wire convergence, CLOSE** — *size ≈ 0.05, unchanged.* Full 11/11 over-the-wire convergence still NOT confirmed in the portable record (the earlier "converging 11/11" claim remains chat-only; the two historical quorums formed with QUAL exclusions). ★ May fold into the W2.5b fleet's first formations — 8 fresh ceremonies at scale will either produce the clean full-convergence record or surface why not; owes only the *portable capture*.

### Dependency graph

```
W2.5b fleet stand-up (image @ >= db52acf, fresh ptxbea chain)
  ├─> the KDD-079 §8 env-gated set (Guard 2 real validation, correlated departures, ODC-052 under demand)
  ├─> SG-2b portable close (8 fresh ceremonies — the convergence record)
  ├─> KDD-066-successor tiebreak + multi-quorum signing exercised at L=8 (§7.4 routing live)
  └─> demo scoping (AFTER this roadmap + fleet — not before)

Everything code-side for W2.5b is DONE (W2.5a #24, Step-0 #25). The fleet is demand-generation and orchestration.
```

**Pre-fleet checklist (from the config doc):** binaries at HEAD ≥ `db52acf` → md5-verified image → gen_fleet dry review (--n 98 exceeds the 60 tested ceiling; --callers N>2 path untested) → fresh chain (never resume the banked N22 chain under new params) → BUG-019 relock discipline → demand ≥ 1 roll/25 blocks sustained.

## Fleet state — snapshot 2026-07-23 ★ HISTORICAL (marked 2026-07-28)

> ★ **This snapshot is HISTORICAL.** The N22 fleet it describes is SG-era (pre-W2.5b params) and its chain will be RETIRED by the W2.5b stand-up (fresh chain required under `db52acf`'s ptxbea shape — never resume it under the new image). The live quick reference for DKG work is the **W2.5b section at the END of the untracked `PTX_LE_STANDUP.md`**; the portable fleet recipe is `doc/ptx/W25B_FLEET_CONFIG.md`. Kept below unedited for the record.

> ⚠ **Live state is NOT derivable from the repo.** Quorum hashes, formation heights, in_qual counts and deployed binaries are runtime facts, not commits. **This section must be updated by hand or it goes stale silently.** Treat everything here as a dated snapshot, not a live query.
>
> ★ **As of 2026-07-24 this snapshot has NOT been re-verified against the fleet.** HEAD has since advanced to **a86a549** — KDD-069 (dealer retirement) and KDD-070 P1–P5 (slot mechanism) landed *after* this snapshot — so the deployed tags below are now further behind HEAD than "2026-07-23" implies. ★ **Sharp point:** KDD-070's share **persistence exists in HEAD but is NOT on the deployed `8719b7c` GMs**, so the ODC-038 / ODC-035 GM-restart hazard **still holds on the fleet** even though the code no longer has the process-lifetime defect. Do not read "persistence is built" as "the GMs are safe to restart" — the call site is also still unwritten (W2.3).

**ACTIVE quorums (CPTXQuorumRecord):**

| quorum_hash | formation height | in_qual | notes |
|---|---|---|---|
| `fc8e0f0d…` | h1040 | **9** in_qual (score-order x's {2,3,4,5,6,7,9,10,11}) | signing quorum for the SG-3 repoint; PASS-capable (t=6) |
| `57e7c7b4…` | h960 | **6** in_qual | at threshold (t=6); never selected under KDD-066 while fc8e0f0d (h1040) is ACTIVE |

Both are sub-11 (QUAL exclusions), both ACTIVE, both above/at t=6. KDD-040 single-quorum-per-GM + these two ACTIVE = **pool saturated** (ODC-034): no new formation until W2.4 disband.

**Binary divergence (★ ODC-038 — DO NOT DEPLOY to GMs):**

- Coordinators (172.31.0.33 / .34): `hemis-ptx-w2:0101a44-dbg`
- All 22 GMs: `hemis-ptx-w2:8719b7c-dbg`

Intentional: the SG-3 repoint (0101a44) is coordinator-side only. **Hazard:** a GM restart clears its in-memory sk_share (ODC-035, process-lifetime only); both quorums are ACTIVE with no disband path (ODC-034), so a fleet-wide GM deploy/restart **destroys both quorums permanently** — recovery is a bank-restore to a pre-h960 snapshot. See **DKG_DESIGN_DOC_v1.md → ODC-038** (register) for the full hazard and safe-deploy conditions.

---

## Pre-testnet blockers (none currently tracked on any status list)

- **ODC-035 — silent quorum degradation — ★ RESOLVED IN CODE 2026-07-28-status: persistence half WIRED (W2.3 shipped KDD-070's call sites, Completed #21), liveness half RESOLVED (KDD-071 → KDD-080 §6: rotation-as-heartbeat + idle retirement IS the signal).** Residual is DEPLOYMENT: the N22 fleet's `8719b7c` GM images predate the wiring — the W2.5b fresh fleet at ≥ `db52acf` retires this blocker entirely. Original 2026-07-24 note kept: ★ Corrected 2026-07-24: the persistence half of the fix is DONE in code — KDD-070 P2 added evoDb persistence (`PTX_BLS_PersistShare`/`LoadShares`), replacing the old `g_ptx_my_bls_sk_bytes` global with the keyed `g_ptx_my_shares` map. But its call site is UNWRITTEN (first consumer W2.3) and the deployed `8719b7c` GMs predate it, so **on the fleet a GM restart still silently drops the member** (no on-chain/RPC signal). Remaining before testnet: (a) wire KDD-070's store/load call sites (W2.3) and redeploy; (b) the **liveness-signal** half — member offline, share intact but process down — still open → **KDD-071** (may fold into rotation-as-heartbeat).
- **V11 activation-height gate.** The formation-boundary check (V11) is deliberately un-gated on the resettable fleet (`specialtx_validation.cpp:689–690`). If the formation cadence ever changes on a chain with history, historical PTXDKGs become retroactively invalid and a node syncing from genesis rejects blocks the network accepted — **split-on-resync**. A height-gate is required before any chain with history. ★ Cross-ref **ODC-040** (fleet homogeneity coverage limit — the fleet has no chain with real history, so this cannot be exercised here; note the cause is temporal, not homogeneity).
- **persist-t (KDD-048, Package-2 prerequisite).** V8 hardcodes `t=6` (`specialtx_validation.cpp:639`) and V5 hardcodes `n=11`; neither is on the wire or the record. The KDD-048 t=6→7 upgrade would **silently split** validators unless t is persisted explicitly (majority(11)=6 cannot yield 7, so it can't be derived from formed_size). Owed before the t-upgrade.
- **ODC-037 — banlist residue (+ KDD-070 share-wipe).** `banlist.dat` survives restart/wipe/restore; a restored fleet silently excludes a wiped-and-rejoining node (24h TTL, presents as broken-pipe). Owed in `restore_fleet.sh`: a banlist-clear step — **and, ★ new with KDD-070, a share-wipe step** (`-ptxwipeshares` / `PTX_BLS_WipeShares`). KDD-070 P2 persists sk-shares to evoDb, so shares now SURVIVE the restart that used to clear them; a bank-restore must explicitly wipe them or stale shares block fresh formation under §C1. `restore_fleet.sh` now owes **both** steps.
- **★ KDD-064 — PTX has NO maturity gate of its own** (ALLOCATED-THEN-ABANDONED; the intended PTX-specific gate never landed). **Coupling to record explicitly:** the deep-fork "mainnet-moot" verdict rests **entirely on inherited code** — the `UPGRADE_V3_4`-gated stake-modifier-V2 depth check (`consensus/params.h:264–267`, `contextHeight − utxoFromBlockHeight >= nStakeMinDepth`), `nStakeMinDepth = 300` mainnet (chainparams.cpp), and `-maxreorg` (validation.cpp:2958/3374). Because 300 > maxreorg=100, a fork-created coin can't re-stake until the fork is already 3× past maxreorg and unhealable for independent reasons — hence mainnet-moot. **This does NOT depend on the never-built KDD-064 gate, so the verdict stands.** But it *does* depend on that inherited check: **if `params.h:264–267`, `nStakeMinDepth`, or the maxreorg relationship ever moves, the deep-fork forensic conclusion must be re-derived** — there is no PTX-specific gate backstopping it.
- **Narrower `ptx_bls.h` (KDD-070 P5 follow-up, owed).** The eight share-store mutators are declared in the public `ptx_bls.h`, which `rpc/ptx.cpp` includes — so only the P5 grep-check (`P5_ShareStore_NoRpcReachableMutator`) stops an rpc call reaching one. Splitting the header so rpc includes only the sign/recover surface (never the mutators) makes an rpc write a **compile error**, strictly stronger than the grep. Recorded in the KDD-070 P5 close-out as owed; not done in P5.
- **v4 test-suite disposition (KDD-069) — ★ the disabled-suites CONCERN closed (SG-5 recon: the "disabled suites" reading was a `--run_test` filter artifact, not silently-skipped coverage); the DISPOSITION of the dealer-era file itself remains owed.** `ptx_test_suite_v4.py` (120 dealer-era tests) was updated to a `DEALERLESS` probe that runs **T01 live and auto-skips T02–T119**; the full disposition of those 119 is OWED — rewrite off-dealer or retire. Syntax-verified only, never runtime-validated (no fleet run authorised).
- **BUG-015 — coinstake-split burn silently skipped (economic-correctness, wallet-side; NOT a split risk).** ★ Severity confirmed from source 2026-07-24. The 10% subsidy burn is applied **only** in the `stakerOuts == 2` branch of `SubtractGmPaymentFromCoinstake` (`gamemaster-payments.cpp:362–364`); the `stakerOuts > 2` branch (`:365–375`) subtracts only the GM payment — **no burn**. That function's sole caller is `FillBlockPayee` (`:410`), **miner-side** block assembly. Consensus does NOT enforce the burn: `ConnectBlock` sets `nExpectedMint = GetBlockValue()` (full value, no burn term — `validation.cpp:1670`) and `IsBlockValueValid` is a **ceiling** — `return nMinted <= nExpectedValue` (`gamemaster-payments.cpp:226`). A split block that skips the burn mints *more* but stays UNDER the full-value ceiling, so **every validator accepts it deterministically — no fork.** The defect: a staker who splits its coinstake (`stakerOuts > 2`) silently keeps the ~10% (≈26.75 HMS/block) the policy intends to burn. **Lower tier** than the split-risk blockers (V11, persist-t) — economic, not consensus-divergent — but still pre-testnet. **Why the fleet cannot exercise it:** all 22 GMs run one `stakeSplitThreshold` and gm01's UTXOs sit above it, so every coinstake had `coinstake_vouts = 2` — all 3 Section B samples burn-on (the 267.5M burn-off net was never produced). **Owed task:** set `stakeSplitThreshold` below gm01's UTXO values, run a fleet, produce a `stakerOuts > 2` block, and decide the policy the check surfaces — is a split staker escaping the burn intended, or a hole to close? (Commit `a86ee16`, BUG-015 row in the register.)

---

*Cross-reference: the decision register is `doc/ptx/DKG_DESIGN_DOC_v1.md` (KDD/ODC entries). This roadmap tracks program status; the register tracks decisions. Live fleet state (above) is portable only as a hand-maintained snapshot.*
