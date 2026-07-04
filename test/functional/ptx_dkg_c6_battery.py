#!/usr/bin/env python3
# Copyright (c) 2026 The Hemis Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""W1.3 Package 3 C6 — cheap-tier anchoring/wiring battery (ODC-031).

Standalone Python-over-Hemis-cli driver (NOT a test_framework test): it runs
against a pre-existing bare banked regtest datadir (the h126 workbench snapshot)
and a locally built Hemisd, driving everything through RPC. Payload crafting is
SERVER-SIDE in the C5 debug RPC ptx_debug_ptxdkgpopulate (structural dummy
premits — throwaway-key BLS sigs; the anchoring checks V1-V3 fire before any
premit content matters), so this file stays special-tx-free.

Rows (each: prediction printed BEFORE running -> run -> content-verify against
real daemon output/debug.log):

  populate-refusal  contextual populate-time refusal (NO force): bad-anchor
                    payload refused AT populate (validate-before-inject,
                    ptx_dkg_pending.cpp), slot stays empty.
  V1                unknown quorum_hash -> ptxdkg-quorum-hash-not-found
                    (generate-time re-validation skip; keep-but-skip E-9).
  V2                wrong formation_height -> ptxdkg-formation-height-mismatch.
  V3-predicate      NON-ANCESTOR-PREDICATE: anchor exists in the index but is
                    not on pindexPrev's active chain (invalidateblock sibling)
                    -> ptxdkg-quorum-hash-not-active-chain.
                    Proves V3 rejects a static non-ancestor anchor. Does NOT
                    cover V3-REORG-TRANSITION — an anchor that was active and
                    orphaned mid-flight, pindexPrev actually moved — which is
                    W2.2-bound (needs a live orphanable formation quorum).
  F-5               build_only tx_hex -> sendrawtransaction ->
                    ptxdkg-mempool-rejected (KDD-058 mechanism b); mempool empty.

C3-invocation (two-PTXDKG block -> ptxdkg-duplicate through the real
ProcessSpecialTxsInBlock) is DEFERRED TO W2.2: the per-tx CheckSpecialTx loop
(specialtx_validation.cpp:1151) runs before CheckPTXDKGBlockRules (:1171), so
reaching the duplicate rule needs payloads passing V1-V8 -> 11 registered GMs ->
datadir-v2. NOT unit-covered — FA-2b proved the unit gate is blind to the
invocation; bound to W2.2 via F-6's accept-path + the datadir-v2 duplicate case.

stub->RED mode: --only ROW --expect-red asserts the row's falsification
prediction against a per-check stubbed REBUILD of the binary (stub applied
container-side only, reverted + re-greened after each run). No consensus-check-
disable flags exist in the shipped code; falsification is by rebuild only.

Usage:
  ptx_dkg_c6_battery.py --bindir /build/work/src --datadir /build/wb/datadir
                        [--only ROW] [--expect-red] [--staking 0|1]
