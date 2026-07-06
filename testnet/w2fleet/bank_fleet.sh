#!/bin/bash
# Bank the ptx-w2 fleet — rt-bank pattern at fleet scale (W2.0a).
# Usage: bank_fleet.sh <N> <tag>     e.g. bank_fleet.sh 22 h123-40b109c
# PRECONDITION: fleet cleanly stopped (docker compose -p ptx-w2 stop) and every
# Hemisd pid gone. This script REFUSES to run with ptx-w2 containers running.
# It never touches ptx-bea-* or any named volume.
set -euo pipefail

N="${1:?usage: bank_fleet.sh <N> <tag>}"
TAG="${2:?usage: bank_fleet.sh <N> <tag>}"

W2_ROOT=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
DOCKER_W2=/mnt/pve/Node14TB/hemis-ptx/docker-w2
BANK="$W2_ROOT/bank"
OUT="$BANK/w2-fleet-N${N}-${TAG}.tar.gz"

if docker ps --format '{{.Names}}' | grep -q '^ptx-w2-'; then
    echo "REFUSED: ptx-w2 containers still running — stop the fleet first" >&2
    exit 1
fi

mkdir -p "$BANK"
STAGE="$BANK/.stage-N${N}-${TAG}"
rm -rf "$STAGE"; mkdir -p "$STAGE"

# log hygiene: truncate per-node debug.log before tar (disk discipline; logs
# are runtime artifacts, not chain state)
find "$W2_ROOT/datadirs" -name debug.log -exec truncate -s 0 {} \;

cp -a "$W2_ROOT/datadirs" "$STAGE/datadirs"
cp -a "$DOCKER_W2/.env" "$STAGE/env"
cp -a "$DOCKER_W2/docker-compose.generated.yml" "$STAGE/"
mkdir -p "$STAGE/binaries"
cp -a "$DOCKER_W2/binaries/." "$STAGE/binaries/"

( cd "$STAGE" && find . -type f ! -name MANIFEST.md5 -exec md5sum {} \; > MANIFEST.md5 )
tar czf "$OUT" -C "$STAGE" .
rm -rf "$STAGE"
md5sum "$OUT" | tee "$OUT.md5"
echo "BANKED: $OUT ($(du -h "$OUT" | cut -f1))"
