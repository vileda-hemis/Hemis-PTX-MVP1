#!/usr/bin/env python3
"""§13 — catch a TOTAL BLACKOUT in the act and answer the one question that splits
the cause in two: DID THE COMMITMENT EVER ENTER THE CALLER'S OWN MEMPOOL?

  never entered  -> build/accept failure BEFORE gossip is attempted (caller-side,
                    nothing to do with propagation at all)
  entered, went nowhere -> a relay question: it existed locally and did not spread

Reuses meshhop_probe's poller (t0 = the caller's OWN first-sight of the commitment,
which is exactly the discriminator) and adds a diagnostic snapshot around every roll
so a caught blackout arrives with its context instead of needing a re-run:
peers (in/out), banned list, mempool depth, tip height + seconds since the last
block (block-boundary correlation), and the roll's own error text.

Blackout rate at d200 measured ~2/72 (§12), so this hunts in batches and stops on
the first catch.  n=1 has been wrong twice this arc -- a catch is a LEAD, and the
script keeps hunting for a second instance when asked (--want N).
"""
import sys, os, json, time, statistics
sys.path.insert(0, "/mnt/pve/Node14TB/hemis-ptx/w2-fleet")
import meshhop_probe as M

OUT = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/blackout_events.jsonl"

CKPT = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/blackout_hunt.ckpt.json"

def ckpt_read():
    """Checkpoint is written ATOMICALLY (tmp + os.replace), never appended, so it
    is structurally immune to the crash-NUL-glue class that broke the ladder's
    vacuity check (crash #17: a NUL hole with no trailing newline, next append
    glued onto it). A torn/absent file simply restarts the hunt rather than
    silently mis-parsing."""
    try:
        with open(CKPT, errors="replace") as f:
            return json.loads(f.read().replace("\0", ""))
    except Exception:
        return {"done_callers": [], "caught": 0, "rolls": {}, "finished": False}

def ckpt_write(st):
    tmp = CKPT + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f)
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, CKPT)

def read_events():
    """NUL-tolerant reader for the APPEND-only events file — the hardened shape
    from ladder_ckpt.sh's check_banked, not a fresh one that repeats the bug."""
    out = []
    try:
        lines = open(OUT, errors="replace").read().splitlines()
    except OSError:
        return out
    for ln in lines:
        t = ln.replace("\0", "").replace("\ufffd", "").strip()
        if not t:
            continue
        if '{"caller"' in t:
            t = t[t.rfind('{"caller"'):]
        try:
            out.append(json.loads(t))
        except ValueError:
            continue
    return out

def snap(conn, cconn):
    """Cheap caller-side state snapshot; every field is one RPC."""
    d = {}
    try:
        pi = conn.call("getpeerinfo") or []
        d["peers"] = len(pi)
        d["peers_in"]  = sum(1 for p in pi if p.get("inbound"))
        d["peers_out"] = sum(1 for p in pi if not p.get("inbound"))
        d["peers_no_send"] = sum(1 for p in pi if p.get("bytessent", 1) == 0)
    except Exception as e:
        d["peers_err"] = str(e)[:120]
    for k, m in (("banned", "listbanned"), ("mempool_n", "getrawmempool"),
                 ("height", "getblockcount")):
        try:
            v = conn.call(m)
            d[k] = len(v) if isinstance(v, list) else v
            if k == "banned" and isinstance(v, list) and v:
                d["banned_detail"] = v[:5]
        except Exception as e:
            d[k + "_err"] = str(e)[:120]
    try:
        h = conn.call("getblockcount")
        bh = conn.call("getblockhash", [h])
        d["secs_since_block"] = round(time.time() - conn.call("getblock", [bh])["time"], 1)
    except Exception:
        pass
    return d

