#!/bin/bash
# Restore a banked ptx-w2 fleet snapshot (W2.0a).
# Usage: restore_fleet.sh <bank-tar>
# Restores datadirs + .env (compound node_ids) into place. Start afterwards:
#   docker compose -f /mnt/pve/Node14TB/hemis-ptx/docker-w2/docker-compose.generated.yml \
#     -p ptx-w2 up -d
# Then prove GM-ELIGIBLE, not just running (Amendment 4):
#   python3 testnet/w2fleet/validate_fleet.py <N> all
# Never touches ptx-bea-* or any named volume.
set -euo pipefail

TAR="${1:?usage: restore_fleet.sh <bank-tar>}"
W2_ROOT=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
DOCKER_W2=/mnt/pve/Node14TB/hemis-ptx/docker-w2

if docker ps --format '{{.Names}}' | grep -q '^ptx-w2-'; then
    echo "REFUSED: ptx-w2 containers running — stop/down (-p ptx-w2) first" >&2
    exit 1
fi

md5sum -c "$TAR.md5" 2>/dev/null || echo "WARN: no .md5 beside tar (continuing)"

STAGE="$W2_ROOT/.restore-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
tar xzf "$TAR" -C "$STAGE"
( cd "$STAGE" && md5sum -c MANIFEST.md5 --quiet ) && echo "manifest OK"

rm -rf "$W2_ROOT/datadirs"
mv "$STAGE/datadirs" "$W2_ROOT/datadirs"
cp -a "$STAGE/env" "$DOCKER_W2/.env"
cp -a "$STAGE/docker-compose.generated.yml" "$DOCKER_W2/"
rm -rf "$STAGE"
echo "RESTORED from $TAR — now: docker compose -f $DOCKER_W2/docker-compose.generated.yml -p ptx-w2 up -d"
