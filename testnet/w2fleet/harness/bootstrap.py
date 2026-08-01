"""Bootstrap a fresh ptx-w2 chain at N GMs — RECONCILED to HEAD (W2.0a Amendment 1).

Descends from testnet/harness/bootstrap.py (the 11-GM ptx-bea recipe). The
registration CONSENSUS surface is byte-identical 801c557->40b109c (source-diff:
rpcevo.cpp, providertx.*, deterministicgms.*, wallet.cpp all unchanged; the
only specialtx_validation delta is the additive PTXDKG validator). What HEAD
adds is on the CONSUMER side, and it is why this file is a reconcile, not a
copy:

  1. KDD-060 eligibility (PTX_DKG_IsGMPTXEligible, ptx_dkg.cpp:109) requires a
     non-empty node_id — provided here as before via the ProRegPL label.
  2. CalculateScores (deterministicgms.cpp:251) SILENTLY SKIPS any GM whose
     confirmedHash is null, and PTX_DKG_InitSession hard-asserts it. A GM is
     confirmed only once its registration is nGMCollateralMinConf=1 deep,
     applied at the NEXT block (deterministicgms.cpp:597-600) — i.e. eligible
     from tip >= registeredHeight + 2. The old recipe waited one block after
     registration: registered, yes; selectable, NOT guaranteed. This flow
     waits for confirmation explicitly (wait_for_gm_confirmation) — the
     "looks registered but fails eligibility" failure Amendment 1 targets.
  3. Registration is paced in batches of K with a one-block wait between
     batches (N=60 in a single mempool window exercises wallet unconfirmed-
     chain limits the 11-GM recipe never saw).

Everything load-bearing in the original is preserved verbatim in spirit and
cited to its origin: external-collateral protx_register with the KDD-033
suffix (bootstrap.py:98-109 rationale), the lockunspent guard against
FundSpecialTx eating future collateral (:136-148), per-GM scriptPTXPayment for
attribution (:164-168, KDD-035 as amended — see below), the two-phase
compound-id restart (:227-296).

★ PHASE 2 — WALLET-LESS GM TOPOLOGY (the treasury retarget)
-----------------------------------------------------------
GMs now run `-disablewallet=1 -gamemaster=1 -gmoperatorprivatekey=<bls>`.  A GM
has no wallet, so it cannot hold coins, cannot register anything, and — the
point of the change — cannot stake: ThreadStakeMinter is inside
`#ifdef ENABLE_WALLET` AND gated on `!vpwallets.empty()` (init.cpp:1889-1891),
so with no wallet the staker thread is never created.  The BUG-019 class is
gone at the root for GMs rather than mitigated.

Consequence for this recipe: gm01 was the treasury — it mined the PoW prefix,
held every collateral, and signed every registration.  That role moves WHOLESALE
to a caller (`cluster.treasury`, the primary caller).  Under -disablewallet the
wallet RPCs are not merely permission-denied, they are UNREGISTERED
(rpcwallet.cpp RegisterWalletRPCCommands early-returns), so a stray GM-side call
fails loudly with RPC_METHOD_NOT_FOUND "Method not found (disabled)".
Wallet-SAFE and deliberately left node-agnostic: protx_list (handles
pwallet==nullptr; only wallet_only=true throws), generateblskeypair (pure
keygen), getblockcount/getrawtransaction/getpeerinfo.
★ generatetoaddress is NOT wallet-safe despite taking an explicit address — it
calls EnsureWalletIsAvailable (mining.cpp:155), so PoW mining must run on the
treasury too.

★ KDD-035 AMENDMENT (necessary, not optional)
---------------------------------------------
Original intent: each GM's scriptPTXPayment came from the GM's OWN wallet, so
"the winning GM's wallet reflects the win" — attribution by wallet ownership.
A wallet-less GM cannot hold a payment address, so that intent cannot survive
the topology.  AMENDED: the payment addresses are minted caller-side, but
PER-GM and DISTINCT — one fresh treasury address per GM, never a single shared
caller address.  What CHANGES is where the win is reflected (a caller-side
per-GM address, not the GM's wallet); what is PRESERVED is the function that
mattered — you can still say exactly which GM won, by looking the address up in
the registration map, which is recorded in registration-N*.json as
`ptx_payment_addr` per label.  A single shared address would have collapsed
per-GM attribution and is explicitly rejected.
"""

