"""Step 16.3 — Enable GM payment via SPORK_21 and confirm corrected split.

Operator model (matches mainnet):
  The spork private key belongs to the chain administrator, not to any GM node.
  A transient broadcaster node is started with the key at init (the only load
  path — init.cpp:1225, SetPrivKey, no runtime setter), broadcasts SPORK_21=0
  to the fleet via P2P, then is stopped once the fleet confirms the spork live.
  This is exactly how a mainnet operator broadcasts sporks.

Prerequisites:
  - Fleet bootstrapped (handled internally — scenario calls do_bootstrap)
  - export PTXBEA_SPORK_KEY=$(cat /mnt/pve/Node14TB/hemis-ptx/ptxbea_spork.key)
    in the SAME shell as python3 (different Bash tool calls are separate shells)

Pass conditions (in order):
  1. Precondition: PTXBEA_SPORK_KEY non-empty (fails fast, before 168s bootstrap)
  2. Bootstrap complete (PoW → PoS → ProRegPL → Phase 2 restart)
  3. Broadcaster node starts on fleet network, loads sporkkey at init
  4. SPORK_21=0 broadcast accepted ("success")
  5. SPORK_21 confirmed live (value=0, active=True) on all 12 fleet nodes
  6. First post-spork PoS coinbase: GM = 267,499,999 sat (±2 sat)
  7. Broadcaster stopped, temp conf deleted

spork active format (verified against misc.cpp:283-286 + sporkid.h):
  Both "show" and "active" key by sporkDef.name = "SPORK_21_LEGACY_GMS_MAX_HEIGHT".
  "active" returns bool: IsSporkActive = GetSporkValue(id) < GetAdjustedTime().
  With value=0: 0 < current_time → True.

Run:
    export PTXBEA_SPORK_KEY=$(cat /mnt/pve/Node14TB/hemis-ptx/ptxbea_spork.key)
    python3 -m testnet.scenarios.spork_gm_payment [--compose /path/to/compose.yml]
"""

import sys
import os
import argparse
import subprocess
import tempfile
import time
import stat

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from testnet.harness.cluster import Cluster
from testnet.harness.bootstrap import bootstrap as do_bootstrap
from testnet.harness.runner import ScenarioRunner
from testnet.harness.node import Node, RPCError

COIN = 100_000_000

EXPECTED_GM_SAT = 267_499_999
SPORK21_NAME = "SPORK_21_LEGACY_GMS_MAX_HEIGHT"

# Broadcaster uses host port 30902 (outside fleet range 30903-30914)
BROADCASTER_HOST_PORT = 30902
BROADCASTER_RPC_USER  = "ptxbearpc"
BROADCASTER_RPC_PASS  = "ptxbeapass2026"
BROADCASTER_CONTAINER = "ptx-bea-broadcaster"
FLEET_NETWORK         = "ptx-bea-net"
FLEET_IMAGE           = "hemis-ptx-bea:354dc4b"
GM01_FLEET_IP         = "172.30.0.11"   # addnode target for broadcaster


def _write_broadcaster_conf(spork_key: str) -> str:
    """Write a chmod-600 temp conf with sporkkey. Returns path.

    Uses a NamedTemporaryFile with delete=False so the caller controls
    when it's deleted (must be in a finally block).
    """
    fd, path = tempfile.mkstemp(suffix=".conf", prefix="ptxbea-broadcaster-")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(
                f"rpcuser={BROADCASTER_RPC_USER}\n"
                f"rpcpassword={BROADCASTER_RPC_PASS}\n"
                f"rpcallowip=0.0.0.0/0\n"
                f"rpcbind=0.0.0.0\n"
                f"server=1\n"
                f"dnsseed=0\n"
                f"sporkkey={spork_key}\n"
                # No addnode= or listen= — peering done via RPC addnode call
                # after RPC-ready. listen defaults to 1 so outbound works.
                # Explicit RPC addnode is more reliable than conf for a
                # throwaway broadcaster with no persistent state.
            )
        os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)  # 0o600
    except Exception:
        os.unlink(path)
        raise
    return path


