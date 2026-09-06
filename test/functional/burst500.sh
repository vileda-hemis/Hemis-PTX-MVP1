
#!/bin/bash
# 500-roll burst test for KDD-034 + fee verification
# Run from: /mnt/pve/Node14TB/hemis-ptx/docker

CLI_CMD="Hemis-cli -ptxtestnet -datadir=/root/.hemis-ptxtestnet -rpcport=29902"
RPC_URL="http://172.28.0.10:29902/"
RPC_AUTH="ptxrpc:ptxpass2026"
N_ROLLS=500
PACE=0.3                      # seconds between rolls
MIN_UTXOS_NEEDED=600          # 500 + 20% headroom for change-recycling lag
LOG=/tmp/burst500.log
WATCH_LOG=/tmp/burst500_watch.log

# ─── Pre-flight: confirm enough UTXOs exist ────────────────────────────────
echo "[$(date +%H:%M:%S)] Pre-flight: checking UTXO availability..."
while true; do
  USABLE=$(docker exec ptx-caller $CLI_CMD listunspent 1 9999999 \
    | python3 -c "import sys,json;u=json.load(sys.stdin);print(sum(1 for x in u if x['amount']>=2.0 and x['amount']<=3.0))")
  echo "[$(date +%H:%M:%S)] Usable 2-3 HMS UTXOs: $USABLE / $MIN_UTXOS_NEEDED needed"
  if [ "$USABLE" -ge "$MIN_UTXOS_NEEDED" ]; then
    echo "[$(date +%H:%M:%S)] Sufficient UTXOs. Proceeding."
    break
  fi
  echo "[$(date +%H:%M:%S)] Split still completing, sleeping 60s..."
  sleep 60
done

# ─── Snapshot pre-state ────────────────────────────────────────────────────
PRE=$(docker exec ptx-caller bash -c "curl -s -u $RPC_AUTH \
  --data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"x\",\"method\":\"ptx_lottery_status\",\"params\":[]}' \
  -H 'content-type:text/plain;' http://127.0.0.1:29902/")
START_H=$(echo "$PRE" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['current_height'])")
POOL_BEFORE=$(echo "$PRE" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['pool_balance_sat'])")
UTXO_BEFORE=$(echo "$PRE" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['pool_utxo_count'])")
WALLET_BEFORE=$(docker exec ptx-caller $CLI_CMD getbalance)

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  500-ROLL BURST — START"
echo "═══════════════════════════════════════════════════════"
echo "  Start height:        $START_H"
echo "  Pool balance:        $(python3 -c "print($POOL_BEFORE/1e8)") HMS"
echo "  Pool UTXO count:     $UTXO_BEFORE"
echo "  Wallet balance:      $WALLET_BEFORE HMS"
echo "  Expected pool delta: ~500 HMS (500 rolls × 1 HMS fee)"
echo "  Expected wallet drop: ~500.05 HMS (incl. miner fees)"
echo "  Pace: ${PACE}s/roll → ~$(python3 -c "print(int($N_ROLLS * $PACE))")s total"
echo "═══════════════════════════════════════════════════════"
echo ""

