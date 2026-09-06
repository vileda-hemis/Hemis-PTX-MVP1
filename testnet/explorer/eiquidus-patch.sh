#!/bin/bash
# ★★ eIquidus is THIRD-PARTY SOFTWARE WITH NO SOURCE IN THIS REPOSITORY, so any
# change to it is a deployed artefact nothing here can see. This script exists so
# that change is at least REPRODUCIBLE: run it again after an eIquidus upgrade and
# the fleet's two divergences from stock are restored, verifiably and in seconds.
#
# ★★★ It touches settings.json ONLY -- never views/*.pug or lib/*.js. eIquidus
# says so itself ("Please edit settings.json, not settings.json.template"), and a
# config change survives an upgrade where a template patch is silently reverted.
# Idempotent: safe to run repeatedly.
#
# (1) FIVE DOCUMENTED ENDPOINTS THAT DO NOT WORK ON THIS CHAIN. Measured
#     2026-09-05: each returns HTTP 200 carrying "This method is disabled" --
#     an integrator checking status codes sees success and gets a string where a
#     number was expected. getvotelist is worse: it returns "Error: Check your
#     console". info.pug renders an endpoint only when its settings flag is true,
#     so flipping the flag removes it from the page through the intended path and
#     KEEPS the key in settings.json -- the information that the endpoint exists
#     upstream and is off here is preserved where an operator would look for it.
#
# (2) A POINTER TO THE PTX API. Without it a developer reading /info sees a
#     generic block explorer and no sign this chain does anything unusual, on the
#     one surface whose purpose is demonstrating that it does. Plain text, not a
#     link: info.pug renders api_description through Pug's escaping `=`, so markup
#     would appear literally.
set -u
S=${EIQUIDUS_SETTINGS:-/opt/eiquidus/settings.json}
[ -f "$S" ] || { echo "no settings.json at $S" >&2; exit 2; }
B="$S.bak.$(date +%Y%m%d-%H%M%S)"; cp -a "$S" "$B"; echo "  backup: $B"

python3 - "$S" <<'PY'
import re, sys
p = sys.argv[1]
src = open(p, encoding="utf-8").read()
lines = src.split("\n")

DISABLE = ["getmasternodelist", "getvotelist", "getcurrentprice",
           "getmasternoderewards", "getdistribution", "getmasternodecount"]
changed = 0
for m in DISABLE:
    for i, ln in enumerate(lines):
        if '"%s"' % m in ln:
            for j in range(i, min(i + 6, len(lines))):
                if '"enabled"' in lines[j]:
                    new = re.sub(r'("enabled"\s*:\s*)true', r'\1false', lines[j])
                    if new != lines[j]:
                        lines[j] = new; changed += 1
                    break
            break
print("  endpoints disabled: %d" % changed)

POINTER = ("PTX-specific endpoints (payload verification, transaction decode, "
           "commitment status) are documented at /v2/api")
hit = 0
for i, ln in enumerate(lines):
    if '"api_description"' in ln and POINTER not in ln:
        m = re.match(r'^(\s*"api_description"\s*:\s*")(.*?)("\s*,?\s*)$', ln)
        if m:
            body = m.group(2).rstrip()
            sep = "" if body.endswith(".") else "."
            lines[i] = m.group(1) + body + sep + " " + POINTER + m.group(3)
            hit += 1
        break
print("  api_description pointer added: %d" % hit)
open(p, "w", encoding="utf-8").write("\n".join(lines))
PY

# ★ The pointer goes in locale/en.json: this install has NO "localization"
# section in settings.json, so there is no config-level override. It is the only
# file outside settings.json this script touches, and it is data rather than code.
L=${EIQUIDUS_LOCALE:-/opt/eiquidus/locale/en.json}
if [ -f "$L" ]; then
    cp -a "$L" "$L.bak.$(date +%Y%m%d-%H%M%S)"
    python3 - "$L" <<'PY2'
import re, sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
PTR = ("PTX-specific endpoints (payload verification, transaction decode, "
       "commitment status) are documented at /v2/api")
hit = 0
for i, ln in enumerate(lines):
    if '"api_description"' in ln and PTR not in ln:
        m = re.match(r'^(\s*"api_description"\s*:\s*")(.*?)("\s*,?\s*)$', ln)
        if m:
            lines[i] = m.group(1) + m.group(2).rstrip(". ") + ". " + PTR + m.group(3)
            hit += 1
        break
print("  api_description pointer: %d" % hit)
open(p, "w", encoding="utf-8").write("\n".join(lines))
PY2
fi
echo "  verify: $(grep -c '"enabled": false' "$S") disabled flags now present"

# ─────────────────────────────────────────────────────────────────────────────
# (3) A ROUTE FROM THE BLOCK EXPLORER TO /v2.
#
# ★ Every PTX page -- the verifier, health, the roll feed, quorum history, the
#   API docs -- was reachable ONLY by typing its URL. eIquidus linked to none of
#   them, so an operator arriving at ptx-explorer.lnky.uk had no route to any of
#   the surface built to make this chain legible.
#
# ★★ It is a FOOTER SOCIAL LINK and that is a real limitation, stated rather
#   than hidden: settings.json offers no way to add a primary nav item, and the
#   alternative -- editing views/*.pug -- is exactly what the header of this
#   file rejects, because a template patch is silently reverted by the next
#   upgrade while a settings change survives it. A weak route that persists
#   beats a strong one that disappears.
#
# ★★★ It APPENDS an entry rather than repurposing one of the stock disabled
#   links (Github, Twitter, Coingecko...), so enabling any of those later is
#   unaffected and nothing stock is misrepresented.
python3 - "$S" <<'PY3'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
if any("PTX roll verifier" in l for l in lines):
    print("  (3) explorer -> /v2 footer link: already present")
    sys.exit(0)
# find the close of the social_links array
start = next(i for i, l in enumerate(lines) if '"social_links"' in l)
close = next(i for i in range(start, len(lines)) if lines[i].strip() == "],")
# the last entry's closing brace, so we can comma it and append after
last = next(i for i in range(close - 1, start, -1) if lines[i].strip() == "}")
lines[last] = lines[last].rstrip() + ","
entry = [
    "        {",
    "          // ADDED BY eiquidus-patch.sh -- the only settings-only route from",
    "          // this explorer to the PTX pages. See section (3) of that script.",
    '          "enabled": true,',
    '          "tooltip_text": "PTX roll verifier",',
    '          "url": "/v2",',
    '          "fontawesome_class": "fa-solid fa-dice",',
    '          "image_path": ""',
    "        }",
]
lines[last + 1:last + 1] = entry
open(p, "w", encoding="utf-8").write("\n".join(lines))
print("  (3) explorer -> /v2 footer link: ADDED")
PY3