def _start_broadcaster(conf_path: str) -> None:
    """Start the transient broadcaster container on the fleet network.

    Uses --entrypoint Hemisd to bypass entrypoint.sh (which rewrites conf).
    Conf is mounted read-only; sporkkey= is loaded at init via -conf.
    listen=0 so it only makes outbound connections (to gm01 via addnode).
    """
    # Clean up any leftover container from a prior failed run
    subprocess.run(["docker", "rm", "-f", BROADCASTER_CONTAINER],
                   capture_output=True)

    result = subprocess.run([
        "docker", "run", "-d",
        "--name", BROADCASTER_CONTAINER,
        "--network", FLEET_NETWORK,
        "-v", f"{conf_path}:/broadcaster.conf:ro",
        "-p", f"{BROADCASTER_HOST_PORT}:29903",
        "--entrypoint", "Hemisd",
        FLEET_IMAGE,
        "-ptxbea",
        "-datadir=/root/.hemis-ptxbea",   # pre-created by Dockerfile RUN mkdir -p
        "-conf=/broadcaster.conf",
        "-rpcport=29903",
        "-rpcbind=0.0.0.0",
        "-port=29994",
    ], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"docker run failed (exit {result.returncode}):\n"
            f"  stdout: {result.stdout.strip()}\n"
            f"  stderr: {result.stderr.strip()}"
        )


def _dump_broadcaster_logs(lines: int = 50) -> None:
    """Print broadcaster container logs before teardown — diagnostic only."""
    result = subprocess.run(
        ["docker", "logs", "--tail", str(lines), BROADCASTER_CONTAINER],
        capture_output=True, text=True
    )
    combined = (result.stdout + result.stderr).strip()
    if combined:
        print(f"\n[broadcaster logs (last {lines} lines)] >>>")
        for line in combined.splitlines():
            print(f"  {line}")
        print("[broadcaster logs] <<<\n")
    else:
        print(f"[broadcaster logs] (empty — container may have crashed immediately)")


def _stop_broadcaster() -> None:
    _dump_broadcaster_logs()
    subprocess.run(["docker", "rm", "-f", BROADCASTER_CONTAINER],
                   capture_output=True)


def _wait_broadcaster_peered(broadcaster: Node, timeout: int = 60) -> None:
    """Connect broadcaster to the fleet via RPC addnode, then wait for ≥1 peer.

    Explicit RPC addnode is more reliable than conf addnode= for a throwaway
    node with no persistent peer cache. listen=0/1 is irrelevant here — that
    controls inbound connections; outbound addnode works regardless.
    """
    try:
        broadcaster.call("addnode", GM01_FLEET_IP, "add")
    except Exception as e:
        print(f"[16.3] addnode RPC: {e} (may already be added)")

    def has_peer():
        try:
            return len(broadcaster.call("getpeerinfo")) >= 1
        except Exception:
            return False
    broadcaster.wait_for_condition(has_peer, "broadcaster peered with fleet",
                                   timeout=timeout)


def _broadcast_spork21(broadcaster: Node) -> None:
    """Broadcast SPORK_21=0 from the broadcaster node."""
    print(f"[16.3] broadcasting SPORK_21=0 from transient broadcaster ...")
    result = broadcaster.call("spork", SPORK21_NAME, 0)
    if result != "success":
        raise AssertionError(
            f"spork broadcast returned {result!r}, expected 'success'.\n"
            "Check that the broadcaster started with SetPrivKey log line:\n"
            f"  docker logs {BROADCASTER_CONTAINER} | grep -i spork"
        )
    print(f"[16.3] broadcast accepted: {result}")


