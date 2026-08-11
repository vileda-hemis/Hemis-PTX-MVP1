#!/usr/bin/env python3
"""GATE 5 grow: +30 the standing ptx-w2r fleet (150 -> 180), self-arming via the
arm-fix (gen_fleet --registration re-wire). SAFE by construction:
  - register_gms is TREASURY-SIDE (no new containers needed to register);
  - the compose regen re-emits gm1-150 armed + UNCHANGED except addnode hints,
    and we only ever `up -d gm151..180`, so the standing GMs are NEVER recreated;
  - the +30 come up ARMED with EMPTY datadirs and network-IBD the clean chain.

Two phases so the treasury-side register (safe) checkpoints before the docker
bring-up (host-port-race prone; cluster.up now retries):
  python3 grow_fleet.py register     # phase 1: register + merge full-180 reg
  python3 grow_fleet.py bringup      # phase 2: regen armed compose + up new 30
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from harness.cluster import W2Cluster
from harness import bootstrap as bs

OUT      = "/mnt/pve/Node14TB/hemis-ptx/docker-w2r"
COMPOSE  = f"{OUT}/docker-compose.generated.yml"
ENV      = f"{OUT}/.env"
BOOT_REG = "/mnt/ptx-ssd-work/w2r-fleet/registration.json"      # gm1-150
FULL_REG = "/mnt/ptx-ssd-work/w2r-fleet/registration-180.json"  # gm1-180 (written here)
IMAGE    = "ptx-w2:roll-4fad27e9"
V6_PREFIX = "fd00:32::"
NEW = list(range(151, 181))
LABELS = [f"gm{i:02d}" for i in NEW]   # gm151..gm180


def cluster():
    return W2Cluster(150, compose_file=COMPOSE, project="ptx-w2r",
                     port_base=32000, subnet_base="172.32.0", callers=8)


def phase_register():
    c = cluster()
    treasury = c.treasury
    print(f"[grow] registering {LABELS[0]}..{LABELS[-1]} (treasury={treasury.name}, "
          f"start_idx=151, mixed_v6, {V6_PREFIX})")
    new_reg = bs.register_gms(treasury, c, LABELS, mixed_v6=True,
                              v6_prefix=V6_PREFIX, start_idx=151)
    if not new_reg:
        print("[grow] register returned empty (already registered?) — "
              "reusing existing on-chain set for the merge")
    # confirm 180 in the DGM list before proceeding
    cur = treasury.getblockcount()
    bs.wait_for_height(treasury, cur + 1, timeout=900)
    bs.wait_for_dgm_stability(treasury, 180)
    bs.wait_for_gm_confirmation(treasury, 180)
    # merge gm1-150 (bootstrap) + new 30 -> full 180 registration
    with open(BOOT_REG) as f:
        full = json.load(f)
    full.update(new_reg)
    with open(FULL_REG, "w") as f:
        json.dump(full, f, indent=1)
    os.chmod(FULL_REG, 0o600)
    print(f"[grow] full registration written: {len(full)} entries -> {FULL_REG}")
    print("[grow] PHASE 1 (register) COMPLETE — run: grow_fleet.py bringup")


def phase_bringup():
    if not os.path.exists(FULL_REG):
        sys.exit("[grow] no full-180 registration — run phase 'register' first")
    # regen the compose ARMED for n=180 (the arm re-wire). gm1-150 stay armed;
    # only addnode hints drift, and they're never applied (we target new only).
    print("[grow] regenerating armed compose for n=180 (gen_fleet --registration)")
    subprocess.run(["python3", f"{HERE}/gen_fleet.py",
                    "--n", "180", "--callers", "8", "--caller-staking", "1",
                    "--mixed-v6", "--v6-prefix", V6_PREFIX,
                    "--project", "ptx-w2r", "--port-base", "32000",
                    "--subnet-base", "172.32.0",
                    "--data-root", "/mnt/ptx-ssd-work/w2r-fleet/datadirs",
                    "--out", OUT, "--image", IMAGE,
                    "--registration", FULL_REG], check=True)
    # bring up ONLY the new 30 (standing gm1-150 never targeted -> never recreated)
    services = [f"gm{i:02d}" for i in NEW]
    print(f"[grow] up -d {services[0]}..{services[-1]} (target new only; retry the "
          f"host-port race)")
    for attempt in range(1, 8):
        r = subprocess.run(["docker", "compose", "-f", COMPOSE, "up", "-d", *services],
                           capture_output=True, text=True)
        up = int(subprocess.run(
            ["bash", "-c", "docker ps --filter name=ptx-w2r-gm1 --filter name=ptx-w2r-gm18 "
             "--format '{{.Names}}' | grep -cE 'gm1(5[1-9]|[6-7][0-9]|80)' || true"],
            capture_output=True, text=True).stdout.strip() or 0)
        if r.returncode == 0:
            print(f"[grow] compose up OK on attempt {attempt}")
            break
        print(f"[grow] up attempt {attempt}/7 hit the port race — retrying")
    # ★ ACTIVATE: the new GMs boot ARMED but with EMPTY datadirs, so their
    # init-time activation ran against an empty chain and activeGamemasterManager
    # never picked up their proTx ("Active gamemaster not ready"). Once synced,
    # a plain `docker restart` (no port rebind, no race) re-inits with the proTx
    # present -> they become active gamemasters ("status":"Ready"). Wait for sync
    # first so the restart lands on a populated chain.
    import time, glob, re
    DD = "/mnt/ptx-ssd-work/w2r-fleet/datadirs"
    def tip():
        return int(subprocess.run(
            ["docker", "exec", "ptx-w2r-caller1", "Hemis-cli",
             "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026",
             "getblockcount"], capture_output=True, text=True).stdout.strip() or 0)
    def synced_count(t):
        n = 0
        for i in NEW:
            try:
                hs = [int(x) for x in re.findall(r"height=(\d+)",
                      open(f"{DD}/gm{i:02d}/ptxbea/debug.log").read())]
                if hs and max(hs) >= t - 5:
                    n += 1
            except OSError:
                pass
        return n
    print("[grow] waiting for the new 30 to network-IBD before the activate restart")
    for _ in range(90):
        t = tip()
        if synced_count(t) >= len(NEW):
            break
        time.sleep(10)
    names = [f"ptx-w2r-gm{i:02d}" for i in NEW]
    print(f"[grow] ACTIVATE: docker restart the synced new 30 "
          f"(inits activeGamemasterManager with their proTx)")
    subprocess.run(["docker", "restart", *names], check=False)
    print("[grow] PHASE 2 (bringup+activate) done — new 30 ARMED, network-IBD'd, "
          "and restarted to ACTIVE. Verify split/Ready/quorum-width next.")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "register":
        phase_register()
    elif mode == "bringup":
        phase_bringup()
    else:
        sys.exit("usage: grow_fleet.py register|bringup")
