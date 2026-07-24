#!/usr/bin/env python3
"""Drive a full ptx-w2 bootstrap at N (W2.0a). Run from testnet/w2fleet/.

  python3 run_bootstrap.py --n 22

Stages: up -> bootstrap (PoW/PoS/fund/register/confirm/phase-2) -> topology
gate -> eligibility gate. PoC-untouched asserted throughout (cluster guard).
Banking is a separate explicit step (bank_fleet.sh) after a clean stop.
"""

import argparse
import json
import os
import sys

from harness.cluster import W2Cluster, DEF_COMPOSE
from harness import bootstrap as bs
import validate_fleet as vf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--compose", default=DEF_COMPOSE)
    ap.add_argument("--env", default="/mnt/pve/Node14TB/hemis-ptx/docker-w2/.env")
    ap.add_argument("--project", default="ptx-w2",
                    help="compose project (use e.g. bfleet for a 2nd isolated instance)")
    ap.add_argument("--port-base", type=int, default=31000)
    ap.add_argument("--subnet-base", default="172.31.0")
    ap.add_argument("--fund-caller-utxos", type=int, default=500)
    ap.add_argument("--reg-out", default=None,
                    help="write registration dict (incl. operator secrets) here")
    args = ap.parse_args()

    c = W2Cluster(args.n, compose_file=args.compose, project=args.project,
                  port_base=args.port_base, subnet_base=args.subnet_base)
    c.up()
    c.wait_ready()
    print(f"[run] fleet up: {args.n} GMs + caller")

    reg = bs.bootstrap(c, env_path=args.env,
                       fund_caller_utxos=args.fund_caller_utxos)
    if args.reg_out:
        with open(args.reg_out, "w") as f:
            json.dump(reg, f, indent=1)
        os.chmod(args.reg_out, 0o600)
        print(f"[run] registration record -> {args.reg_out}")

    vf.topology_gate(c)
    vf.eligibility_gate(c, args.n)
    c.assert_poc_untouched("run_bootstrap end")
    print("[run] BOOTSTRAP + GATES COMPLETE — fleet is formation-ready; "
          "bank with: compose -p ptx-w2 stop && bank_fleet.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
