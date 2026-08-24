#!/usr/bin/env python3
"""Harvest real PTX payloads from a node YOU OWN, as decoder/verifier fixtures.

★ This is a TEST tool, not part of the page's data path. The page reaches into
no node it does not own; this script is how a developer collects known-good
payloads to prove the PTX_MapBeacon port reproduces the chain exactly.

  python3 harvest_fixtures.py --rpc http://127.0.0.1:29995 --user U --password P \
      [--from H] [--count 12] [--out fixtures.json]

★ Aim it at a range where rolls actually happened. A window with no PTXSESS in
it yields a fixture file that makes every test pass by having nothing to check
-- the vacuity trap, applied to your own test data. This refuses to write one.
"""
import argparse
import base64
import json
import sys
import urllib.request


def make_rpc(url, user, pw):
    def call(method, params):
        body = json.dumps({"jsonrpc": "1.0", "id": "h", "method": method,
                           "params": params}).encode()
        req = urllib.request.Request(url, data=body, headers={
            "Content-Type": "application/json",
            "Authorization": "Basic " + base64.b64encode(
                ("%s:%s" % (user, pw)).encode()).decode()})
        with urllib.request.urlopen(req, timeout=30) as r:
            d = json.loads(r.read())
        if d.get("error"):
            raise RuntimeError(d["error"])
        return d["result"]
    return call


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--from", dest="start", type=int, default=0, help="0 = chain tip")
    ap.add_argument("--count", type=int, default=12, help="blocks to walk back")
    ap.add_argument("--out", default="fixtures.json")
    a = ap.parse_args()

    rpc = make_rpc(a.rpc, a.user, a.password)
    tip = a.start or rpc("getblockcount", [])
    fx = []
    for h in range(tip, max(0, tip - a.count), -1):
        b = rpc("getblock", [rpc("getblockhash", [h]), 2])
        for tx in b.get("tx", []):
            if tx.get("type") in (6, 12) and tx.get("extraPayload"):
                fx.append({"height": h, "type": tx["type"], "txid": tx["txid"],
                           "payload": tx["extraPayload"], "blockhash": b["hash"],
                           "time": b.get("time")})
    n6 = sum(1 for f in fx if f["type"] == 6)
    n12 = sum(1 for f in fx if f["type"] == 12)
    print("harvested %d payload(s): PTXSESS=%d ROLLCOMMIT=%d from h%d..h%d"
          % (len(fx), n6, n12, max(0, tip - a.count) + 1, tip))
    if not n6:
        print("REFUSING TO WRITE: no PTXSESS in that window. Checks B and C would have "
              "nothing to verify and the suite would pass vacuously. Pick a range with rolls.",
              file=sys.stderr)
        return 1
    with open(a.out, "w") as fh:
        json.dump(fx, fh)
    print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
