"""Cluster lifecycle for the ISOLATED ptx-w2 fleet (W2.0a).

Parameterized-N descendant of testnet/harness/cluster.py (which is fixed to
the 11-GM ptx-bea PoC). Two safety properties this class enforces, because a
mis-scoped lifecycle command against the PoC wipes the live public demo:

1. Every compose invocation carries BOTH the explicit -f <generated compose>
   AND -p ptx-w2. No command can act on a default/ambient project.
2. down() refuses to run unless the compose file declares `name: ptx-w2` —
   a hard gate against ever pointing lifecycle at docker-bea's compose.

assert_poc_untouched() snapshots the ptx-bea container set (names + created
timestamps) and the docker-bea volume list; call it before and after every
W2 lifecycle operation and compare — any drift is an abort.
"""

import subprocess
import time
import os
from typing import Optional
from .node import Node

DEF_COMPOSE = "/mnt/pve/Node14TB/hemis-ptx/docker-w2/docker-compose.generated.yml"
DEF_RPC_USER = "ptxw2rpc"
DEF_RPC_PASS = "ptxw2pass2026"
_HOST = "127.0.0.1"
PROJECT = "ptx-w2"


def poc_snapshot() -> str:
    """Stable text snapshot of the PoC surfaces the W2 fleet must not touch."""
    ps = subprocess.run(
        ["docker", "ps", "-a", "--filter", "name=ptx-bea",
         "--format", "{{.Names}} {{.CreatedAt}} {{.Image}}"],
        capture_output=True, text=True).stdout
    vols = subprocess.run(
        ["docker", "volume", "ls", "--format", "{{.Name}}"],
        capture_output=True, text=True).stdout
    vols = "\n".join(sorted(v for v in vols.splitlines() if v.startswith("docker-bea_")))
    return ps.strip() + "\n---\n" + vols


def relock_collaterals(gm01: Node, expect_n: Optional[int] = None) -> int:
    """BUG-019 (a) INTERIM guard: lock every registered GM collateral UTXO in
    gm01's wallet (in-memory lockunspent), idempotent.

    MECHANISM (corrected 2026-07-10, SG-0 Piece 2): the daemon ALREADY
    auto-locks all IsMine DGM collaterals at init (-gmconflock default ON,
    tiertwo/init.cpp "automatic lock for DGM"), BEFORE RPC warm-up finishes,
    and the staker respects locks (StakeableCoins passes fIncludeLocked=false).
    This harness relock is therefore a BELT + the input to an ASSERTED
    invariant (validate_fleet lock_gate), not the primary protection.

    HONEST RESIDUALS (a) cannot close — both live BEFORE RPC exists:
      R1: ThreadStakeMinter starts at init.cpp:1855; the auto-lock runs
          inside InitActiveGM at :1860 — a staker-live-before-locks gap of
          seconds on every start.
      R2: any init failure between those points aborts InitActiveGM BEFORE
          the auto-lock while the staker is already live (observed 2026-07-10
          Piece-1 crash-loop: 33 blocks staked lock-free, clean by luck).
    BUG-019 (d) redefined by this finding: lock at WALLET LOAD / before the
    staker thread starts + cover the abort path (closes R1+R2); owed,
    pre-testnet-bound. gm44's 2026-07-07 death mechanism remains an OPEN
    sub-question (the recorded "no auto-lock exists" root cause is FALSIFIED
    — the auto-lock exists in the 40b109c binary; see standup 2026-07-10).

    Returns the number of collaterals covered (0 pre-registration)."""
    try:
        protx = gm01.protx_list(detailed=True, valid_only=True)
    except Exception:
        return 0  # pre-registration bootstrap stage — nothing to lock yet
    outs = [{"txid": e["collateralHash"], "vout": e["collateralIndex"]}
            for e in protx]
    if expect_n is not None and len(outs) != expect_n:
        raise AssertionError(
            f"relock: expected {expect_n} registered GMs, protx_list gave {len(outs)}")
    if not outs:
        return 0
    already = {(l["txid"], l["vout"])
               for l in gm01.call("listlockunspent")["transparent"]}
    todo = [o for o in outs if (o["txid"], o["vout"]) not in already]
    if todo:
        gm01.call("lockunspent", False, True, todo)
    locked = {(l["txid"], l["vout"])
              for l in gm01.call("listlockunspent")["transparent"]}
    missing = [o for o in outs if (o["txid"], o["vout"]) not in locked]
    if missing:
        raise AssertionError(
            f"relock: {len(missing)}/{len(outs)} collaterals NOT in listlockunspent")
    print(f"[relock] {len(outs)} collaterals locked "
          f"(in-memory; start->lock residual applies — BUG-019 (d) owed)")
    return len(outs)


