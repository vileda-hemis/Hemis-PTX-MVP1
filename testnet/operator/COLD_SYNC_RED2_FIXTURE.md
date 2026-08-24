# RED 2 FIXTURE for cold-sync-test.sh — a DELIBERATELY DIVERGENT BINARY.
# ★ NEVER APPLY THIS TO A TREE YOU INTEND TO SHIP. Build it separately, keep the
# binary out of the release, and point PTX_MUTANT_BINDIR at it.
#
# WHY THIS SHAPE. It reproduces h385: an UNGATED consensus rule that applies to
# all history, so a running fleet keeps its already-connected chain while a node
# validating from block 0 refuses a block the fleet accepted. That is the exact
# discrimination the harness exists to make -- "reached the chain and refused it"
# versus "could not reach it".
#
# WHY NOT A GENESIS/MAGIC MISMATCH INSTEAD (the cheaper option, rejected):
# ptxbea's magic is "PTX3" and ptxtestnet's is "PTXT" (chainparams.cpp), so a
# cross-chain probe never completes the HANDSHAKE. It fails at the network layer
# without ever seeing a block, which is indistinguishable from RED 1 -- it would
# prove the connectivity path while appearing to prove the validity one.
#
# Measured with this fixture on the ptxbea fleet 2026-08-24: wedged at height
# 209 with `bad-synthetic-coldsync`, peers=1, against a reference at 10530.
--- a/src/validation.cpp
+++ b/src/validation.cpp
@@ CheckBlock, immediately before "These are checks that are independent of context."
+    // ★★★ SYNTHETIC DEFECT — COLD-SYNC RED LEG FIXTURE ONLY. NEVER MERGE.
+    if (block.vtx.size() > 3)
+        return state.DoS(100, error("CheckBlock : synthetic cold-sync fixture"),
+                         REJECT_INVALID, "bad-synthetic-coldsync");
+
     // These are checks that are independent of context.
     const bool IsPoS = block.IsProofOfStake();

# ===========================================================================
# ★★ VARIANT A ABOVE IS CHAIN-SHAPE-DEPENDENT, AND ON A FRESH CHAIN IT IS
# VACUOUS. Found when the harness was first pointed at a newly mined
# ptxtestnet, 2026-08-24. "block.vtx.size() > 3" discriminates only on a chain
# whose blocks carry transactions -- true of the ptxbea fleet under 60
# rolls/block, and FALSE of every hand-mined chain, where each block holds one
# coinbase and nothing else. Run there, the mutant validates the whole chain
# happily, RED 2 reports "did not produce a validity rejection", and a reader
# who did not know why would read it as the harness being broken.
#
# ★ The fixture must fire on the chain it is pointed at. That is a property of
# the pair, not of the fixture, and it is exactly the vacuity trap this harness
# was built to expose -- turned on the harness's own falsification leg.
#
# VARIANT B -- HEIGHT-BASED. Use this on a freshly mined chain. It reproduces
# the same h385 shape (an UNGATED rule applied to all history: the fleet keeps
# its already-connected chain, a node validating from block 0 refuses a block
# the fleet accepted) without depending on how fat the blocks are. It goes in
# ContextualCheckBlock rather than CheckBlock because that is where nHeight
# exists (validation.cpp:3069).
# ===========================================================================
--- a/src/validation.cpp
+++ b/src/validation.cpp
@@ ContextualCheckBlock, immediately after nHeight/chainparams are established
     const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
     const CChainParams& chainparams = Params();
 
+    // ★★★ SYNTHETIC DEFECT -- COLD-SYNC RED LEG FIXTURE ONLY. NEVER MERGE.
+    if (nHeight > 25)
+        return state.DoS(100, error("ContextualCheckBlock : synthetic cold-sync fixture"),
+                         REJECT_INVALID, "bad-synthetic-coldsync");
+
     // Check that all transactions are finalized
     for (const auto& tx : block.vtx) {

# Pick the threshold BELOW the reference tip and ABOVE 0, or the leg collapses
# into RED 1: at 0 the probe never leaves genesis and looks like a connectivity
# failure; at or above the tip it never fires at all. 25 against a 51-block
# chain is comfortably inside both bounds.
