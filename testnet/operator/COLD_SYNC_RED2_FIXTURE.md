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
