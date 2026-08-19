#!/bin/bash
# WAN-latency substrate for the w2r fleet (pre-testnet fan-out budget arc,
# 2026-08-16). Applies netem delay on the HOST-side veth of every ptx-w2r
# container. Host-side veth egress is traffic INTO the container, so a uniform
# one-way delay D gives container<->container RTT of 2D on every pair (each
# direction crosses exactly one delayed veth), and host->container probe RTT
# of ~D (probes ride one delayed leg; RPC sweep timings shift accordingly).
#
# Chosen over Toxiproxy deliberately: fan-out members are addressed by their
# DGM-REGISTERED IPs (protx service field, consensus data) — routing through a
# TCP proxy would need every GM re-registered. netem is transparent to
# addressing and also delays the P2P mesh (INV trickle), which is half of the
# not-seen population the fan-out budget exists to cover. Same experiment,
# honest substrate.
#
#   netem_mesh.sh map                 rebuild container->veth map (cached)
#   netem_mesh.sh apply <D_ms> [J_ms] set delay D (jitter J, default D/10) on all
#   netem_mesh.sh clear               remove all netem qdiscs
#   netem_mesh.sh status              count veths with netem active
#   netem_mesh.sh verify              measure P2P RTT via getpeerinfo pingtime
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
MAP=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/netem_veth.map
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)

names() { seq -f 'ptx-w2r-caller%.0f' 1 8; seq -f 'ptx-w2r-gm%02.0f' 1 153; }

build_map() {
    : > "$MAP"
    local c pid peer veth
    for c in $(names); do
        pid=$(docker inspect -f '{{.State.Pid}}' "$c" 2>/dev/null) || continue
        [ -n "$pid" ] && [ "$pid" != 0 ] || continue
        peer=$(nsenter -t "$pid" -n ip -o link show eth0 2>/dev/null \
               | grep -o '@if[0-9]*' | tr -dc 0-9) || continue
        veth=$(ip -o link | awk -F': ' -v n="$peer" '$1==n{print $2}' | cut -d@ -f1)
        [ -n "$veth" ] && echo "$c $veth" >> "$MAP"
    done
    wc -l < "$MAP" | xargs echo "mapped veths:"
}

ensure_map() {
    # Container restarts change veths — stale map entries name devices that no
    # longer exist. Rebuild whenever any mapped veth is missing.
    [ -s "$MAP" ] || { build_map; return; }
    while read -r _ v; do
        [ -e "/sys/class/net/$v" ] || { build_map; return; }
    done < "$MAP"
}

case "${1:-}" in
map) build_map ;;
apply)
    D="${2:?usage: apply <delay_ms> [jitter_ms]}"; J="${3:-$((D/10))}"
    ensure_map
    n=0
    while read -r c v; do
        tc qdisc replace dev "$v" root netem delay "${D}ms" "${J}ms" && n=$((n+1))
    done < "$MAP"
    echo "netem delay ${D}ms jitter ${J}ms applied on $n/$(wc -l < "$MAP") veths (pair RTT ~$((2*D))ms)"
    ;;
clear)
    ensure_map
    n=0
    while read -r c v; do
        tc qdisc del dev "$v" root 2>/dev/null && n=$((n+1))
    done < "$MAP"
    echo "netem cleared on $n veths"
    ;;
status)
    ensure_map
    act=0; tot=0
    while read -r c v; do
        tot=$((tot+1))
        tc qdisc show dev "$v" 2>/dev/null | grep -q netem && act=$((act+1))
    done < "$MAP"
    echo "netem active on $act/$tot veths"
    tc qdisc show dev "$(head -1 "$MAP" | awk '{print $2}')" | head -1
    ;;
verify)
    # Organic RTT check: P2P ping times between real peers reflect the pair RTT.
    for c in ptx-w2r-gm01 ptx-w2r-caller1; do
        docker exec "$c" "${CLI[@]}" getpeerinfo 2>/dev/null \
        | python3 -c '
import json,sys,statistics
ps=[p.get("pingtime") for p in json.load(sys.stdin) if p.get("pingtime")]
ps=[p*1000 for p in ps]
print("'"$c"': peers=%d pingtime ms p50=%.0f p95=%.0f" % (
    len(ps),
    statistics.median(ps) if ps else -1,
    sorted(ps)[max(0,int(len(ps)*.95)-1)] if ps else -1))'
    done
    ;;
*) echo "usage: $0 map|apply <D_ms> [J_ms]|clear|status|verify"; exit 1 ;;
esac
