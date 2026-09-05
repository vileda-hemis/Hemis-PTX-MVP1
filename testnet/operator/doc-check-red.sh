#!/usr/bin/env bash
# RED-prove each limb of doc-check.sh separately. Four invariants, four
# constructible failing inputs. "The suite is green" says nothing about which
# limb is live -- that is the vacuity shape (KDD-115), found three times this week.
R="/mnt/pve/Node14TB/hemis-ptx/src/hemisd"
PASS=0; FAIL=0
mk() {  # build a scratch tree that doc-check.sh can cd into
    T=$(mktemp -d); mkdir -p "$T/testnet/operator"
    cp "$R"/testnet/operator/{OPERATOR_ONEPAGER.md,OPERATOR_GUIDE.md,ONBOARDING.md,doc-check.sh} "$T/testnet/operator/"
    cp "$R"/GM_QUICKSTART.md "$R"/vps-install.sh "$T/"
    echo "$T"
}
run() { ( cd "$1" && bash testnet/operator/doc-check.sh 2>&1 ); }
red() { # $1=name $2=tree $3=expected substring
    out=$(run "$2")
    if echo "$out" | grep -q "$3"; then
        echo "  [RED]  $1"; echo "           -> $(echo "$out" | grep -m1 "$3" | cut -c1-96)"; PASS=$((PASS+1))
    else
        echo "  [VACUOUS] $1 -- the defect was ACCEPTED; this limb proves nothing"; FAIL=$((FAIL+1))
    fi
    rm -rf "$2"
}

# INV-1: a REAL reordering -- the one-pager as it stood before 48cb8d4
T=$(mk); git -C "$R" show 48cb8d4^:testnet/operator/OPERATOR_ONEPAGER.md > "$T/testnet/operator/OPERATOR_ONEPAGER.md"
red "INV-1 ordering, against the REAL pre-48cb8d4 one-pager" "$T" "generateblskeypair appears BEFORE the daemon start"

# INV-2: remove the enable line from the guide
T=$(mk); sed -i '/systemctl enable --now hemis-ptx/d' "$T/testnet/operator/OPERATOR_GUIDE.md"
red "INV-2 presence, enable line deleted from the guide" "$T" "never says to ENABLE the unit"

# INV-3: reintroduce the externalip instruction the one-pager used to carry
T=$(mk); printf '\n```\nexternalip=<this host'"'"'s global IPv6 address, bare, no brackets>\n```\n' >> "$T/testnet/operator/OPERATOR_ONEPAGER.md"
red "INV-3 forbidden, externalip= presented as a line to add" "$T" "presents 'externalip=' as a line to add"

# INV-4: an unmarked retired noun
T=$(mk); printf '\nArrange for it to start at boot with @reboot in cron.\n' >> "$T/testnet/operator/OPERATOR_GUIDE.md"
red "INV-4 retired noun, unmarked @reboot" "$T" "names a retired mechanism with no retirement marker"

# INV-5: put a hand-start back into the documented path -- the instruction that
# actually cost two hosts. ★ This leg REPLACED an earlier one that deleted the
# `Hemis-cli stop` between a hand-start and the enable; when INV-5 was rewritten
# on 2026-09-05 that mutation stopped producing a defect, and this suite reported
# the leg VACUOUS rather than passing it. A red leg that survives the invariant it
# was written for is testing nothing -- which is the whole reason it is checked.
T=$(mk); sed -i 's|^sudo systemctl start hemis-ptx.*$|Hemisd -daemon|' "$T/testnet/operator/OPERATOR_ONEPAGER.md"
red "INV-5 hand-start restored to the documented path" "$T" "prescribes a hand-start"

# ★ And the inverse: a QUERY must not trip it. `Hemisd -version` owns no datadir
# and exits immediately; counting it would make the gate cry wolf on the command
# the guide uses to check which build is installed.
T=$(mk); sed -i 's|^sudo systemctl start hemis-ptx.*$|&\nHemisd -version|' "$T/testnet/operator/OPERATOR_ONEPAGER.md"
if bash "$T/testnet/operator/doc-check.sh" >/dev/null 2>&1; then
    printf '  [RED]  INV-5 inverse: `Hemisd -version` correctly NOT treated as a start\n'
    PASS=$((PASS+1))
else
    printf '  [VACUOUS] INV-5 inverse: a query tripped the gate -- it cries wolf\n'
    FAIL=$((FAIL+1))
fi

# and the exemption-rot check
T=$(mk); sed -i '/no .-ptxfanoutport. to match/d' "$T/testnet/operator/ONBOARDING.md"
red "exemption rot, an allowlist entry that stops matching" "$T" "STALE EXEMPTION"

echo
echo "  red legs: $PASS falsified, $FAIL vacuous"
[ "$FAIL" -eq 0 ] || exit 1
