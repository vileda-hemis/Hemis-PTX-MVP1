#!/bin/bash
# PASSIVE ceremony-window CPU sampler (expansion assessment + B evidence).
# Waits for h2205, samples docker stats + loadavg every 30s until h2265.
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
tip() { python3 -c "
from harness.cluster import W2Cluster
print(W2Cluster(98).gms[0].getblockcount())" 2>/dev/null || echo 0; }
T=0; until [ "$T" -ge 2205 ] 2>/dev/null; do sleep 120; T=$(tip); done
echo "sampling from h$T $(date -u +%FT%TZ)"
while [ "$T" -lt 2265 ]; do
  echo "== h$T $(date -u +%T) load: $(cut -d' ' -f1-3 /proc/loadavg)"
  docker stats --no-stream --format '{{.Name}} {{.CPUPerc}} {{.MemUsage}}' $(docker ps -q --filter label=com.docker.compose.project=ptx-w2) 2>/dev/null | sort -t' ' -k2 -rn | head -14
  sleep 30; T=$(tip)
done
echo "done h$T"
