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

import re
import subprocess
import time
import os
from pathlib import Path
from typing import Optional
from .node import Node

DEF_COMPOSE = "/mnt/pve/Node14TB/hemis-ptx/docker-w2/docker-compose.generated.yml"
DEF_RPC_USER = "ptxw2rpc"


def _env_cred(key: str, fallback: str) -> str:
    """Read a credential from the generated .env, falling back to the literal.

    ★ WHY THIS EXISTS (2026-08-06): DEF_RPC_PASS used to be the hardcoded
    fixture password. When the fleet's RPC credentials were rotated at rebuild,
    gen_fleet wrote the new value to .env and the containers picked it up — but
    the harness kept sending the OLD one, so every RPC failed AUTH. is_rpc_ready
    swallows the exception, so 106 auth failures presented as 106 "non-responsive"
    nodes and burned a 50-minute wait_ready timeout before anything said "401".
    The credential now has ONE source of truth: the .env gen_fleet writes.
    """
    env_path = os.path.join(os.path.dirname(DEF_COMPOSE), ".env")
    try:
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith(key + "="):
                    val = line.split("=", 1)[1].strip()
                    if val:
                        return val
    except OSError:
        pass
    return fallback


DEF_RPC_PASS = _env_cred("RPCPASSWORD", "ptxw2pass2026")


def ptxbea_boundary_interval() -> int:
    """nBoundaryInterval for the fleet's chain (-ptxbea), parsed from the
    tree's chainparams.cpp — the same tree that builds the fleet image.

    ★ WHY THIS EXISTS (2026-08-06): the eligibility gate carried
    FORMATION_N = 80, a hand-copied SG-1b dev-net value. W2.5b's fleet shape
    (KDD-079 decouple) moved ptxbea's boundary cadence to 30; the copy kept
    80, the gate anchored off-boundary and refused a chain that satisfied it.
    Same defect class as the RPCPASSWORD literal above: a copied constant
    rotting while its source of truth moved. NO fallback — a parse failure
    raises, because a silent default IS the class this kills.

    HONEST LIMIT: binds to the SOURCE TREE, not the running binary. A tree
    moved ahead of the running image is the standing stale-image trap; it is
    caught downstream — an off-boundary refusal from the boundary-anchored
    probe names the tree-vs-binary mismatch explicitly."""
    src = Path(__file__).resolve().parents[3] / "src" / "chainparams.cpp"
    m = re.search(r'consensus\.ptxFormation\s*=\s*\{\s*"ptxbea"\s*,\s*(\d+)',
                  src.read_text())
    if not m:
        raise RuntimeError(
            f"cannot parse ptxbea ptxFormation from {src} — chainparams "
            f"moved; fix this parse, do NOT reintroduce a literal")
    n = int(m.group(1))
    if n < 1:
        raise RuntimeError(f"parsed nonsense boundary interval {n} from {src}")
    return n
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