import time
import json
from typing import Dict, List, Optional
from .cluster import W2Cluster
from .node import Node, RPCError

# CPTXBeaTestNetParams (chainparams.cpp:848-930) — unchanged 801c557->HEAD
POW_BLOCKS = 49            # PoS activates at 50
POS_ACTIVATION = 50
GM_COLLATERAL = 100        # HMS (nGMCollateralAmt, chainparams.cpp:872)
GM_MIN_CONF = 1            # nGMCollateralMinConf (chainparams.cpp:875)
REG_BATCH = 10             # confirm-every-K registration pacing (W2.0a plan §5)
CALLER_SPLIT_AMOUNT = 2.0
CALLER_SPLIT_COUNT = 500
# ★ PRODUCER-SET STAKE (Phase 2).  Each NON-treasury caller gets its own
# stakeable coin.  Sizes chosen so every producer carries real weight rather
# than a token balance: staking probability is stake-weighted, so a producer
# funded with dust is a producer on paper only.  Many mid-size UTXOs beat one
# large one — each is an independent stake attempt, and nStakeMinDepth=20 /
# nCoinbaseMaturity=10 (ptxbea) are both cleared long before the fleet reaches
# its first settlement boundary.
PRODUCER_UTXO_COUNT = 20
PRODUCER_UTXO_AMOUNT = 500.0


def wait_for_height(node: Node, target: int, timeout: int = 900) -> int:
    return node.wait_for_height(target, timeout=timeout)


def mine_pow_blocks(treasury: Node, n: int = POW_BLOCKS) -> str:
    """★ Runs on the TREASURY: generatetoaddress is wallet-gated
    (EnsureWalletIsAvailable, mining.cpp:155) even though it takes an explicit
    address, so a wallet-less GM cannot mine the PoW prefix."""
    addr = treasury.getnewaddress()
    print(f"[bootstrap] mining {n} PoW blocks to {addr} (treasury={treasury.name})")
    t0 = time.time()
    treasury.generatetoaddress(n, addr)
    print(f"[bootstrap] {n} PoW blocks in {time.time()-t0:.1f}s; "
          f"height={treasury.getblockcount()}")
    return addr


def wait_for_pos(treasury: Node, timeout: int = 900) -> int:
    """PoS activates at POS_ACTIVATION. ★ Under the wallet-less-GM topology the
    treasury caller is the ONLY node that can stake, so it alone carries the
    chain past this point — it must be launched with -staking=1 and hold the
    PoW coinbases (mature at nCoinbaseMaturity=10, stakeable at
    nStakeMinDepth=20, both satisfied by the 49-block prefix)."""
    print(f"[bootstrap] waiting for PoS activation at {POS_ACTIVATION}")
    wait_for_height(treasury, POS_ACTIVATION, timeout=timeout)
    h = wait_for_height(treasury, POS_ACTIVATION + 1, timeout=timeout)
    print(f"[bootstrap] PoS active; height={h}")
    return h


def fund_caller(treasury: Node, caller: Node,
                split_count: int = CALLER_SPLIT_COUNT,
                amount_each: float = CALLER_SPLIT_AMOUNT) -> str:
    """Split coin into many small UTXOs so ptx_roll always has a fee input.

    ★ When treasury IS the caller (the default Phase-2 topology) this is a
    SELF-SPLIT: the treasury sends to its own fresh addresses. That is not a
    no-op — the point was never to move value between nodes, it was to break
    one huge coinbase UTXO into `split_count` spendable pieces."""
    same = (treasury.name == caller.name)
    print(f"[bootstrap] {'self-splitting treasury' if same else 'funding caller'} "
          f"into {split_count}x{amount_each} HMS UTXOs")
    amounts = {}
    for _ in range(split_count):
        amounts[caller.getnewaddress()] = amount_each
    txid = treasury.sendmany("", amounts)
    height = treasury.getblockcount()
    wait_for_height(treasury, height + 1, timeout=900)
    wait_for_height(caller, height + 1, timeout=120)
    print(f"[bootstrap] caller funded: {len(caller.listunspent(1))} UTXOs")
    return txid


