#!/bin/sh
# ptx-w2 fleet entrypoint — copied from docker-bea (W2.0a; identical semantics).
# Launches Hemisd under -ptxbea, injecting RPC credentials and PTX node identity
# from env. Extra args (e.g. -addnode/-ptxnode) forwarded verbatim from CMD.

set -e

DATADIR="${DATADIR:-/root/.hemis-ptxbea}"

mkdir -p "$DATADIR"

cat > "$DATADIR/Hemis.conf" <<EOF
rpcuser=${RPCUSER:?RPCUSER env var required}
rpcpassword=${RPCPASSWORD:?RPCPASSWORD env var required}
rpcallowip=0.0.0.0/0
rpcbind=0.0.0.0
server=1
listen=1
dbcache=${DBCACHE:-50}
maxmempool=${MAXMEMPOOL:-50}
EOF

# PTX_NODE_ID is set per-container and updated after ProRegPL registration
# (Phase 2 bootstrap restart with compound label:suffix identity).
if [ -n "${PTX_NODE_ID:-}" ]; then
    echo "ptxnodeid=${PTX_NODE_ID}" >> "$DATADIR/Hemis.conf"
fi

# One-shot reindex (BUG-037 recovery tooling): if the operator dropped a
# REINDEX_ONCE marker in the datadir, reindex on THIS start only — the marker
# is consumed before launch so a restart loop cannot reindex twice.  Conf-based
# because this file regenerates Hemis.conf on every start.
if [ -f "$DATADIR/REINDEX_ONCE" ]; then
    echo "reindex=1" >> "$DATADIR/Hemis.conf"
    rm -f "$DATADIR/REINDEX_ONCE"
    echo "entrypoint: REINDEX_ONCE marker consumed — this start reindexes"
fi


# Dual-stack RPC (2026-08-14 18h-incident fix, previously image-only — now the
# durable template): `[::]` alone binds BOTH families (the IPv6 socket is not
# V6ONLY; adding 0.0.0.0 alongside it conflicts). Without this, a caller whose
# DGM-derived member address is v6 cannot reach that member's RPC at all —
# transport-fail was 86% of members on the mixed-v6 fleet.
exec Hemisd \
    -ptxbea \
    -datadir="$DATADIR" \
    -dnsseed=0 \
    -port=29994 \
    -rpcport=29903 \
    -rpcbind=[::] \
    -rpcallowip=0.0.0.0/0 \
    -rpcallowip=::/0 \
    "$@"