def _assert_spork21_live(runner: ScenarioRunner, nodes: list,
                         timeout: int = 90) -> None:
    """Hard-assert SPORK_21 value=0 and active=True on all fleet nodes.

    Fleet nodes, not the broadcaster — confirms the spork propagated
    through P2P to the entire fleet. Broadcaster stays alive until
    this assertion passes (keeps the P2P channel open during propagation).
    """
    print(f"[16.3] waiting for SPORK_21 live on all {len(nodes)} fleet nodes ...")
    deadline = time.time() + timeout

    while time.time() < deadline:
        live_count = 0
        for node in nodes:
            try:
                active = node.call("spork", "active")
                values = node.call("spork", "show")
                if (active.get(SPORK21_NAME) is True and
                        values.get(SPORK21_NAME) == 0):
                    live_count += 1
            except Exception:
                pass
        if live_count == len(nodes):
            print(f"[16.3] SPORK_21 live on all {len(nodes)} fleet nodes ✓")
            return
        time.sleep(3)

    for node in nodes:
        try:
            active = node.call("spork", "active")
            values = node.call("spork", "show")
            runner.assert_true(
                active.get(SPORK21_NAME) is True,
                f"{node.name}: SPORK_21 not active "
                f"(active={active.get(SPORK21_NAME)!r}, "
                f"value={values.get(SPORK21_NAME)!r})"
            )
            runner.assert_true(
                values.get(SPORK21_NAME) == 0,
                f"{node.name}: SPORK_21 value={values.get(SPORK21_NAME)!r}, "
                f"expected 0"
            )
        except RPCError as e:
            raise AssertionError(
                f"{node.name}: RPC error checking SPORK_21: {e}")


def _find_gm_coinbase(node: Node, start_height: int,
                      timeout: int = 300) -> tuple:
    """Return (height, gm_sat) for first post-spork PoS block with GM payment.

    PoS discriminant for ptxbea: nonce=0 AND version=11.
    (ptxbea has LLMQ qcTx so tx_count ≥ 3; do not use tx_count==2.)

    Returns (height, 0) on timeout — caller treats gm=0 as
    "DGM payment didn't fire" since spork-live was already asserted.
    """
    deadline = time.time() + timeout
    h = start_height + 1
    fallback = None

    while time.time() < deadline:
        try:
            bh = node.getblockhash(h)
        except RPCError:
            time.sleep(3)
            continue

        block = node.getblock(bh, 1)
        if block.get("nonce", 1) != 0 or block.get("version", 0) != 11:
            h += 1
            continue

        cb_txid = block["tx"][0]
        raw = node.call("getrawtransaction", cb_txid, True)
        gm_sat = round(sum(v["value"] for v in raw.get("vout", [])) * COIN)

        if fallback is None:
            fallback = (h, gm_sat)
        if gm_sat > 0:
            return h, gm_sat
        h += 1

    return fallback if fallback else (start_height + 1, 0)


