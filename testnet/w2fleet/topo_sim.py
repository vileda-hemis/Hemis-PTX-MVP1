#!/usr/bin/env python3
"""Relay-topology conversion simulator — VERIFICATION, not a guard (verdict-A).

Ceremony member-connection establishment (KDD-065) converts member<->member
normal edges into quorum-only relay-EXCLUDED channels (net.cpp:1934-40 +
CanRelay net.h:923). Verdict-A (2026-07-21): conversion churn is production-
safe via the inherited slot-refill machinery; on the TEST FLEET self-heal is
impossible by construction (peer discovery disabled + shared-/16 netgroup cap,
net.cpp:1732-47), compensated by the 11-addnode pigeonhole (gen_fleet SKIPS).
This tool VERIFIES the structural property holds on the live graph — it is
not a scaffold's guard.

Modes:
  topo_sim.py                          worst-case superset (drop ALL GM-GM
                                       edges) — diagnostic only; unusable as
                                       a gate on an all-GM fleet
  topo_sim.py --members gm01,gm05,...  exact conversion for a selected set
                                       (the boundary-time check: run when
                                       selectquorum names the 11)
  topo_sim.py --montecarlo [seed]      500 random 11-subsets, count partitions

Read-only: docker ps/inspect/exec getpeerinfo only.
"""
import json
import subprocess
import sys


def sh(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=60).stdout


def build_graph():
    names = [n for n in sh(["docker", "ps", "--format", "{{.Names}}"]).split()
             if n.startswith("ptx-w2-")]
    ip2name = {}
    for n in names:
        out = sh(["docker", "inspect", "-f",
                  "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", n]).strip()
        if out:
            ip2name[out] = n
    edges = set()
    for n in names:
        try:
            pi = json.loads(sh(["docker", "exec", n, "Hemis-cli", "-ptxbea",
                                "-datadir=/root/.hemis-ptxbea", "getpeerinfo"]))
        except Exception as e:
            print("WARN %s: %s" % (n, e))
            continue
        for p in pi:
            ip = p.get("addr", "").rsplit(":", 1)[0]
            peer = ip2name.get(ip)
            if peer and peer != n:
                edges.add(tuple(sorted((n, peer))))
    return names, edges


def components(nodes, es):
    adj = {n: set() for n in nodes}
    for a, b in es:
        adj[a].add(b)
        adj[b].add(a)
    seen, comps = set(), []
    for n in nodes:
        if n in seen:
            continue
        stack, comp = [n], set()
        while stack:
            x = stack.pop()
            if x in seen:
                continue
            seen.add(x)
            comp.add(x)
            stack.extend(adj[x] - seen)
        comps.append(comp)
    return comps


def main():
    names, edges = build_graph()
    gms = {n for n in names if "-gm" in n}

    if len(sys.argv) > 1 and sys.argv[1] == "--montecarlo":
        import random
        random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 42)
        trials, fails = 500, 0
        for _ in range(trials):
            mem = set(random.sample(sorted(gms), 11))
            kept = {e for e in edges if not (e[0] in mem and e[1] in mem)}
            if len(components(set(names), kept)) != 1:
                fails += 1
        print("MONTECARLO: %d random 11-subsets, partitioned=%d" % (trials, fails))
        sys.exit(0 if fails == 0 else 2)

    if len(sys.argv) > 2 and sys.argv[1] == "--members":
        members = {"ptx-w2-" + m for m in sys.argv[2].split(",")}
        kept = {e for e in edges if not (e[0] in members and e[1] in members)}
        label = "exact-set"
    else:
        kept = {e for e in edges if not (e[0] in gms and e[1] in gms)}
        label = "superset(all GM-GM) — diagnostic only"

    print("nodes=%d live-edges=%d converted=%d kept=%d [%s]"
          % (len(names), len(edges), len(edges) - len(kept), len(kept), label))
    comps = components(set(names), kept)
    sizes = sorted((len(c) for c in comps), reverse=True)
    print("post-conversion components=%d sizes=%s" % (len(comps), sizes))
    deg = {}
    for a, b in kept:
        deg[a] = deg.get(a, 0) + 1
        deg[b] = deg.get(b, 0) + 1
    zero = sorted(n for n in gms if deg.get(n, 0) == 0)
    print("VERDICT:", "PASS single-component" if len(comps) == 1 else "FAIL partitioned")
    if zero:
        print("nodes with 0 surviving normal edges:", zero)
    sys.exit(0 if len(comps) == 1 else 2)


if __name__ == "__main__":
    main()
