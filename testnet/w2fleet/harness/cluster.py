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


class W2Cluster:
    def __init__(self,
                 n: int,
                 compose_file: str = DEF_COMPOSE,
                 rpc_user: str = DEF_RPC_USER,
                 rpc_pass: str = DEF_RPC_PASS,
                 host: str = _HOST,
                 port_base: int = 31000,
                 subnet_base: str = "172.31.0"):
        self.n = n
        self.compose_file = compose_file
        self.rpc_user = rpc_user
        self.rpc_pass = rpc_pass
        self.host = host
        self.port_base = port_base
        self.subnet_base = subnet_base

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
        cmd = ["docker", "compose", "-f", self.compose_file, "-p", PROJECT] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, check=check)

    def _assert_w2_compose(self):
        with open(self.compose_file) as f:
            head = f.read(4096)
        if "name: ptx-w2" not in head:
            raise AssertionError(
                f"REFUSED: {self.compose_file} does not declare 'name: ptx-w2' "
                f"— will not run lifecycle against a non-W2 compose file")

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
                return
            time.sleep(poll)
        not_ready = [nd.name for nd in self.all_nodes if not nd.is_rpc_ready()]
        raise TimeoutError(
            f"W2 cluster not ready after {timeout}s; non-responsive: {not_ready}")