def fund_producer_set(treasury: Node, producers: List[Node],
                      utxos: int = PRODUCER_UTXO_COUNT,
                      amount: float = PRODUCER_UTXO_AMOUNT) -> Optional[str]:
    """★ Fund every NON-treasury caller so the producer set is REAL.

    THE FOOTGUN THIS CLOSES.  Before Phase 2 the recipe funded only the primary
    caller, and that was harmless: 98 wallet-holding GMs could stake, so block
    production never depended on callers.  Under wallet-less GMs no GM can
    stake, so the callers ARE the producer set — and funding only caller1 leaves
    a fleet that reports 8 producers and has ONE.  Observed exactly that on the
    first Phase-2 rebuild: callers 2-8 at 0.0 HMS with -staking=1 inert, so a
    single caller death would have halted the chain, with the compose and the
    container list both claiming eight-way redundancy.

    Funding is what ACTIVATES -staking=1; the flag alone does nothing. This step
    is therefore not an optional extra — it is the second half of
    `--caller-staking 1`, and gen_fleet's no-producer guard has a blind spot
    without it (it can check the flag, not the balance).

    No-op (returns None) when there is only a treasury, which keeps the
    single-caller fixtures working unchanged.
    """
    others = [c for c in producers if c.name != treasury.name]
    if not others:
        print("[bootstrap] producer set = treasury only; no extra funding needed")
        return None
    amounts = {}
    for c in others:
        for _ in range(utxos):
            amounts[c.getnewaddress()] = amount
    total = len(amounts) * amount
    print(f"[bootstrap] funding producer set: {len(others)} caller(s) x {utxos} "
          f"x {amount} HMS = {total} HMS total")
    txid = treasury.sendmany("", amounts)
    height = treasury.getblockcount()
    wait_for_height(treasury, height + 1, timeout=900)
    for c in others:
        wait_for_height(c, height + 1, timeout=300)
    for c in others:
        print(f"[bootstrap]   {c.name}: {len(c.listunspent(1))} UTXO(s), "
              f"balance {c.getbalance()}")
    return txid


def _find_vout(treasury: Node, txid: str, addr: str, amount: float) -> int:
    raw = treasury.call("getrawtransaction", txid, True)
    for vout in raw["vout"]:
        addrs = vout.get("scriptPubKey", {}).get("addresses", [])
        if addr in addrs and abs(vout["value"] - amount) < 1e-6:
            return vout["n"]
    raise RuntimeError(f"collateral output {amount} to {addr} not in {txid}")


