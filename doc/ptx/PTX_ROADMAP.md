# PTX Roadmap — to first testnet

*Tracked, portable. This file is the fix for the "roadmap lived only in the untracked standup" finding (KDD-065 class): the program plan must survive a clone. Status labels below are source-checked; where a claim rests only on live fleet state or the untracked `PTX_LE_STANDUP.md`, that is called out.*

*Last reconciled: 2026-07-23. Prior working roadmap ("17 completed, 6 remaining") corrected here — the count was wrong (7 were listed), three completed labels were stale, two "remaining" items had already landed, and the signing repoint had no line at all.*

---

## Completed (18)

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

> **Numbering note — SG-3 and SG-4 as originally named were already satisfied before 2026-07-23.** "SG-3 ceremony over the wire (first live ceremony)" and "SG-4 ClosePhase5 → persist (first complete formation)" describe work that landed under the SG-1/SG-2 arc: **ceremonies ran at h960 (→ quorum 57e7c7b4) and h1040 (→ quorum fc8e0f0d)**, and both quorums are **persisted in CPTXQuorumRecord** with group_pk_bytes, share_index and in_qual. What actually closed on **2026-07-23** was *unnamed* work — the signing repoint — now recorded as item 18. The old SG-3/SG-4 labels are retired **on the record, not silently**: they were satisfied earlier, and the 2026-07-23 milestone was mislabelled as them.

---

## Remaining (5) — dependency-ordered

The header count "6 remaining" was wrong (7 were listed). After moving the two already-landed items (old SG-3, old SG-4) into Completed, the true remaining count is **5**.

1. **W2.4 — disband / top-up ★ KEYSTONE.** ODC-034: the fleet is quorum-saturated (22 GMs × 2 ACTIVE quorums exhausts the KDD-040 single-quorum-per-GM pool), so **no new formation is possible until a disband path exists**. W2.4 gates almost everything below it. It is the one remaining item that can be *built* on the current saturated fleet (it does not need a free pool to be written). It belongs first, not second-to-last.
2. **SG-2b — over-the-wire ceremony convergence (phase driver, live).** SG-2a (the phase driver) is **built and wired** (`ptx_ceremony_driver.h` `PTX_Ceremony_Step`; `PTXCeremonyDriverState dstate` in the formation thread, `ptx_formation.cpp:188`). SG-2b-0's first run **aborted at CP-3** (zero-wire relay); KDD-065 (7391617) landed to unblock it. **Full 11/11 over-the-wire convergence is NOT confirmed in the portable record.** ⚠ A claimed "SG-2b-0 converging 11/11 on quorums b4eda9f8…d2b079fe" is **chat-only** — those hashes appear nowhere in the repo or the standup and cannot be verified; the two quorums that *do* exist (fc8e0f0d, 57e7c7b4) formed with **QUAL exclusions (9 and 6 in_qual, not 11/11)**. Status: driver done, CP-3 unblocked, clean full-convergence run **unverified** — remains open until a portable record shows it.
3. **SG-5 — abort / exclusion paths.** *Gated by W2.4* (exercising exclusion needs reformation, which needs a free pool).
4. **W2.3 — rotation + handover-at-accept (KDD-063).** *Gated by W2.4* (rotation reforms into new quorums → needs pool capacity).
5. **W2.5 — multi-quorum (L/N dial-up).** *Gated by W2.4* (needs to form additional quorums). Also unblocks the KDD-066 tiebreak (needs a same-formation_height pair; 960 ≠ 1040 today so the tiebreak branch has never fired) and lets 57e7c7b4 ever sign (KDD-066 currently always picks fc8e0f0d, h1040 > h960).

### Dependency graph

```
                        ┌─> SG-5   (abort/exclusion — needs reform)
                        ├─> W2.3   (rotation/handover — needs pool capacity)
W2.4 (disband/top-up) ──┼─> W2.5   (multi-quorum L/N — needs to form more)
   [KEYSTONE]           ├─> 57e7c7b4 ever signing (fc8e0f0d must yield, or multi-quorum)
                        └─> KDD-066 tiebreak ever firing (needs same-fh pair)

SG-2b driver ── built; full convergence unverified in portable record
Signing repoint ── DONE 2026-07-23 (item 18)
```

**Can proceed on the current saturated fleet:** only signing with the existing quorums, and **building W2.4 itself**. Everything else remaining is blocked behind W2.4.

---

## Fleet state — snapshot 2026-07-23

> ⚠ **Live state is NOT derivable from the repo.** Quorum hashes, formation heights, in_qual counts and deployed binaries are runtime facts, not commits. **This section must be updated by hand or it goes stale silently.** Treat everything here as a dated snapshot, not a live query.

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

- **ODC-035 — silent quorum degradation.** Share material is process-lifetime only (`g_ptx_my_bls_sk_bytes`, no persistence). A GM restart silently drops that member while the chain still lists it `in_qual` — no on-chain or RPC signal. On a public testnet with operators rebooting, quorums degrade invisibly toward threshold. Fix owed: share persistence, or an on-chain/RPC liveness signal, or both.
- **V11 activation-height gate.** The formation-boundary check (V11) is deliberately un-gated on the resettable fleet (`specialtx_validation.cpp:689–690`). If the formation cadence ever changes on a chain with history, historical PTXDKGs become retroactively invalid and a node syncing from genesis rejects blocks the network accepted — **split-on-resync**. A height-gate is required before any chain with history.
- **persist-t (KDD-048, Package-2 prerequisite).** V8 hardcodes `t=6` (`specialtx_validation.cpp:639`) and V5 hardcodes `n=11`; neither is on the wire or the record. The KDD-048 t=6→7 upgrade would **silently split** validators unless t is persisted explicitly (majority(11)=6 cannot yield 7, so it can't be derived from formed_size). Owed before the t-upgrade.
- **ODC-037 — banlist residue.** `banlist.dat` survives restart/wipe/restore; a restored fleet silently excludes a wiped-and-rejoining node (24h TTL, presents as broken-pipe). Owed: a banlist-clear step in `restore_fleet.sh`.
- **★ KDD-064 — PTX has NO maturity gate of its own** (ALLOCATED-THEN-ABANDONED; the intended PTX-specific gate never landed). **Coupling to record explicitly:** the deep-fork "mainnet-moot" verdict rests **entirely on inherited code** — the `UPGRADE_V3_4`-gated stake-modifier-V2 depth check (`consensus/params.h:264–267`, `contextHeight − utxoFromBlockHeight >= nStakeMinDepth`), `nStakeMinDepth = 300` mainnet (chainparams.cpp), and `-maxreorg` (validation.cpp:2958/3374). Because 300 > maxreorg=100, a fork-created coin can't re-stake until the fork is already 3× past maxreorg and unhealable for independent reasons — hence mainnet-moot. **This does NOT depend on the never-built KDD-064 gate, so the verdict stands.** But it *does* depend on that inherited check: **if `params.h:264–267`, `nStakeMinDepth`, or the maxreorg relationship ever moves, the deep-fork forensic conclusion must be re-derived** — there is no PTX-specific gate backstopping it.

---

*Cross-reference: the decision register is `doc/ptx/DKG_DESIGN_DOC_v1.md` (KDD/ODC entries). This roadmap tracks program status; the register tracks decisions. Live fleet state (above) is portable only as a hand-maintained snapshot.*