def run_spork_gm_payment(runner: ScenarioRunner) -> None:
    cluster = runner.cluster
    caller  = cluster.caller
    gm01    = cluster.gms[0]

    # ── Precondition: fail fast before 168s bootstrap ────────────────────────
    spork_key = os.environ.get("PTXBEA_SPORK_KEY", "")
    if not spork_key:
        raise RuntimeError(
            "PTXBEA_SPORK_KEY is not set or empty.\n"
            "Run in the SAME shell:\n"
            "  export PTXBEA_SPORK_KEY=$(cat "
            "/mnt/pve/Node14TB/hemis-ptx/ptxbea_spork.key)\n"
            "  python3 -m testnet.scenarios.spork_gm_payment"
        )
    print(f"[16.3] precondition: PTXBEA_SPORK_KEY present "
          f"({len(spork_key)} chars) ✓")

    # ── Bootstrap ────────────────────────────────────────────────────────────
    print("[16.3] === bootstrap ===")
    cluster.up()
    cluster.wait_ready(timeout=120)
    runner.checkpoint("fleet up")

    do_bootstrap(cluster)
    runner.checkpoint("bootstrap complete")

    # ── Transient broadcaster: start, broadcast, keep alive for propagation ──
    conf_path = None
    try:
        print("[16.3] === starting transient spork broadcaster ===")
        conf_path = _write_broadcaster_conf(spork_key)
        _start_broadcaster(conf_path)
        print(f"[16.3] broadcaster container started ({BROADCASTER_CONTAINER})")

        broadcaster = Node(
            "broadcaster", "127.0.0.1", BROADCASTER_HOST_PORT,
            BROADCASTER_RPC_USER, BROADCASTER_RPC_PASS
        )
        broadcaster.wait_for_condition(
            broadcaster.is_rpc_ready,
            "broadcaster RPC ready", timeout=60
        )
        print("[16.3] broadcaster RPC ready ✓")

        _wait_broadcaster_peered(broadcaster, timeout=60)
        print(f"[16.3] broadcaster peered with fleet ✓")

        # ── Broadcast SPORK_21=0 ─────────────────────────────────────────────
        print("[16.3] === broadcasting SPORK_21=0 ===")
        _broadcast_spork21(broadcaster)
        runner.checkpoint("SPORK_21=0 broadcast accepted")

        # ── Assert live on ALL fleet nodes before stopping broadcaster ────────
        # Keep broadcaster alive — it's the relay channel. Only stop it after
        # the fleet confirms the spork, not immediately after broadcast.
        print("[16.3] === asserting SPORK_21 live on all 12 fleet nodes ===")
        _assert_spork21_live(runner, cluster.all_nodes, timeout=90)
        runner.checkpoint(
            "SPORK_21 confirmed live (value=0, active=True) on all 12 nodes"
        )

    finally:
        print("[16.3] stopping broadcaster and cleaning temp conf ...")
        _stop_broadcaster()
        if conf_path and os.path.exists(conf_path):
            os.unlink(conf_path)
        print("[16.3] broadcaster stopped, temp conf deleted ✓")

    # ── Observe post-spork GM coinbase ────────────────────────────────────────
    print("[16.3] === observing post-spork GM coinbase ===")
    tip = caller.getblockcount()
    gm01.wait_for_height(tip + 15, timeout=300)
    caller.wait_for_height(tip + 15, timeout=60)

    pay_height, gm_sat = _find_gm_coinbase(caller, tip, timeout=300)
    print(f"[16.3] coinbase at height {pay_height}: "
          f"GM = {gm_sat} sat ({gm_sat/COIN:.8f} HMS)")

    # ── Pass condition ────────────────────────────────────────────────────────
    runner.assert_true(
        abs(gm_sat - EXPECTED_GM_SAT) <= 2,
        f"GM coinbase {gm_sat} sat != expected {EXPECTED_GM_SAT} sat (±2).\n"
        f"SPORK_21 was confirmed live — if gm_sat=0, DGM payment path "
        f"didn't fire (check DGM registration and gamemaster-payments.cpp)."
    )

    print(f"\n[16.3] === PASS ===")
    print(f"[16.3] SPORK_21=0 accepted → propagated to 12 fleet nodes → "
          f"GM coinbase confirmed.")
    print(f"[16.3] GM = {gm_sat} sat = {gm_sat/COIN:.8f} HMS  "
          f"(expected {EXPECTED_GM_SAT} sat ±2) ✓")
    print(f"[16.3] Reward fix (2c9b26d) + rotated spork key (354dc4b) "
          f"proven end-to-end via faithful operator model.")
    runner.checkpoint(
        f"PASS: GM coinbase = {gm_sat} sat at height {pay_height}"
    )


def main():
    parser = argparse.ArgumentParser(
        description="Step 16.3: SPORK_21=0 via transient broadcaster, "
                    "confirm GM coinbase = 267,499,999 sat"
    )
    parser.add_argument("--compose", default=None)
    args = parser.parse_args()

    cluster_kwargs = {}
    if args.compose:
        cluster_kwargs["compose_file"] = args.compose

    cluster = Cluster(**cluster_kwargs)
    runner  = ScenarioRunner(cluster)
    runner.run(run_spork_gm_payment)


if __name__ == "__main__":
    main()
