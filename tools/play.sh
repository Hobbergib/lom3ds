#!/bin/sh
# Runs the host build against build/sdmc with an input script and turns the
# screenshots into PNGs. Under gdb: prints where the game crashed, or what
# it was doing when the script ran out.
# Usage: tools/play.sh NAME SCRIPT [TIMEOUT_SECONDS]
set -u
status=$(mktemp)
out=build/shots/$1
rm -rf "$out"; mkdir -p "$out"
{ timeout "${3:-170}" gdb -q -batch -ex "break plat_exit" -ex run -ex "bt 14" --args build/host/lom3ds-host --data build/sdmc --out "$out" --script "$2" 2>&1; echo $? > "$status"; } |
    grep -v "not implemented\|^\[\|^$\|libthread_db\|debuginfod\|^Using host\|No stack" | cut -c1-220 | tail -30
echo "exit=$(cat "$status") (124 means it timed out)"; rm -f "$status"
python3 tools/shots2png.py "$out"; for v in "$out"/vram*.ppm; do [ -e "$v" ] && python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[1][:-3]+\"png\")" "$v"; done; ls "$out"/*.png 2>/dev/null
