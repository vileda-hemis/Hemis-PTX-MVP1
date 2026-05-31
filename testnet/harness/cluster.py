"""Cluster lifecycle management for the ptx-bea testnet fleet.

The compose_file parameter makes this reusable for any fleet variant
(Phase 2 IPv6, alternate network configs, etc.) — just pass a different
compose file path. The harness never hardcodes the fleet definition.
"""

import subprocess
import time
import os
from typing import Optional
from .node import Node

# Default fleet parameters — override via constructor arguments for IPv6 or
# other variants. The ptx-bea fleet is the Step 14 default.
_DEFAULT_COMPOSE = os.path.join(
    os.path.dirname(__file__),
    "../../../..",  # up to hemis-ptx/
    "docker-bea/docker-compose.yml",
)
_DEFAULT_COMPOSE = os.path.normpath(_DEFAULT_COMPOSE)

_RPC_USER = "ptxbearpc"
_RPC_PASS = "ptxbeapass2026"
_HOST = "127.0.0.1"

# Host-port assignments: caller=30903, gm01-gm11=30904-30914
_PORTS = {
    "caller": 30903,
    "gm01": 30904, "gm02": 30905, "gm03": 30906, "gm04": 30907,
    "gm05": 30908, "gm06": 30909, "gm07": 30910, "gm08": 30911,
    "gm09": 30912, "gm10": 30913, "gm11": 30914,
}


class Cluster:
    """Manages the ptx-bea Docker fleet lifecycle.

    Args:
        compose_file: Path to the docker-compose.yml for this fleet.
            Defaults to the ptx-bea fleet. Pass a different path for IPv6
            or other variants without modifying the harness.
        rpc_user / rpc_pass: RPC credentials matching the .env file.
        host: Host IP for RPC connections (127.0.0.1 for local Docker).
        ports: Dict mapping service name to host RPC port.
    """

    def __init__(
        self,
        compose_file: str = _DEFAULT_COMPOSE,
        rpc_user: str = _RPC_USER,
        rpc_pass: str = _RPC_PASS,
        host: str = _HOST,
        ports: Optional[dict] = None,
    ):
        self.compose_file = compose_file
        self.rpc_user = rpc_user
        self.rpc_pass = rpc_pass
        self.host = host
        self.ports = ports or dict(_PORTS)

        self.caller = Node("caller", host, self.ports["caller"], rpc_user, rpc_pass)
        self.gms = [
            Node(f"gm{i:02d}", host, self.ports[f"gm{i:02d}"], rpc_user, rpc_pass)
            for i in range(1, 12)
        ]
        self.all_nodes = [self.caller] + self.gms

    def _compose(self, *args, check: bool = True):
        cmd = ["docker", "compose", "-f", self.compose_file] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, check=check)

    def down(self, volumes: bool = True):
        """Tear down the ptx-bea fleet only. Never touches other fleets."""
        args = ["down"]
        if volumes:
            args.append("-v")
        result = self._compose(*args, check=False)
        if result.returncode not in (0, 1):  # 1 = already down, that's fine
            print(f"[cluster] down stderr: {result.stderr.strip()}")

    def up(self, env_overrides: Optional[dict] = None):
        """Start the fleet. Optionally write env_overrides to a temp env file."""
        if env_overrides:
            self._write_env_overrides(env_overrides)
        result = self._compose("up", "-d", "--remove-orphans", check=False)
        if result.returncode != 0:
            raise RuntimeError(f"docker compose up failed:\n{result.stderr}")

    def _write_env_overrides(self, overrides: dict):
        env_dir = os.path.dirname(self.compose_file)
        env_path = os.path.join(env_dir, ".env")
        lines = [f"RPCUSER={self.rpc_user}", f"RPCPASSWORD={self.rpc_pass}"]
        for k, v in overrides.items():
            lines.append(f"{k}={v}")
        with open(env_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    def wait_ready(self, timeout: int = 120, poll: float = 3.0) -> None:
        """Block until all nodes answer getblockcount()."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready = sum(1 for n in self.all_nodes if n.is_rpc_ready())
            if ready == len(self.all_nodes):
                return
            time.sleep(poll)
        not_ready = [n.name for n in self.all_nodes if not n.is_rpc_ready()]
        raise TimeoutError(
            f"Cluster not ready after {timeout}s; "
            f"non-responsive nodes: {not_ready}"
        )

    def assert_old_fleet_untouched(self):
        """Verify the ptx-phase2-bls fleet on 172.28.0.0/24 is still running."""
        result = subprocess.run(
            ["docker", "ps", "--filter", "name=ptx-caller",
             "--filter", "status=running", "--format", "{{.Names}}"],
            capture_output=True, text=True
        )
        if "ptx-caller" not in result.stdout:
            raise AssertionError(
                "ptx-phase2-bls fleet (ptx-caller) not running — "
                "check that the existing fleet was not disturbed"
            )