def register_gms(treasury: Node, cluster: W2Cluster,
                 gm_labels: List[str]) -> Dict[str, dict]:
    """External-collateral protx_register for each GM, batched.

    KDD-033: suffix = hash(collateralOutpoint) fingerprints the specific
    collateral UTXO; requires a stable external outpoint known before the
    registration tx is built (self-funded registration is circular — see
    origin bootstrap.py:98-109).

    ★ Every wallet RPC below runs on the TREASURY (a caller). GMs are
    -disablewallet=1 and would answer "Method not found (disabled)".
    protx_register in particular is wallet-bound: it funds and signs the
    registration tx from the wallet (ProTxRegister -> EnsureWalletIsAvailable,
    rpcevo.cpp:490-494). generateblskeypair is pure keygen and node-agnostic,
    but is kept on the treasury so the operator secrets are produced in one
    place and recorded in the registration map.
    """
    collateral_amt = float(GM_COLLATERAL)
    n_gms = len(gm_labels)

    print(f"[bootstrap] funding {n_gms} collateral outputs ({collateral_amt} HMS each) "
          f"from treasury={treasury.name}")
    collateral_addrs = [treasury.getnewaddress() for _ in range(n_gms)]
    funding_txid = treasury.sendmany("", {a: collateral_amt for a in collateral_addrs})
    print(f"[bootstrap] collateral funding tx: {funding_txid}")

    # ★ LOCK FIRST, CONFIRM SECOND (2026-07-30).  These outputs become stakeable
    # at nStakeMinDepth (20 blocks on ptxbea) and the TREASURY IS the staker, so
    # every block between funding and locking is a chance for the staker to eat
    # a future collateral.  Locking after the confirmation wait means winning a
    # >20-block race; under load (a concurrent build starving RPC) that race was
    # LOST — the staker consumed 5 of 98 collaterals at h189-h205 and
    # lockunspent then failed with "expected unspent output", killing the
    # bootstrap.  The outputs are in the wallet the moment sendmany returns, so
    # lock them immediately: the window closes to zero instead of being raced.
    # Same family as BUG-019 R1 — anything not yet locked is fair game.
    # ★ Phase 2: this hazard did NOT go away when GMs lost their wallets — it
    # moved here with the collateral, because the treasury caller both holds the
    # collateral and stakes.  Wallet-less GMs remove the GM-side exposure only.
    collateral_outpoints = []
    for coll_addr in collateral_addrs:
        vout_n = _find_vout(treasury, funding_txid, coll_addr, collateral_amt)
        collateral_outpoints.append({"txid": funding_txid, "vout": vout_n})
    treasury.call("lockunspent", False, True, collateral_outpoints)
    print(f"[bootstrap] locked {len(collateral_outpoints)} collateral UTXOs "
          f"(pre-confirmation — closes the staker race)")

    current = treasury.getblockcount()
    wait_for_height(treasury, current + 1, timeout=900)

    def _funding_confirmed():
        try:
            return treasury.call("getrawtransaction", funding_txid, True).get(
                "confirmations", 0) >= 1
        except Exception:
            return False
    treasury.wait_for_condition(_funding_confirmed, "collateral funding confirmed",
                                timeout=120)

    # Re-assert after confirmation: locks are in-memory only, and a restart or
    # wallet reload between here and registration would drop them.
    # ★ lockunspent ERRORS with "output already locked" if ANY outpoint in the
    # batch is already held, so re-locking the whole set unconditionally fails
    # on the happy path.  Lock only what is actually missing — the same idiom
    # relock_collaterals() uses (cluster.py) — then assert the full set.
    locked_now = {(l["txid"], l["vout"])
                  for l in treasury.call("listlockunspent")["transparent"]}
    todo = [o for o in collateral_outpoints
            if (o["txid"], o["vout"]) not in locked_now]
    if todo:
        treasury.call("lockunspent", False, True, todo)
        print(f"[bootstrap] re-locked {len(todo)} collateral(s) dropped since funding")
    locked_now = {(l["txid"], l["vout"])
                  for l in treasury.call("listlockunspent")["transparent"]}
    missing = [o for o in collateral_outpoints
               if (o["txid"], o["vout"]) not in locked_now]
    if missing:
        raise AssertionError(
            f"collateral lock incomplete: {len(missing)}/{len(collateral_outpoints)} "
            f"not in listlockunspent — the staker may have consumed them")

    results = {}
    for i, (gm, label, outpoint) in enumerate(
            zip(cluster.gms, gm_labels, collateral_outpoints)):
        ip_port = f"{cluster.subnet_base}.{11 + i}:29994"
        owner_addr  = treasury.getnewaddress()
        voting_addr = treasury.getnewaddress()
        payout_addr = treasury.getnewaddress()
        # ★ KDD-035 AS AMENDED (Phase 2): scriptPTXPayment is minted on the
        # TREASURY, not on the GM — a wallet-less GM has no wallet to mint from,
        # so the original "the winning GM's wallet reflects the win" is not
        # implementable under this topology.  A FRESH address PER GM keeps the
        # property that actually mattered: attribution stays per-GM and is
        # resolved by looking the address up in the registration map below
        # (persisted to registration-N*.json as ptx_payment_addr).  Reusing one
        # shared caller address here would collapse that and is rejected.
        ptx_pay_addr = treasury.getnewaddress()

        bls = treasury.bls_generate()
        print(f"[bootstrap] registering {label} at {ip_port} "
              f"({i+1}/{n_gms}, collateral {funding_txid[:12]}:{outpoint['vout']})")
        reg = treasury.call(
            "protx_register",
            funding_txid, outpoint["vout"], ip_port,
            owner_addr, bls["public"], voting_addr, payout_addr,
            0, "", ptx_pay_addr, label
        )
        if isinstance(reg, dict):
            compound   = reg.get("ptxNodeId", label)
            protx_hash = reg.get("txid", "")
        else:
            compound   = label
            protx_hash = str(reg)
        results[label] = {
            "compound_node_id": compound,
            "ptx_payment_addr": ptx_pay_addr,
            "protx_hash": protx_hash,
            "operator_pubkey": bls["public"],
            "operator_secret": bls["secret"],
        }
        print(f"[bootstrap] {label} -> {compound} protx={protx_hash[:16]}...")

        # Amendment-1 pacing: flush the mempool every REG_BATCH registrations
        # so N=60 never stacks the full set as one unconfirmed chain.
        if (i + 1) % REG_BATCH == 0 and (i + 1) < n_gms:
            h = treasury.getblockcount()
            print(f"[bootstrap] batch of {REG_BATCH} done — waiting 1 block")
            wait_for_height(treasury, h + 1, timeout=900)

    return results


