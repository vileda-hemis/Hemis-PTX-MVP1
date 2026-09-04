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

# and the exemption-rot check
T=$(mk); sed -i '/no .-ptxfanoutport. to match/d' "$T/testnet/operator/ONBOARDING.md"
red "exemption rot, an allowlist entry that stops matching" "$T" "STALE EXEMPTION"

echo
echo "  red legs: $PASS falsified, $FAIL vacuous"
[ "$FAIL" -eq 0 ] || exit 1
