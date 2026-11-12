#!/bin/sh
# Plays a fixed scripted session on the host build and prints a checksum of
# each screenshot and of the recorded sound. Run before and after a change
# that should not alter what the game draws or plays; the output must match.
# Usage: tools/regress.sh [OUT_DIR]
set -eu
out=${1:-build/regress}
rm -rf "$out"; mkdir -p "$out"
script="A,-*600,shot,B*3,-*320,shot,B*3,-*200,B*3,-*200,shot,B*3,-*300,shot,B*3,-*200,shot,B*3,-*300,B*3,-*300,B*3,-*300,shot,B*3,-*150,shot,B*3,-*300,B*3,-*300,B*3,-*400,B*3,-*300,B*3,-*300,shot"
i=0; while [ $i -lt 12 ]; do script="$script,B*3,-*240,B*3,-*240"; i=$((i+1)); done
script="$script,shot,RIGHT*90,DOWN*70,RIGHT*100,UP*60,RIGHT*60,UP*60,UP+LEFT*60,shot,UP*80,LEFT*200,UP*100,shot,START*3,-*120,shot"
build/host/lom3ds-host --data build/sdmc --out "$out" --wav "$out/sound.wav" --script "$script" 2>"$out/log.txt"
grep "speed:" "$out/log.txt" | awk '{for(i=1;i<=NF;i++) if($i=="(drawing") {d+=$(i+1); n++}} END {if(n) printf "average drawing time %.2f ms per frame over %d windows\n", d/n, n}'
( cd "$out" && md5sum shot*_top.ppm sound.wav )