def wait_for_dgm_stability(observer: Node, expected: int, timeout: int = 900) -> None:
    """protx_list is wallet-SAFE (rpcevo.cpp:868-880 tolerates pwallet==nullptr;
    only wallet_only=true throws), so `observer` may be any node — but the
    recipes pass the treasury so registration and observation share one view."""
    print(f"[bootstrap] waiting for {expected} GMs in DGM list")

    def check():
        try:
            return len(observer.protx_list(detailed=False, valid_only=True)) >= expected
        except RPCError:
            return False
    observer.wait_for_condition(check, f"DGM list has {expected}", timeout=timeout)


def wait_for_gm_confirmation(observer: Node, expected: int, timeout: int = 900) -> None:
    """Amendment 1 (the reconcile's core): wait until EVERY GM is confirmed —
    tip >= registeredHeight + GM_MIN_CONF + 1, the point confirmedHash is set
    (deterministicgms.cpp:597-600) and the GM becomes visible to
    CalculateScores/KDD-060 selection. Registration alone is NOT eligibility."""
    print(f"[bootstrap] waiting for confirmedHash on all {expected} GMs")

    def check():
        try:
            lst = observer.protx_list(detailed=True, valid_only=True)
            if len(lst) < expected:
                return False
            tip = observer.getblockcount()
            return all(
                tip >= e["dgmstate"]["registeredHeight"] + GM_MIN_CONF + 1
                for e in lst)
        except (RPCError, KeyError):
            return False
    observer.wait_for_condition(check, "all GMs confirmation-deep", timeout=timeout)
    print(f"[bootstrap] all {expected} GMs confirmation-deep at "
          f"tip={observer.getblockcount()}")


def assert_all_eligible(observer: Node, expected: int) -> None:
    """KDD-060 leg check from exposed state: every registered GM carries a
    compound node_id (ptxNodeId 'label:8hex'). The end-to-end selection proof
    (validate_fleet.eligibility_gate) complements this via the real V5 core."""
    lst = observer.protx_list(detailed=True, valid_only=True)
    assert len(lst) == expected, f"DGM list {len(lst)} != {expected}"
    bad = []
    for e in lst:
        nid = e.get("dgmstate", {}).get("ptxNodeId", "")
        if ":" not in nid or len(nid.split(":")[1]) != 8:
            bad.append((e.get("proTxHash", "?")[:12], nid))
    assert not bad, f"GMs without compound node_id: {bad}"
    print(f"[bootstrap] all {expected} GMs carry compound ptxNodeId")