# ─── Background watcher: poll every 30s ────────────────────────────────────
> $WATCH_LOG
(while true; do
  ST=$(docker exec ptx-caller bash -c "curl -s -u $RPC_AUTH \
    --data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"x\",\"method\":\"ptx_lottery_status\",\"params\":[]}' \
    -H 'content-type:text/plain;' http://127.0.0.1:29902/" 2>/dev/null)
  if [ -n "$ST" ]; then
    LINE=$(echo "$ST" | python3 -c "
import sys,json,time
try:
    r=json.load(sys.stdin)['result']
    print(f'{time.strftime(\"%H:%M:%S\")} h={r[\"current_height\"]} utxos={r[\"pool_utxo_count\"]} bal={r[\"pool_balance_sat\"]/1e8:.2f}HMS')
except: pass
")
    [ -n "$LINE" ] && echo "$LINE" | tee -a $WATCH_LOG
  fi
  sleep 30
done) &
WATCH_PID=$!

# ─── Fire the burst ────────────────────────────────────────────────────────
> $LOG
echo "[$(date +%H:%M:%S)] Firing $N_ROLLS rolls at ${PACE}s pace..."
BURST_START=$(date +%s)
for i in $(seq 1 $N_ROLLS); do
  SALT=$(python3 -c "import hashlib;print(hashlib.md5(b'burst500_$i').hexdigest()[:8])")
  RESP=$(curl -s -u $RPC_AUTH \
    --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"x\",\"method\":\"ptx_roll\",\"params\":[1,1,100,false,[],\"burst500_$i\",\"$SALT\"]}" \
    -H 'content-type:text/plain;' $RPC_URL 2>/dev/null)
  if echo "$RESP" | grep -q '"results"'; then
    TXID=$(echo "$RESP" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['result'].get('tx_id') or d['result'].get('session_txid') or '?')" 2>/dev/null)
    echo "$i OK txid=${TXID:0:16}..." >> $LOG
  else
    ERR=$(echo "$RESP" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('error',{}).get('message','?')[:60])" 2>/dev/null)
    echo "$i ERR: $ERR" >> $LOG
  fi
  # Progress every 50 rolls
  if [ $((i % 50)) -eq 0 ]; then
    OK_SOFAR=$(grep -c "OK txid" $LOG)
    ERR_SOFAR=$(grep -c "ERR:" $LOG)
    echo "[$(date +%H:%M:%S)] Progress: $i/$N_ROLLS (ok=$OK_SOFAR err=$ERR_SOFAR)"
  fi
  sleep $PACE
done
BURST_END=$(date +%s)
BURST_DURATION=$((BURST_END - BURST_START))

OK_TOTAL=$(grep -c "OK txid" $LOG)
ERR_TOTAL=$(grep -c "ERR:" $LOG)
echo ""
echo "[$(date +%H:%M:%S)] Burst fired: ${BURST_DURATION}s, ok=$OK_TOTAL err=$ERR_TOTAL"

# ─── Wait for trailing confirms (3 blocks past final roll) ─────────────────
FIRE_END_H=$(docker exec ptx-caller $CLI_CMD getblockcount)
CONFIRM_TARGET=$((FIRE_END_H + 3))
echo "[$(date +%H:%M:%S)] Waiting for block $CONFIRM_TARGET (current=$FIRE_END_H)..."
while [ "$(docker exec ptx-caller $CLI_CMD getblockcount)" -lt "$CONFIRM_TARGET" ]; do
  sleep 20
done
echo "[$(date +%H:%M:%S)] Reached confirmation target."

# ─── Stop watcher ──────────────────────────────────────────────────────────
kill $WATCH_PID 2>/dev/null
sleep 2

# ─── Snapshot post-state ───────────────────────────────────────────────────
POST=$(docker exec ptx-caller bash -c "curl -s -u $RPC_AUTH \
  --data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"x\",\"method\":\"ptx_lottery_status\",\"params\":[]}' \
  -H 'content-type:text/plain;' http://127.0.0.1:29902/")
END_H=$(echo "$POST" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['current_height'])")
POOL_AFTER=$(echo "$POST" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['pool_balance_sat'])")
UTXO_AFTER=$(echo "$POST" | python3 -c "import sys,json;print(json.load(sys.stdin)['result']['pool_utxo_count'])")
WALLET_AFTER=$(docker exec ptx-caller $CLI_CMD getbalance)

# ─── Count consolidations and settlements during the burst window ──────────
CONSOL_COUNT=$(docker exec ptx-caller bash -c "tail -10000 /root/.hemis-ptxtestnet/ptxtestnet/debug.log" \
  | grep "PTX: pool consolidated" \
  | awk -v s=$START_H -v e=$END_H '{for(i=1;i<=NF;i++) if($i~/h=/) {split($i,a,"="); h=a[2]+0; if(h>=s && h<=e) print}}' \
  | wc -l)
SETTLE_COUNT=$(docker exec ptx-caller bash -c "tail -10000 /root/.hemis-ptxtestnet/ptxtestnet/debug.log" \
  | grep -E "PTX: lottery settle confirmed|PTX: settle confirmed|lottery_h=.*settled" \
  | wc -l)

# ─── Math ──────────────────────────────────────────────────────────────────
POOL_DELTA_SAT=$((POOL_AFTER - POOL_BEFORE))
POOL_DELTA_HMS=$(python3 -c "print($POOL_DELTA_SAT / 1e8)")
WALLET_DELTA=$(python3 -c "print($WALLET_BEFORE - $WALLET_AFTER)")
EXPECTED_POOL=$(python3 -c "print($OK_TOTAL * 1.0)")        # service fee = 1 HMS/roll
EXPECTED_WALLET=$(python3 -c "print($OK_TOTAL * 1.0001)")   # + miner fee

# Settlement detection: did pool drop below pre-burst at any point?
SETTLEMENT_FIRED="no"
if [ "$POOL_DELTA_SAT" -lt 0 ] || [ "$SETTLE_COUNT" -gt 0 ]; then
  SETTLEMENT_FIRED="yes ($SETTLE_COUNT settlement events)"
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  500-ROLL BURST — RESULTS"
echo "═══════════════════════════════════════════════════════"
echo "  RPC successes:       $OK_TOTAL / $N_ROLLS"
echo "  RPC errors:          $ERR_TOTAL"
echo "  Burst duration:      ${BURST_DURATION}s"
echo "  Blocks elapsed:      $((END_H - START_H))  (h=$START_H → h=$END_H)"
echo ""
echo "  Pool balance:        $(python3 -c "print($POOL_BEFORE/1e8)") → $(python3 -c "print($POOL_AFTER/1e8)") HMS"
echo "  Pool delta:          $POOL_DELTA_HMS HMS  (expected ~$EXPECTED_POOL HMS)"
echo "  Pool UTXO count:     $UTXO_BEFORE → $UTXO_AFTER"
echo ""
echo "  Wallet balance:      $WALLET_BEFORE → $WALLET_AFTER HMS"
echo "  Wallet delta:        $WALLET_DELTA HMS  (expected ~$EXPECTED_WALLET HMS)"
echo ""
echo "  KDD-034 events:      $CONSOL_COUNT consolidations fired during burst"
echo "  Settlement events:   $SETTLEMENT_FIRED"
echo "═══════════════════════════════════════════════════════"

# ─── Verdict ───────────────────────────────────────────────────────────────
echo ""
if [ "$SETTLEMENT_FIRED" = "no" ]; then
  # Clean window — strict checks
  POOL_RATIO=$(python3 -c "print($POOL_DELTA_HMS / $EXPECTED_POOL * 100)" 2>/dev/null || echo "0")
  echo "VERDICT:"
  echo "  RPC success rate:    $(python3 -c "print(round($OK_TOTAL / $N_ROLLS * 100, 1))")%  (target: >=95%)"
  echo "  Pool/expected ratio: ${POOL_RATIO}%  (target: 95-105%)"
  echo "  Consolidation fired: $CONSOL_COUNT times  (target: >=1 for 500-roll burst)"
else
  echo "VERDICT: settlement fired mid-burst — fee accounting partial"
  echo "  RPC success rate:    $(python3 -c "print(round($OK_TOTAL / $N_ROLLS * 100, 1))")%  (target: >=95%)"
  echo "  Consolidation fired: $CONSOL_COUNT times  (target: >=1)"
fi
echo ""
echo "Watch log:  $WATCH_LOG"
echo "Burst log:  $LOG"
echo ""
echo "Sample errors (first 5):"
grep "ERR:" $LOG | head -5