def relock_collaterals(treasury: Node, expect_n: Optional[int] = None) -> int:
    """BUG-019 (a) INTERIM guard: lock every registered GM collateral UTXO in
    the TREASURY wallet (in-memory lockunspent), idempotent.

    ★ RETARGETED gm01 -> CALLER for the wallet-less-GM topology (Phase 2).
    GMs now run -disablewallet=1 + -gmoperatorprivatekey, so a GM has no wallet,
    holds no collateral, and cannot stake (ThreadStakeMinter is inside
    #ifdef ENABLE_WALLET *and* gated on !vpwallets.empty(), init.cpp:1889-1891)
    — the GM-side BUG-019 exposure is gone BY CONSTRUCTION, not mitigated.
    But the exposure MOVES rather than vanishing: collateral now lives in the
    caller wallet, and the caller is the staker. Caller = holds collateral +
    stakes = exactly the "combined topology" this docstring flags below as the
    remaining silent-loss path. So this guard is RETARGETED AND KEPT, never
    retired.
    The daemon-side protection follows it automatically, because it is
    WALLET-scoped and not GM-role-scoped: init.cpp:1886 calls
    LockGamemasterCollaterals() on `!vpwallets.empty()` (NOT on fGameMaster),
    and that function iterates every wallet in vpwallets locking any DGM
    collateral that is IsMine. The caller therefore auto-locks before its own
    staker thread starts, with no -gamemaster flag required.

    MECHANISM (corrected 2026-07-10, SG-0 Piece 2): the daemon ALREADY
    auto-locks all IsMine DGM collaterals at init (-gmconflock default ON,
    tiertwo/init.cpp "automatic lock for DGM"), BEFORE RPC warm-up finishes,
    and the staker respects locks (StakeableCoins passes fIncludeLocked=false).
    This harness relock is therefore a BELT + the input to an ASSERTED
    invariant (validate_fleet lock_gate), not the primary protection.

    ★ R1/R2 ARE FIXED AT 870acc7 — do NOT read the historical framing below as
    current state (this docstring previously described the pre-fix world and
    produced a wrong "unmitigated exposure" analysis on 2026-07-30; corrected
    here). BUG-019 (d) was DISCHARGED by hoisting the lock ahead of the staker:
        init.cpp:1886  LockGamemasterCollaterals()          <- lock FIRST
        init.cpp:1891  create_thread(ThreadStakeMinter)     <- staker SECOND
        init.cpp:1896  InitActiveGM()                       <- no longer locks
    See tiertwo/init.cpp:231-239 for the contract. This closes BOTH residuals
    STRUCTURALLY: R1 (the staker's first stakeable-coins snapshot is now
    post-lock) and R2 (an init abort before the lock is also before the staker
    start, since the lock precedes it on the same code path). Runtime is covered
    too: LockIfMyCollateral, called from AddToWalletIfInvolvingMe
    (wallet.cpp:1102), locks a ProRegTx collateral the moment it lands, so a GM
    registered while the node is running does not wait for a restart.
    VERIFIED LIVE on the N98 fleet 2026-07-30: across 98 nodes x 3 boots, ZERO
    staker-starts occurred without a preceding "Locking gamemaster collaterals".

    HISTORICAL (pre-870acc7, retained for provenance — NOT current):
      R1: ThreadStakeMinter started at init.cpp:1855 while the auto-lock ran
          inside InitActiveGM at :1860 — a staker-live-before-locks gap.
      R2: an init failure between those points aborted InitActiveGM BEFORE the
          auto-lock while the staker was already live (2026-07-10 Piece-1
          crash-loop: 33 blocks staked lock-free, clean by luck).
    gm44's 2026-07-07 death mechanism: the recorded "no auto-lock exists" root
    cause is FALSIFIED — the auto-lock existed even in 40b109c; the real
    mechanism was R1 (see standup 2026-07-10/12).

    ONLY REMAINING silent-collateral-loss path: an operator explicitly setting
    -gmconflock=0 with collateral IsMine in a staking-enabled wallet (combined
    topology). Self-inflicted, single-node, requires an opt-out; a loud warning
    is owed. Correct hot/cold topology is immune regardless (StakeableCoins
    skips ISMINE_NO), as is any fresh collateral (nStakeMinDepth = 300 mainnet).

    Returns the number of collaterals covered (0 pre-registration).

    NOTE protx_list is wallet-SAFE (rpcevo.cpp:868-880 handles pwallet==nullptr;
    only wallet_only=true throws, and we pass False), so this still reads
    correctly even if pointed at a wallet-less node — but the lockunspent /
    listlockunspent calls below do NOT: under -disablewallet those RPCs are
    unregistered outright (rpcwallet.cpp RegisterWalletRPCCommands early-return)
    and fail RPC_METHOD_NOT_FOUND "Method not found (disabled)". Point this at
    the wallet-holding treasury, never at a GM."""
    try:
        protx = treasury.protx_list(detailed=True, valid_only=True)
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
               for l in treasury.call("listlockunspent")["transparent"]}
    todo = [o for o in outs if (o["txid"], o["vout"]) not in already]
    if todo:
        treasury.call("lockunspent", False, True, todo)
    locked = {(l["txid"], l["vout"])
              for l in treasury.call("listlockunspent")["transparent"]}
    missing = [o for o in outs if (o["txid"], o["vout"]) not in locked]
    if missing:
        raise AssertionError(
            f"relock: {len(missing)}/{len(outs)} collaterals NOT in listlockunspent")
    print(f"[relock] {len(outs)} collaterals locked in {treasury.name} "
          f"(in-memory belt; daemon auto-lock is the primary, and at 870acc7 it "
          f"precedes the staker start)")
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
                 project: str = PROJECT,
                 callers: int = 1):
        self.n = n
        self.n_callers = callers
        self.compose_file = compose_file
        self.rpc_user = rpc_user
        self.rpc_pass = rpc_pass
        self.host = host
        self.port_base = port_base
        self.subnet_base = subnet_base
        self.project = project

        # ★ THE PRODUCER SET (Phase 2).  Under the wallet-less-GM topology the
        # callers are not just demand generators and drill spares — they are the
        # ENTIRE block-producing population, because no GM can stake.  The
        # cluster therefore has to model all of them, not just the primary:
        # modelling one caller is what let the fleet sit in a silent
        # single-producer state (7 of 8 callers funded 0.0 HMS, -staking=1 inert)
        # while the compose claimed 8 producers.
        # Port allocation MIRRORS gen_fleet.py exactly — caller1 at port_base,
        # caller_k>1 at port_base + n + (k-1). Keep the two in step.
        self.callers = [
            Node(f"caller{k}" if callers > 1 else "caller", host,
                 port_base if k == 1 else port_base + n + (k - 1),
                 rpc_user, rpc_pass)
            for k in range(1, callers + 1)
        ]
        # Primary caller: the treasury and the harness's drive endpoint.
        # `self.caller` kept as the long-standing name for it (back-compat with
        # every recipe and battery that already says cluster.caller).
        self.caller = self.callers[0]
        self.gms = [
            Node(f"gm{i:02d}", host, port_base + i, rpc_user, rpc_pass)
            for i in range(1, n + 1)
        ]
        self.all_nodes = list(self.callers) + self.gms
        # ★ TREASURY (Phase 2, wallet-less-GM topology): the single wallet-
        # holding node that mines the PoW prefix, funds collateral, registers
        # every GM and holds the per-GM scriptPTXPayment addresses.  This was
        # gm01 while GMs carried wallets; under -disablewallet=1 GMs have no
        # wallet at all, so EVERY wallet RPC must land here instead.  Kept as an
        # attribute rather than hardcoded so a spare caller can take the role
        # after a restore without editing the recipes.
        self.treasury = self.caller
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
                # ★ Targets the TREASURY (caller), not gms[0]: under the
                # wallet-less-GM topology the collateral lives in the caller
                # wallet and a GM would answer these RPCs with
                # "Method not found (disabled)".
                relock_collaterals(self.treasury)
                return
            time.sleep(poll)
        stragglers = [nd for nd in self.all_nodes if not nd.is_rpc_ready()]
        not_ready = [nd.name for nd in stragglers]
        # ★ Report WHY, not just WHO. A uniform reason across every node means a
        # CONFIG fault (auth, port, host), not N independent node failures — the
        # fleet-identical signature. Without this the operator sees only a list.
        reasons = {}
        for nd in stragglers:
            r = getattr(nd, "last_rpc_error", None) or "unknown"
            reasons.setdefault(r, []).append(nd.name)
        summary = "; ".join(f"{r} [{len(v)} nodes, e.g. {v[0]}]" for r, v in reasons.items())
        raise TimeoutError(
            f"W2 cluster not ready after {timeout}s; non-responsive: {len(not_ready)}/"
            f"{len(self.all_nodes)}\n  REASONS: {summary}\n  nodes: {not_ready}")