def build_phase2_env(registration: Dict[str, dict]) -> dict:
    env = {}
    for label, info in registration.items():
        env[label.upper().replace("-", "_") + "_NODE_ID"] = info["compound_node_id"]
    return env


def rewrite_env(cluster: W2Cluster, env_overrides: dict, env_path: str):
    lines = [f"RPCUSER={cluster.rpc_user}", f"RPCPASSWORD={cluster.rpc_pass}",
             "CALLER_NODE_ID=caller"]
    for k, v in sorted(env_overrides.items()):
        lines.append(f"{k}={v}")
    with open(env_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def bootstrap(cluster: W2Cluster,
              env_path: str,
              gm_labels: Optional[List[str]] = None,
              phase1_warmup_blocks: int = 5,
              fund_caller_utxos: int = CALLER_SPLIT_COUNT) -> Dict[str, dict]:
    """Full two-phase bootstrap at N = cluster.n."""
    if gm_labels is None:
        gm_labels = [f"gm{i:02d}" for i in range(1, cluster.n + 1)]
    assert len(gm_labels) == cluster.n

    # ★ Phase 2 topology: the treasury is a CALLER, not gm01.  Every wallet RPC
    # in this recipe runs here; GMs are -disablewallet=1 and answer wallet RPCs
    # with "Method not found (disabled)".  cluster.treasury defaults to the
    # primary caller and is overridable so a spare can take the role.
    treasury = cluster.treasury
    caller = cluster.caller

    print(f"[bootstrap] === Phase 1 (N={cluster.n}): PoW + funding + ProRegPL ===")
    print(f"[bootstrap] treasury={treasury.name} (wallet-holder; GMs are wallet-less)")
    mine_pow_blocks(treasury, POW_BLOCKS)
    wait_for_pos(treasury)
    for node in cluster.all_nodes:
        wait_for_height(node, POS_ACTIVATION, timeout=300)

    current = treasury.getblockcount()
    wait_for_height(treasury, current + phase1_warmup_blocks, timeout=4500)

    if fund_caller_utxos:
        fund_caller(treasury, caller, split_count=fund_caller_utxos)

    # ★ Activate the rest of the producer set BEFORE registration, so their coin
    # has the longest possible run-up to nStakeMinDepth=20 and they are eligible
    # to produce by the time formation starts. Ordering matters: fund late and
    # the fleet spends its first boundaries single-producer anyway.
    fund_producer_set(treasury, cluster.callers)

    registration = register_gms(treasury, cluster, gm_labels)

    current = treasury.getblockcount()
    wait_for_height(treasury, current + 1, timeout=900)
    wait_for_dgm_stability(treasury, len(gm_labels))
    wait_for_gm_confirmation(treasury, len(gm_labels))   # Amendment 1
    assert_all_eligible(treasury, len(gm_labels))        # Amendment 1

    print("[bootstrap] === Phase 2: restart with compound node_ids ===")
    env_overrides = build_phase2_env(registration)
    print(f"[bootstrap] compound ids: {json.dumps(env_overrides, indent=1)}")
    rewrite_env(cluster, env_overrides, env_path)
    cluster.restart_with_env()
    cluster.wait_ready()

    tip = max(nd.getblockcount() for nd in cluster.all_nodes if nd.is_rpc_ready())
    for node in cluster.all_nodes:
        wait_for_height(node, tip, timeout=300)

    def caller_p2p_ready():
        try:
            return len(cluster.caller.call("getpeerinfo")) >= 1
        except Exception:
            return False
    cluster.caller.wait_for_condition(caller_p2p_ready,
                                      "caller has >=1 P2P peer", timeout=120)
    print("[bootstrap] === bootstrap complete ===")
    return registration
