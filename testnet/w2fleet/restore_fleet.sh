#!/bin/bash
# Restore a banked ptx-w2 fleet snapshot (W2.0a).
# Usage: restore_fleet.sh <bank-tar>
# Restores datadirs + .env (compound node_ids) into place. Start afterwards:
#   docker compose -f /mnt/pve/Node14TB/hemis-ptx/docker-w2/docker-compose.generated.yml \
#     -p ptx-w2 up -d
# Then IMMEDIATELY validate (BUG-019 (a): the daemon auto-locks IsMine DGM
# collaterals at init IF init completes (gmconflock, tiertwo/init.cpp);
# wait_ready re-locks at earliest RPC readiness and the `locks` gate ASSERTS
# coverage instead of assuming it. Residuals R1/R2 — staker starts before
# the auto-lock; init-abort skips it — are pre-RPC and owed to BUG-019 (d),
# pre-testnet):
#   python3 testnet/w2fleet/validate_fleet.py <N> all
# Never touches ptx-bea-* or any named volume.
set -euo pipefail

TAR="${1:?usage: restore_fleet.sh <bank-tar>}"
# Second-instance parameterisation (defaults = the live ptx-w2 fleet):
PROJECT="${PROJECT:-ptx-w2}"
W2_ROOT="${W2_ROOT:-/mnt/pve/Node14TB/hemis-ptx/w2-fleet}"
DOCKER_W2="${DOCKER_W2:-/mnt/pve/Node14TB/hemis-ptx/docker-w2}"

if docker ps --format '{{.Names}}' | grep -q "^${PROJECT}-"; then
    echo "REFUSED: ${PROJECT} containers running — stop/down (-p ${PROJECT}) first" >&2
    exit 1
fi

# run the tar checksum from the tar's own directory — the .md5 references a
# bare filename, so a CWD elsewhere made this silently fail (Piece-1 nit)
( cd "$(dirname "$TAR")" && md5sum -c "$(basename "$TAR").md5" ) 2>/dev/null \
    || echo "WARN: no .md5 beside tar (continuing)"

STAGE="$W2_ROOT/.restore-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
tar xzf "$TAR" -C "$STAGE"
( cd "$STAGE" && md5sum -c MANIFEST.md5 --quiet ) && echo "manifest OK"

rm -rf "$W2_ROOT/datadirs"
mv "$STAGE/datadirs" "$W2_ROOT/datadirs"
cp -a "$STAGE/env" "$DOCKER_W2/.env"
cp -a "$STAGE/docker-compose.generated.yml" "$DOCKER_W2/"
rm -rf "$STAGE"
echo "RESTORED from $TAR — now: docker compose -f $DOCKER_W2/docker-compose.generated.yml -p ${PROJECT} up -d"
echo "THEN IMMEDIATELY: python3 testnet/w2fleet/validate_fleet.py <N> all  (BUG-019 (a): banked collaterals UNLOCKED until relock lands; residual owed to (d))"