Exit 0 iff every executed row matched its prediction.
"""

import argparse
import json
import os
import subprocess
import sys
import time

ROWS = ["populate-refusal", "V1", "V2", "V3-predicate", "F-5"]

UNKNOWN_HASH = "ab" * 32

DAEMON_FLAGS = ["-regtest", "-nuparams=GM_Enable:1", "-nuparams=PoS:121",
                "-debug=1", "-listenonion=0"]


class Driver:
    def __init__(self, bindir, datadir, staking):
        self.hemisd = os.path.join(bindir, "Hemisd")
        self.cli = os.path.join(bindir, "Hemis-cli")
        self.datadir = datadir
        self.staking = staking
        self.log_path = os.path.join(datadir, "regtest", "debug.log")
        self.results = []  # (row, matched, predicted, observed)

    # -- daemon lifecycle -------------------------------------------------
    def start(self):
        cmd = [self.hemisd] + DAEMON_FLAGS + [
            "-staking=%d" % self.staking, "-datadir=%s" % self.datadir, "-daemon"]
        # DEVNULL, not PIPE: the daemonized child inherits the launcher's fds,
        # so capturing output would block on pipe-EOF forever.
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        for _ in range(120):
            time.sleep(1)
            try:
                self.rpc("getblockcount")
                return
            except Exception:
                continue
        raise RuntimeError("daemon did not come up within 120s")

    def stop(self):
        try:
            self.rpc("stop")
        except Exception:
            pass
        for _ in range(60):
            time.sleep(1)
            try:
                self.rpc("getblockcount")
            except Exception:
                return
        raise RuntimeError("daemon did not stop within 60s")

    # -- rpc / log helpers -------------------------------------------------
    def rpc(self, *args):
        """Returns parsed JSON (or raw string). Raises CliError on nonzero."""
        p = subprocess.run(
            [self.cli, "-regtest", "-datadir=%s" % self.datadir] + list(args),
            capture_output=True, text=True)
        if p.returncode != 0:
            raise CliError((p.stderr or p.stdout).strip())
        out = p.stdout.strip()
        try:
            return json.loads(out)
        except (ValueError, TypeError):
            return out

    def log_tell(self):
        return os.path.getsize(self.log_path) if os.path.exists(self.log_path) else 0

    def log_since(self, offset):
        with open(self.log_path, "r", errors="replace") as f:
            f.seek(offset)
            return f.read()

    # -- payload helpers ---------------------------------------------------
    @staticmethod
    def spec(quorum_hash, formation_height, **extra):
        d = {"quorum_hash": quorum_hash, "formation_height": formation_height,
             "group_pk": "generate", "premits": 6, "members": 11}
        d.update(extra)
        return json.dumps(d)

    def populate(self, spec_json, force):
        return self.rpc("ptx_debug_ptxdkgpopulate", spec_json,
                        "true" if force else "false")

    def block_has_ptxdkg(self, blockhash):
        """True iff any tx in the block is nType==11 (PTXDKG).

        The strict txcount==2 form of this assertion is WRONG: the inherited
        LLMQ machinery injects a null quorum-commitment (nType==5) at the
        llmq-test session's mining window (first observed at h130 on the h126
        workbench), so a 3-tx block can be perfectly clean. The consensus
        property under test is 'the pending PTXDKG did not enter the block'.
        """
        blk = self.rpc("getblock", blockhash)
        for txid in blk["tx"]:
            tx = self.rpc("getrawtransaction", txid, "1")
            if tx.get("type") == 11:
                return True, blk
        return False, blk

    # -- assertion/reporting ----------------------------------------------
    def report(self, row, matched, predicted, observed):
        self.results.append((row, matched, predicted, observed))
        tag = "GREEN — prediction matched" if matched else "*** MISMATCH ***"
        print("[%s] %s\n  predicted: %s\n  observed:  %s" % (row, tag, predicted, observed))

    # -- rows ----------------------------------------------------------------
    def row_populate_refusal(self, expect_red):
        row = "populate-refusal"
        off = self.log_tell()
        if expect_red:
            # RED (stub ptx_dkg_pending.cpp:32): no-force bad-anchor populate SUCCEEDS.
            predicted = ("populate(no force, unknown anchor) returns populated=true; "
                         "log 'slot set to'; no 'refusing' line")
            try:
                res = self.populate(self.spec(UNKNOWN_HASH, 100), force=False)
            except CliError as e:
                self.report(row, False, predicted, "still refused: %s" % e)
                return
            log = self.log_since(off)
            ok = (res.get("populated") is True and res.get("force") is False
                  and "pending PTXDKG slot set to" in log and "refusing" not in log)
            self.report(row, ok, predicted,
                        "populated=%s force=%s; slot-set-line=%s refusing-line=%s" % (
                            res.get("populated"), res.get("force"),
                            "pending PTXDKG slot set to" in log, "refusing" in log))
            return
        predicted = ("RPC -26 'populate refused: ptxdkg-quorum-hash-not-found'; log "
                     "'refusing pending PTXDKG'; slot empty: generate 1 -> txcount==2, "
                     "no 'skipped at generate time' line")
        try:
            self.populate(self.spec(UNKNOWN_HASH, 100), force=False)
            self.report(row, False, predicted, "populate unexpectedly SUCCEEDED")
            return
        except CliError as e:
            err = str(e)
        log1 = self.log_since(off)
        off2 = self.log_tell()
        self.rpc("generate", "1")
        has_dkg, tip = self.block_has_ptxdkg(self.rpc("getbestblockhash"))
        log2 = self.log_since(off2)
        ok = ("populate refused: ptxdkg-quorum-hash-not-found" in err
              and "refusing pending PTXDKG" in log1
              and not has_dkg
              and "skipped at generate time" not in log2)
        self.report(row, ok, predicted,
                    "err=%r; refusing-line=%s; dkg-in-block=%s (txcount=%d); skip-line-after=%s" % (
                        err.splitlines()[-1] if err else "",
                        "refusing pending PTXDKG" in log1, has_dkg, len(tip["tx"]),
                        "skipped at generate time" in log2))

    def _force_skip_row(self, row, spec_json, expect_reason, red_predicate=None,
                        red_predicted=None):
        """Shared body for V1/V2/V3: force-populate -> generate -> skip-reason."""
        res = self.populate(spec_json, force=True)
        txid = res["txid"]
        off = self.log_tell()
        gen_err = None
        try:
            self.rpc("generate", "1")
        except CliError as e:
            gen_err = str(e)
        log = self.log_since(off)
        if red_predicate is not None:
            ok, observed = red_predicate(log, gen_err)
            self.report(row, ok, red_predicted, observed)
            return
        has_dkg, tip = self.block_has_ptxdkg(self.rpc("getbestblockhash"))
        skip = ("pending PTXDKG %s skipped at generate time (%s) — slot kept"
                % (txid, expect_reason))
        ok = (gen_err is None and skip in log and not has_dkg
              and txid not in tip["tx"])
        self.report(row, ok,
                    "skip line '(%s)'; no PTXDKG (nType 11) in block; txid absent"
                    % expect_reason,
                    "skip-line=%s; dkg-in-block=%s (txcount=%d); txid-in-block=%s; generate-err=%r" % (
                        skip in log, has_dkg, len(tip["tx"]), txid in tip["tx"], gen_err))

    def row_v1(self, expect_red):
        spec = self.spec(UNKNOWN_HASH, 100)
        if expect_red:
            # RED (stub :662 remaps unknown anchor to pindexPrev): reason falls
            # through to V2 (100 != tip height).
            def pred(log, gen_err):
                ok = ("(ptxdkg-formation-height-mismatch)" in log
                      and "(ptxdkg-quorum-hash-not-found)" not in log)
                return ok, ("mismatch-skip=%s; not-found-skip=%s; generate-err=%r" % (
                    "(ptxdkg-formation-height-mismatch)" in log,
                    "(ptxdkg-quorum-hash-not-found)" in log, gen_err))
            self._force_skip_row("V1", spec, None, pred,
                                 "reason CHANGES to formation-height-mismatch; "
                                 "quorum-hash-not-found ABSENT")
            return
        self._force_skip_row("V1", spec, "ptxdkg-quorum-hash-not-found")

    def row_v2(self, expect_red):
        anchor = self.rpc("getblockhash", "100")
        spec = self.spec(anchor, 101)
        if expect_red:
            # RED (stub :669): falls through V3 (passes) to V5 underfull.
            def pred(log, gen_err):
                ok = ("(ptxdkg-quorum-underfull)" in log
                      and "(ptxdkg-formation-height-mismatch)" not in log)
                return ok, ("underfull-skip=%s; mismatch-skip=%s; generate-err=%r" % (
                    "(ptxdkg-quorum-underfull)" in log,
                    "(ptxdkg-formation-height-mismatch)" in log, gen_err))
            self._force_skip_row("V2", spec, None, pred,
                                 "reason CHANGES to quorum-underfull; "
                                 "formation-height-mismatch ABSENT")
            return
        self._force_skip_row("V2", spec, "ptxdkg-formation-height-mismatch")

    def row_v3(self, expect_red):
        # V3-PREDICATE (non-ancestor anchor). NOT V3-REORG-TRANSITION (W2.2).
        h = self.rpc("getblockcount")
        h_old = self.rpc("getblockhash", str(h))
        self.rpc("invalidateblock", h_old)
        self.rpc("generate", "1")  # sibling tip at height h, hash != h_old
        spec = self.spec(h_old, h)
        if expect_red:
            # RED (stub :676): V4 GetListForBlock(orphaned index). Primary: throw
            # propagates -> generate RPC error, no V3 skip line. Secondary: V5
            # underfull skip. Either way not rejected for the V3 reason.
            def pred(log, gen_err):
                v3_absent = "(ptxdkg-quorum-hash-not-active-chain)" not in log
                threw = gen_err is not None
                underfull = "(ptxdkg-quorum-underfull)" in log
                ok = v3_absent and (threw or underfull)
                return ok, ("v3-skip-absent=%s; generate-err=%r; underfull-skip=%s" % (
                    v3_absent, gen_err, underfull))
            self._force_skip_row("V3-predicate", spec, None, pred,
                                 "NOT rejected for V3 reason: generate throws "
                                 "(GetListForBlock, primary) OR underfull skip "
                                 "(secondary)")
            return
        self._force_skip_row("V3-predicate", spec,
                             "ptxdkg-quorum-hash-not-active-chain")

    def row_f5(self, expect_red):
        row = "F-5"
        off = self.log_tell()
        res = self.populate(self.spec(UNKNOWN_HASH, 100, build_only=True),
                            force=False)
        log = self.log_since(off)
        build_ok = (res.get("build_only") is True and res.get("populated") is False
                    and "tx_hex" in res
                    and "slot set to" not in log and "FORCE-populating" not in log)
        if not build_ok:
            self.report(row, False, "build_only returns tx_hex without touching slot",
                        "build_only=%s populated=%s slot-touched=%s" % (
                            res.get("build_only"), res.get("populated"),
                            ("slot set to" in log or "FORCE-populating" in log)))
            return
        # RED fall-through gate (amended after RED-5 run #1): with the :399
        # mechanism-b clause stubbed, the next gate on this pre-v5 chain is the
        # POLICY standardness check (validation.cpp:437 -> policy.cpp:106
        # 'version': UPGRADE_V5_0 inactive => nVersion != LEGACY nonstandard) —
        # NOT CheckSpecialTx V1, which sits later (:506) and would be the next
        # gate only on a v5-active chain. Mechanism b remains the only gate that
        # is both consensus-graded and version-independent.
        predicted = ("-26 ptxdkg-mempool-rejected; mempool empty" if not expect_red
                     else "error CHANGES to 'version' (policy IsStandardTx gate); "
                          "ptxdkg-mempool-rejected ABSENT; mempool empty")
        try:
            self.rpc("sendrawtransaction", res["tx_hex"])
            self.report(row, False, predicted, "sendrawtransaction ACCEPTED")
            return
        except CliError as e:
            err = str(e)
        mempool = self.rpc("getrawmempool")
        if expect_red:
            ok = ("version" in err
                  and "ptxdkg-mempool-rejected" not in err and mempool == [])
        else:
            ok = "ptxdkg-mempool-rejected" in err and mempool == []
        self.report(row, ok, predicted, "err=%r; mempool=%s" % (
            err.splitlines()[-1] if err else "", mempool))


class CliError(Exception):
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bindir", required=True)
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--only", choices=ROWS)
    ap.add_argument("--expect-red", action="store_true",
                    help="assert the row's stub->RED prediction (use with --only)")
    ap.add_argument("--staking", type=int, default=1, choices=[0, 1])
    args = ap.parse_args()
    if args.expect_red and not args.only:
        ap.error("--expect-red requires --only")

    d = Driver(args.bindir, args.datadir, args.staking)
    rows = {
        "populate-refusal": d.row_populate_refusal,
        "V1": d.row_v1,
        "V2": d.row_v2,
        "V3-predicate": d.row_v3,
        "F-5": d.row_f5,
    }
    torun = [args.only] if args.only else ROWS

    d.start()
    try:
        print("chain height at start: %s" % d.rpc("getblockcount"))
        for name in torun:
            print("\n=== row %s%s ===" % (name, " [expect-RED]" if args.expect_red else ""))
            rows[name](args.expect_red)
    finally:
        d.stop()

    print("\n===== summary =====")
    all_ok = True
    for row, matched, _, _ in d.results:
        print("%-18s %s" % (row, "MATCHED" if matched else "MISMATCH"))
        all_ok = all_ok and matched
    ran = [r for r, *_ in d.results]
    missing = [r for r in torun if r not in ran]
    if missing:
        print("rows that did not report: %s" % missing)
        all_ok = False
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
