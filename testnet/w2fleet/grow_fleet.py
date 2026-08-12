#!/usr/bin/env python3
"""GATE 5 grow: expand the standing ptx-w2r fleet by a batch, self-arming via the
arm-fix (gen_fleet --registration re-wire). SAFE by construction:
  - register_gms is TREASURY-SIDE (no new containers needed to register);
  - the compose regen re-emits gm1..prev armed + UNCHANGED except addnode hints,
    and we only ever `up -d` the NEW range, so standing GMs are NEVER recreated;
  - the new GMs come up ARMED with EMPTY datadirs, network-IBD the clean chain,
    then a post-sync `docker restart` activates them (arm-before-sync).
  - caller ports are STABLE (gen_fleet CALLER_PORT_OFFSET) so a grow never collides.

Two phases so the treasury-side register (safe) checkpoints before the docker
bring-up (host-port-race prone; retried here):
  python3 grow_fleet.py register <from> <to>   # register + merge full-<to> reg
  python3 grow_fleet.py bringup  <from> <to>   # regen armed + up new + activate
<from>..<to> is the NEW GM index range; <to> is the new fleet total N.
"""
import json, os, subprocess, sys, time, re

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from harness.cluster import W2Cluster
from harness import bootstrap as bs

OUT      = "/mnt/pve/Node14TB/hemis-ptx/docker-w2r"
COMPOSE  = f"{OUT}/docker-compose.generated.yml"
REGDIR   = "/mnt/ptx-ssd-work/w2r-fleet"
DATADIRS = f"{REGDIR}/datadirs"
IMAGE    = "ptx-w2:roll-4fad27e9"
V6_PREFIX = "fd00:32::"


def full_reg_path(n): return f"{REGDIR}/registration-{n}.json"
def labels(a, b):     return [f"gm{i:02d}" for i in range(a, b + 1)]


def phase_register(new_from, new_to):
    prev = full_reg_path(new_from - 1)
    if not os.path.exists(prev):
        sys.exit(f"[grow] missing previous full registration {prev} — grow in order")
    c = W2Cluster(new_from - 1, compose_file=COMPOSE, project="ptx-w2r",
                  port_base=32000, subnet_base="172.32.0", callers=8)
    treasury = c.treasury
    lbls = labels(new_from, new_to)
    print(f"[grow] registering {lbls[0]}..{lbls[-1]} (treasury={treasury.name}, "
          f"start_idx={new_from}, mixed_v6, {V6_PREFIX})")
    new_reg = bs.register_gms(treasury, c, lbls, mixed_v6=True,
                              v6_prefix=V6_PREFIX, start_idx=new_from)
    cur = treasury.getblockcount()
    bs.wait_for_height(treasury, cur + 1, timeout=900)
    bs.wait_for_dgm_stability(treasury, new_to)
    bs.wait_for_gm_confirmation(treasury, new_to)
    with open(prev) as f:
        full = json.load(f)
    full.update(new_reg)
    with open(full_reg_path(new_to), "w") as f:
        json.dump(full, f, indent=1)
    os.chmod(full_reg_path(new_to), 0o600)
    print(f"[grow] full registration: {len(full)} entries -> {full_reg_path(new_to)}")
    print(f"[grow] PHASE 1 COMPLETE — run: grow_fleet.py bringup {new_from} {new_to}")


def _tip():
    return int(subprocess.run(
        ["docker", "exec", "ptx-w2r-caller1", "Hemis-cli", "-ptxbea",
         "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026", "getblockcount"],
        capture_output=True, text=True).stdout.strip() or 0)


def phase_bringup(new_from, new_to):
    reg = full_reg_path(new_to)
    if not os.path.exists(reg):
        sys.exit(f"[grow] no {reg} — run 'register {new_from} {new_to}' first")
    print(f"[grow] regenerating armed compose for n={new_to} (gen_fleet --registration)")
    subprocess.run(["python3", f"{HERE}/gen_fleet.py",
                    "--n", str(new_to), "--callers", "8", "--caller-staking", "1",
                    "--mixed-v6", "--v6-prefix", V6_PREFIX,
                    "--project", "ptx-w2r", "--port-base", "32000",
                    "--subnet-base", "172.32.0", "--data-root", DATADIRS,
                    "--out", OUT, "--image", IMAGE, "--registration", reg], check=True)
    services = [f"gm{i:02d}" for i in range(new_from, new_to + 1)]
    print(f"[grow] up -d {services[0]}..{services[-1]} (new only; retry the port race)")
    for attempt in range(1, 8):
        r = subprocess.run(["docker", "compose", "-f", COMPOSE, "up", "-d", *services],
                           capture_output=True, text=True)
        if r.returncode == 0:
            print(f"[grow] compose up OK on attempt {attempt}")
            break
        print(f"[grow] up attempt {attempt}/7 hit the port race — retrying")
        time.sleep(5)
    # ACTIVATE: armed-at-empty-datadir boot misses proTx ('not ready'); once synced
    # a plain restart re-inits activeGamemasterManager -> 'Ready'.
    rng = list(range(new_from, new_to + 1))
    def synced(t):
        n = 0
        for i in rng:
            try:
                hs = [int(x) for x in re.findall(r"height=(\d+)",
                      open(f"{DATADIRS}/gm{i:02d}/ptxbea/debug.log").read())]
                if hs and max(hs) >= t - 5:
                    n += 1
            except OSError:
                pass
        return n
    print("[grow] waiting for the new range to network-IBD before the activate restart")
    for _ in range(120):
        if synced(_tip()) >= len(rng):
            break
        time.sleep(10)
    names = [f"ptx-w2r-gm{i:02d}" for i in rng]
    print("[grow] ACTIVATE: docker restart the synced new range")
    subprocess.run(["docker", "restart", *names], check=False)
    print(f"[grow] PHASE 2 COMPLETE — {services[0]}..{services[-1]} ARMED + IBD'd + "
          f"ACTIVE. Verify split/Ready/quorum-width.")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: grow_fleet.py register|bringup|grow <from> <to>")
    mode, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    if mode == "register":
        phase_register(a, b)
    elif mode == "bringup":
        phase_bringup(a, b)
    elif mode == "grow":            # both phases, one process (no self-matching waiter)
        phase_register(a, b)
        phase_bringup(a, b)
    else:
        sys.exit("usage: register|bringup|grow <from> <to>")
