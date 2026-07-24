# PTX Roadmap — to first testnet

*Tracked, portable. This file is the fix for the "roadmap lived only in the untracked standup" finding (KDD-065 class): the program plan must survive a clone. Status labels below are source-checked; where a claim rests only on live fleet state or the untracked `PTX_LE_STANDUP.md`, that is called out.*

*Last reconciled: 2026-07-23. Prior working roadmap ("17 completed, 6 remaining") corrected here — the count was wrong (7 were listed), three completed labels were stale, two "remaining" items had already landed, and the signing repoint had no line at all.*

---

## Completed (20)

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

> **Numbering note — SG-3 and SG-4 as originally named were already satisfied before 2026-07-23.** "SG-3 ceremony over the wire (first live ceremony)" and "SG-4 ClosePhase5 → persist (first complete formation)" describe work that landed under the SG-1/SG-2 arc: **ceremonies ran at h960 (→ quorum 57e7c7b4) and h1040 (→ quorum fc8e0f0d)**, and both quorums are **persisted in CPTXQuorumRecord** with group_pk_bytes, share_index and in_qual. What actually closed on **2026-07-23** was *unnamed* work — the signing repoint — now recorded as item 18. The old SG-3/SG-4 labels are retired **on the record, not silently**: they were satisfied earlier, and the 2026-07-23 milestone was mislabelled as them.

---

## Remaining (6) — dependency-ordered, sized

