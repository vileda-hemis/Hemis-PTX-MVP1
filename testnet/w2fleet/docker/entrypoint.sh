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
EOF

# PTX_NODE_ID is set per-container and updated after ProRegPL registration
# (Phase 2 bootstrap restart with compound label:suffix identity).
if [ -n "${PTX_NODE_ID:-}" ]; then
    echo "ptxnodeid=${PTX_NODE_ID}" >> "$DATADIR/Hemis.conf"
fi


exec Hemisd \
    -ptxbea \
    -datadir="$DATADIR" \
    -dnsseed=0 \
    -port=29994 \
    -rpcport=29903 \
    -rpcbind=0.0.0.0 \
    "$@"