class W2Cluster:
    def __init__(self,
                 n: int,
                 compose_file: str = DEF_COMPOSE,
                 rpc_user: str = DEF_RPC_USER,
                 rpc_pass: str = DEF_RPC_PASS,
                 host: str = _HOST,
                 port_base: int = 31000,
                 subnet_base: str = "172.31.0",
                 project: str = PROJECT):
        self.n = n
        self.compose_file = compose_file
        self.rpc_user = rpc_user
        self.rpc_pass = rpc_pass
        self.host = host
        self.port_base = port_base
        self.subnet_base = subnet_base
        self.project = project

        self.caller = Node("caller", host, port_base, rpc_user, rpc_pass)
        self.gms = [
            Node(f"gm{i:02d}", host, port_base + i, rpc_user, rpc_pass)
            for i in range(1, n + 1)
        ]
        self.all_nodes = [self.caller] + self.gms
        self._poc_baseline = poc_snapshot()

    # ── PoC guard ───────────────────────────────────────────────────────
    def assert_poc_untouched(self, context: str = ""):
        now = poc_snapshot()
        if now != self._poc_baseline:
            raise AssertionError(
                f"PoC DRIFT DETECTED ({context}) — ptx-bea containers/volumes "
                f"changed during a W2 operation. ABORT.\n--- baseline:\n"
                f"{self._poc_baseline}\n--- now:\n{now}")

    # ── scoped compose ──────────────────────────────────────────────────
    def _compose(self, *args, check: bool = True):
        cmd = ["docker", "compose", "-f", self.compose_file, "-p", self.project] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, check=check)

    def _assert_w2_compose(self):
        with open(self.compose_file) as f:
            head = f.read(4096)
        if f"name: {self.project}" not in head:
            raise AssertionError(
                f"REFUSED: {self.compose_file} does not declare 'name: {self.project}' "
                f"— will not run lifecycle against a mismatched compose file")

    def up(self):
        self._assert_w2_compose()
        self.assert_poc_untouched("pre-up")
        r = self._compose("up", "-d", "--remove-orphans", check=False)
        if r.returncode != 0:
            raise RuntimeError(f"compose up failed:\n{r.stderr}")
        self.assert_poc_untouched("post-up")

    def stop(self):
        """Graceful stop, volumes and containers KEPT — the banking-safe halt."""
        self._assert_w2_compose()
        r = self._compose("stop", "-t", "60", check=False)
        if r.returncode != 0:
            raise RuntimeError(f"compose stop failed:\n{r.stderr}")
        self.assert_poc_untouched("post-stop")

    def down(self, volumes: bool = False):
        """Tear down the ptx-w2 project ONLY (name-gated + project-scoped)."""
        self._assert_w2_compose()
        self.assert_poc_untouched("pre-down")
        args = ["down", "--remove-orphans"]
        if volumes:
            args.append("-v")
        r = self._compose(*args, check=False)
        if r.returncode not in (0, 1):
            raise RuntimeError(f"compose down failed:\n{r.stderr}")
        self.assert_poc_untouched("post-down")

    def restart_with_env(self):
        """Phase-2 restart after .env rewrite. down+up (not stop+up) so compose
        recreates containers and the entrypoint re-renders Hemis.conf with the
        compound ptxnodeid. Chain state is safe: datadirs are bind mounts on
        Node14TB; this project has no named volumes for down to remove."""
        self.down(volumes=False)
        self.up()

    def wait_ready(self, timeout: Optional[int] = None, poll: float = 3.0) -> None:
        if timeout is None:
            # first boot creates every HD wallet from scratch under full-fleet
            # CPU contention — measured ~9 min at N=22. This is a ceiling, not
            # a sleep; restored/warm fleets return in seconds.
            timeout = max(900, 10 + 30 * (self.n + 1))
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready = sum(1 for nd in self.all_nodes if nd.is_rpc_ready())
            if ready == len(self.all_nodes):
                # BUG-019 (a): re-apply the in-memory collateral locks at the
                # EARLIEST harness moment after every fleet start — this hook
                # covers every recipe that goes through wait_ready (bootstrap
                # Phase-2 restart, run_bootstrap, restore->validate, batteries).
                # The start->lock residual before this point is NOT closed
                # here — see relock_collaterals docstring; (d) owed.
                relock_collaterals(self.gms[0])
                return
            time.sleep(poll)
        not_ready = [nd.name for nd in self.all_nodes if not nd.is_rpc_ready()]
        raise TimeoutError(
            f"W2 cluster not ready after {timeout}s; non-responsive: {not_ready}")