Sizing is **relative to KDD-070 = 1.0** (the just-completed slot mechanism, ~37 tests across five packages). KDD-070 has DROPPED OUT of "remaining" — it is done (Completed #20) — and is **no longer a gate on W2.3**; instead its consumer W2.3 absorbs the cost of wiring its five unwritten call sites. Prior count was 5; adding **KDD-071** (the ODC-035 liveness half, which may fold into rotation-as-heartbeat) makes it 6.

1. **W2.4 — disband / top-up ★ KEYSTONE** — *size ≈ 0.8–1.0.* ODC-034: the fleet is quorum-saturated (22 GMs × 2 ACTIVE quorums exhausts the KDD-040 single-quorum-per-GM pool), so **no new formation is possible until a disband path exists**. W2.4 gates the pool-dependent work below (SG-5, W2.5, W2.3's *live* arm). It can be *built* on the current saturated fleet (it does not need a free pool to be written). Also owes the **record-side undo revert** (the half KDD-070 P4 did NOT build — successor de-activated, predecessor SUPERSEDED→ACTIVE in `CPTXQuorumStore::UndoBlock`; shared with KDD-063).
2. **W2.3 — rotation + handover-at-accept (KDD-063); CONSUMES KDD-070** — *size ≈ 1.5–2.0.* ★ **Re-scoped:** KDD-070 built the slot mechanism but left ZERO production callers. W2.3 is where it starts to fire — write the **five unwritten call sites** (`StoreSkShare(…,PENDING)` at ceremony FINALIZE; `PTX_BLS_Promote` on successor block-connect; `PTX_BLS_UndoPromote` + `PTX_BLS_DiscardSuperseded` on disconnect/tip-advance; `PTX_BLS_ExpirePending` on the reconcile/connect sweep) **plus W2.3's own rotation-validation consensus arm and handover-at-accept**. Two arms: the **wiring + consensus arm is buildable NOW** (like W2.4, it is code, not pool-dependent); the **live-rotation exercise needs a free pool → gated by W2.4**. Largest remaining item because it is mechanism-wiring + new consensus + the fleet exercise.
3. **W2.5 — multi-quorum (L/N dial-up)** — *size ≈ 1.5–2.5.* *Gated by W2.4* (needs to form additional quorums). Also unblocks the KDD-066 tiebreak (needs a same-formation_height pair; 960 ≠ 1040 today so the tiebreak branch has never fired) and lets 57e7c7b4 ever sign (KDD-066 currently always picks fc8e0f0d, h1040 > h960). Relaxes the KDD-040 one-quorum-per-GM bound (the per-quorum invariants in KDD-070 already hold at any L).
4. **SG-5 — abort / exclusion paths** — *size ≈ 0.25.* *Gated by W2.4* (exercising exclusion needs reformation, which needs a free pool).
5. **SG-2b — over-the-wire convergence, CLOSE** — *size ≈ 0.05.* SG-2a (the phase driver) is **built and wired** (`ptx_ceremony_driver.h` `PTX_Ceremony_Step`; `PTXCeremonyDriverState dstate` in the formation thread, `ptx_formation.cpp:188`). SG-2b-0's first run **aborted at CP-3** (zero-wire relay); KDD-065 (7391617) landed to unblock it. **Full 11/11 over-the-wire convergence is NOT confirmed in the portable record.** ⚠ A claimed "SG-2b-0 converging 11/11 on quorums b4eda9f8…d2b079fe" is **chat-only** — those hashes appear nowhere in the repo or the standup; the two quorums that *do* exist (fc8e0f0d, 57e7c7b4) formed with QUAL exclusions (9 and 6 in_qual, not 11/11). Just owes a clean full-convergence run captured in a portable record.
6. **KDD-071 — share-liveness signal** — *size ≈ 0–0.5.* The half of ODC-035 that KDD-070 did NOT close: a member genuinely offline (share intact but process down) with no on-chain/RPC signal. **May never be needed separately** — rotation-as-heartbeat (KDD-045 periodic same-set re-DKG) makes a missed rotation an on-chain-observable "couldn't muster the member" signal, which is most of what a disband trigger needs. Held here as a sized placeholder pending recon.

### Dependency graph

```
                        ┌─> SG-5   (abort/exclusion — needs reform)
                        ├─> W2.3 (live arm)  (rotation exercise — needs pool capacity)
W2.4 (disband/top-up) ──┼─> W2.5   (multi-quorum L/N — needs to form more)
   [KEYSTONE]           ├─> 57e7c7b4 ever signing (fc8e0f0d must yield, or multi-quorum)
                        └─> KDD-066 tiebreak ever firing (needs same-fh pair)

W2.3 (wiring + consensus arm) ── buildable NOW (writes KDD-070's 5 call sites; not pool-dependent)
KDD-070 slot mechanism ── DONE 2026-07-24 (Completed #20; unit-verified, zero callers)
SG-2b driver ── built; full convergence unverified in portable record
Signing repoint / dealer retirement ── DONE 2026-07-23 / 2026-07-24 (items 18 / 19)
```

**Can proceed on the current saturated fleet (all code, no free pool needed):** signing with the existing quorums, **building W2.4**, and **W2.3's wiring + consensus arm** (writing KDD-070's call sites + rotation validation). Everything *pool-dependent* — SG-5, W2.5, and W2.3's live-rotation exercise — is blocked behind W2.4.

---

## Fleet state — snapshot 2026-07-23 (NOT re-verified since)

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

- **ODC-035 — silent quorum degradation (persistence half now BUILT, unwired).** ★ Corrected 2026-07-24: the persistence half of the fix is DONE in code — KDD-070 P2 added evoDb persistence (`PTX_BLS_PersistShare`/`LoadShares`), replacing the old `g_ptx_my_bls_sk_bytes` global with the keyed `g_ptx_my_shares` map. But its call site is UNWRITTEN (first consumer W2.3) and the deployed `8719b7c` GMs predate it, so **on the fleet a GM restart still silently drops the member** (no on-chain/RPC signal). Remaining before testnet: (a) wire KDD-070's store/load call sites (W2.3) and redeploy; (b) the **liveness-signal** half — member offline, share intact but process down — still open → **KDD-071** (may fold into rotation-as-heartbeat).
- **V11 activation-height gate.** The formation-boundary check (V11) is deliberately un-gated on the resettable fleet (`specialtx_validation.cpp:689–690`). If the formation cadence ever changes on a chain with history, historical PTXDKGs become retroactively invalid and a node syncing from genesis rejects blocks the network accepted — **split-on-resync**. A height-gate is required before any chain with history.
- **persist-t (KDD-048, Package-2 prerequisite).** V8 hardcodes `t=6` (`specialtx_validation.cpp:639`) and V5 hardcodes `n=11`; neither is on the wire or the record. The KDD-048 t=6→7 upgrade would **silently split** validators unless t is persisted explicitly (majority(11)=6 cannot yield 7, so it can't be derived from formed_size). Owed before the t-upgrade.
- **ODC-037 — banlist residue (+ KDD-070 share-wipe).** `banlist.dat` survives restart/wipe/restore; a restored fleet silently excludes a wiped-and-rejoining node (24h TTL, presents as broken-pipe). Owed in `restore_fleet.sh`: a banlist-clear step — **and, ★ new with KDD-070, a share-wipe step** (`-ptxwipeshares` / `PTX_BLS_WipeShares`). KDD-070 P2 persists sk-shares to evoDb, so shares now SURVIVE the restart that used to clear them; a bank-restore must explicitly wipe them or stale shares block fresh formation under §C1. `restore_fleet.sh` now owes **both** steps.
- **★ KDD-064 — PTX has NO maturity gate of its own** (ALLOCATED-THEN-ABANDONED; the intended PTX-specific gate never landed). **Coupling to record explicitly:** the deep-fork "mainnet-moot" verdict rests **entirely on inherited code** — the `UPGRADE_V3_4`-gated stake-modifier-V2 depth check (`consensus/params.h:264–267`, `contextHeight − utxoFromBlockHeight >= nStakeMinDepth`), `nStakeMinDepth = 300` mainnet (chainparams.cpp), and `-maxreorg` (validation.cpp:2958/3374). Because 300 > maxreorg=100, a fork-created coin can't re-stake until the fork is already 3× past maxreorg and unhealable for independent reasons — hence mainnet-moot. **This does NOT depend on the never-built KDD-064 gate, so the verdict stands.** But it *does* depend on that inherited check: **if `params.h:264–267`, `nStakeMinDepth`, or the maxreorg relationship ever moves, the deep-fork forensic conclusion must be re-derived** — there is no PTX-specific gate backstopping it.
- **Narrower `ptx_bls.h` (KDD-070 P5 follow-up, owed).** The eight share-store mutators are declared in the public `ptx_bls.h`, which `rpc/ptx.cpp` includes — so only the P5 grep-check (`P5_ShareStore_NoRpcReachableMutator`) stops an rpc call reaching one. Splitting the header so rpc includes only the sign/recover surface (never the mutators) makes an rpc write a **compile error**, strictly stronger than the grep. Recorded in the KDD-070 P5 close-out as owed; not done in P5.
- **v4 test-suite disposition (KDD-069, owed).** `ptx_test_suite_v4.py` (120 dealer-era tests) was updated to a `DEALERLESS` probe that runs **T01 live and auto-skips T02–T119**; the full disposition of those 119 is OWED — rewrite off-dealer or retire. Syntax-verified only, never runtime-validated (no fleet run authorised).

---

*Cross-reference: the decision register is `doc/ptx/DKG_DESIGN_DOC_v1.md` (KDD/ODC entries). This roadmap tracks program status; the register tracks decisions. Live fleet state (above) is portable only as a hand-maintained snapshot.*
