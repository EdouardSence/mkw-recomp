#!/usr/bin/env bash
# KWin window rule for our runs: windows whose Wayland app id is mkw-recomp-run (MKW_BACKGROUND_WINDOW=1, set by
# live.sh, run_perf.sh and the launcher shots) never take the focus and open minimized, below the others. KWin
# activates every new window by default and ignores SDL's NOT_FOCUSABLE for that, so a run an agent launched jumped in
# front of Adrià (07/11/2026). Plasma 6 lists the rules in General/Order (it rewrites the older rules/count keys).
# The player's window (app id mkw-recomp) is not touched. Idempotent: the rule is written again under a fixed id.
# Usage: tools/run/kwin-rule.sh [--remove | --ensure]   (--ensure: only when the rule is missing or not listed; the run
# scripts call it, so a rule dropped from the list comes back before the next window opens)
set -euo pipefail
id=mkw-recomp-run
file=kwinrulesrc
command -v kreadconfig6 >/dev/null || exit 0   # not KDE: nothing to do
rules=$(kreadconfig6 --file "$file" --group General --key Order)
if [ "${1:-}" = --ensure ] && tr ',' '\n' <<<"$rules" | grep -qx "$id" &&
   [ "$(kreadconfig6 --file "$file" --group "$id" --key minimize)" = true ]; then
  exit 0
fi
list=$(tr ',' '\n' <<<"$rules" | grep -vx -e "$id" -e '' || true)
if [ "${1:-}" = --remove ]; then
  kwriteconfig6 --file "$file" --group "$id" --key Description --delete
else
  w() { kwriteconfig6 --file "$file" --group "$id" --key "$1" "$2"; }
  w Description "mkw-recomp: runs in the background"
  w wmclass "$id"; w wmclassmatch 1; w wmclasscomplete false
  w acceptfocus false; w acceptfocusrule 2          # never takes the focus
  w fsplevel 4; w fsplevelrule 2                    # focus stealing prevention: extreme
  w below true; w belowrule 3                       # opens below the other windows (initially)
  w minimize true; w minimizerule 3                 # and minimized (Adrià 09/11/2026)
  list=$(printf '%s\n%s' "$list" "$id" | grep -v '^$')
fi
kwriteconfig6 --file "$file" --group General --key Order "$(paste -sd, <<<"$list")"
qdbus6 org.kde.KWin /KWin reconfigure
echo "kwin rules: $(paste -sd, <<<"$list")"
