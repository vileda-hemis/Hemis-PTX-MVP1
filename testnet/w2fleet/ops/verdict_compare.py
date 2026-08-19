#!/usr/bin/env python3
"""BUG-036 lesson: same tip-hash and same canonicity are different properties.
Compare VERDICT-LEVEL state between two nodes: canonicalized quorum records
(list + full per-quorum info), pose status, and lottery status.  Exit 0 only
if all three match byte-for-byte after canonical JSON dump."""
import json, subprocess, sys, hashlib

CLI = ["Hemis-cli", "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026"]

def rpc(node, method, *params):
    cmd = ["docker", "exec", f"ptx-w2r-{node}"] + CLI + [method] + [str(p) for p in params]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        return None
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return out.stdout.strip()

def curl_rpc(node, method, params="[]"):
    cmd = ["docker", "exec", f"ptx-w2r-{node}", "curl", "-s", "-u",
           "ptxw2rpc:ptxw2pass2026", "-d",
           f'{{"jsonrpc":"1.0","id":"v","method":"{method}","params":{params}}}',
           "http://127.0.0.1:29903/"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    try:
        return json.loads(out.stdout).get("result")
    except Exception:
        return None

def verdict_state(node):
    ql = curl_rpc(node, "ptx_quorum_list") or {}
    quorums = {}
    for q in ql.get("quorums", []):
        qh = q["quorum_hash"]
        quorums[qh] = curl_rpc(node, "ptx_quorum_info", f'["{qh}"]')
    pose = rpc(node, "ptx_pose_status")
    lottery = rpc(node, "ptx_lottery_status")
    return {"quorum_list": ql, "quorum_info": quorums,
            "pose": pose, "lottery": lottery}

def canon_hash(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()

a, b = sys.argv[1], sys.argv[2]
sa, sb = verdict_state(a), verdict_state(b)
ok = True
for key in ("quorum_list", "quorum_info", "pose", "lottery"):
    ha, hb = canon_hash(sa[key]), canon_hash(sb[key])
    match = "MATCH" if ha == hb else "DIVERGE"
    if ha != hb:
        ok = False
    print(f"{key:12} {a}={ha[:12]} {b}={hb[:12]}  {match}")
print("VERDICT-LEVEL:", "AGREE" if ok else "DIVERGENT (BUG-036 class)")
sys.exit(0 if ok else 1)