def main():
    caller = os.environ.get("HUNT_CALLER", "caller1")
    want   = int(os.environ.get("HUNT_WANT", "1"))
    budget = int(os.environ.get("HUNT_BUDGET", "60"))

    cconn = M.Conn(M.port_of(caller))
    disc = cconn.call("ptx_roll", [1, 1, 100, False, [], "bo-disc", "b0000000"])
    members = sorted(m.split(":")[0] for m in disc["quorum_members"])
    print(f"[hunt] {caller} quorum {disc['quorum_hash'][:16]} members={','.join(members)}")

    st = ckpt_read()
    if st.get("finished"):
        # `finished` is terminal for BOTH outcomes (fixed 2026-08-19): a catch, or
        # every caller having exhausted its budget. `outcome` says which; before the
        # fix, exhaustion never set the flag and the sweep re-fired on every boot.
        why = ("budget exhausted on every caller, no catch"
               if st.get("outcome") == "exhausted" else "blackout already caught")
        print(f"[hunt] checkpoint says FINISHED ({why}) — nothing to do")
        return
    caught = st.get("caught", 0)
    start = int(st.get("rolls", {}).get(caller, 0))
    if start:
        print(f"[hunt] resuming {caller} from roll {start} (checkpoint)")
    for i in range(start, budget):
        pre = snap(cconn, cconn)
        r = M.one_roll(caller, members, f"{i:04x}b00b", "blackout-hunt")
        post = snap(cconn, cconn)
        acc = r.get("members_accepted")
        entered = r.get("commit_txid") is not None
        # A sample with a rotated quorum proves nothing about delivery — the
        # probe was watching the wrong nodes. Re-discover and skip, never count.
        if not r.get("quorum_matched", True):
            print(f"[hunt] {i+1}/{budget} QUORUM ROTATED mid-hunt — sample discarded, "
                  f"re-discovering members", flush=True)
            members = sorted(r.get("quorum_actual") or members)
            st.setdefault("rolls", {})[caller] = i + 1
            ckpt_write(st)
            time.sleep(4)
            continue
        blackout = (acc == 0) or (not entered and r.get("why"))
        line = (f"[hunt] {i+1}/{budget} total={r.get('roll_total_s')}s "
                f"accepted={acc}/{r.get('members_total')} "
                f"commit_in_caller_mempool={'YES' if entered else 'NO'}")
        if not r.get("ok"):
            line += f"  FAIL({(r.get('why') or r.get('err') or '')[:50]})"
        print(line, flush=True)
        if blackout or not r.get("ok"):
            rec = {"caller": caller, "i": i, "when": time.strftime("%FT%TZ", time.gmtime()),
                   "blackout": bool(blackout), "commit_entered_caller_mempool": entered,
                   "result": r, "pre": pre, "post": post}
            with open(OUT, "a") as f:
                f.write(json.dumps(rec) + "\n")
            print(f"    ^^ CAUGHT: entered_caller_mempool={entered} "
                  f"peers {pre.get('peers')}->{post.get('peers')} "
                  f"banned {pre.get('banned')} mempool {pre.get('mempool_n')}->{post.get('mempool_n')} "
                  f"secs_since_block={pre.get('secs_since_block')}", flush=True)
            if blackout:
                caught += 1
                st["caught"] = caught
                if caught >= want:
                    st["finished"] = True
                    st["outcome"] = "caught"
                    ckpt_write(st)
                    print(f"[hunt] caught {caught} blackout(s) — TERMINATING on success "
                          f"(no further stakes burned)")
                    return
        # checkpoint EVERY roll: the host's mean time between crashes is shorter
        # than this hunt, so a restart must resume, not start over.
        st.setdefault("rolls", {})[caller] = i + 1
        st["caught"] = caught
        ckpt_write(st)
        time.sleep(4)
    st.setdefault("done_callers", [])
    if caller not in st["done_callers"]:
        st["done_callers"].append(caller)
    ckpt_write(st)
    print(f"[hunt] budget exhausted: {caught} blackout(s) in {budget} rolls")

main()
