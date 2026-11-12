#!/bin/sh
# Drives the launcher and settings menu through the headless host build.
# Usage: run_ui.sh HOST_BINARY TESTDATA_DIR SHOTS_DIR
set -eu
bin=$1 data=$2 shots=$3
rm -rf "$shots"

# run NAME DATA_SUBDIR SCRIPT: one launcher session, screenshots in $shots/NAME
run() {
    mkdir -p "$shots/$1"
    "$bin" --data "$data/$2" --out "$shots/$1" --script "$3"
}

fail() { echo "FAIL: $1"; exit 1; }
value() { sed -n "s/^$2=//p" "$1/config.ini"; }

rm -f "$data"/*/config.ini

# The three disc states.
run missing empty "shot"
run unrecognised raw "shot"
[ ! -e "$data/empty/config.ini" ] || fail "config written although nothing changed"

# Keys: flip the first option, switch tab with R, go down one row, flip it.
run keys empty "RIGHT,R,DOWN,LEFT,shot"
[ "$(value "$data/empty" screen_fit)" = 1 ] || fail "d-pad did not change screen_fit"
[ "$(value "$data/empty" n3ds_boost)" = 0 ] || fail "d-pad did not change n3ds_boost"

# Touch: Audio tab, then the left arrow of the Volume row three times;
# a fourth row tap left of the value area only moves the cursor.
run touch empty "T200:15,T190:52,T190:52,T190:52,T40:96,shot"
[ "$(value "$data/empty" volume)" = 7 ] || fail "touch did not lower the volume"
[ "$(value "$data/empty" reverb)" = 1 ] || fail "touching a label changed its value"

# Settings persist into the next session; lists stop at their ends.
run clamp empty "R,R,RIGHT,RIGHT,RIGHT,RIGHT,RIGHT,shot"
[ "$(value "$data/empty" volume)" = 10 ] || fail "volume did not stop at 10"
[ "$(value "$data/empty" screen_fit)" = 1 ] || fail "screen_fit was not kept"

# Control bindings: stepping one leaves the others alone, even when two
# then share a control.
rm -f "$data/empty/config.ini"
run bind empty "R,R,R,RIGHT,shot"
[ "$(value "$data/empty" bind_cross)" = 1 ] || fail "Cross was not rebound to B"
[ "$(value "$data/empty" bind_circle)" = 1 ] || fail "rebinding Cross changed Circle"
# The same by touch, in the right-hand column: R1's right arrow gives it ZL,
# which L2 keeps.
run bindtouch empty "T270:15,T290:74,shot"
[ "$(value "$data/empty" bind_r1)" = 6 ] || fail "touch did not rebind R1"
[ "$(value "$data/empty" bind_l2)" = 6 ] || fail "rebinding R1 changed L2"
# The reset button under the bindings, by keys (up from the first row) and
# by touch, puts them all back.
run reset empty "R,R,R,UP,RIGHT,shot"
[ "$(value "$data/empty" bind_cross)" = 0 ] || fail "reset did not restore Cross"
[ "$(value "$data/empty" bind_r1)" = 5 ] || fail "reset did not restore R1"
run bind empty "R,R,R,RIGHT"
run resettouch empty "T270:15,T160:162,shot"
[ "$(value "$data/empty" bind_cross)" = 0 ] || fail "touching reset did not restore Cross"

# The original 3DS has no ZL or ZR: they are not offered, and L2 and R2
# start unbound.
rm -f "$data/empty/config.ini"
# Stepping L1 twice goes L -> R -> START, passing over ZL and ZR.
LOM3DS_OLD3DS=1 run old3ds empty "R,R,R,DOWN*5,RIGHT,RIGHT,shot"
[ "$(value "$data/empty" bind_l2)" = 10 ] || fail "L2 is bound on a console without ZL"
[ "$(value "$data/empty" bind_l1)" = 8 ] || fail "L1 did not skip ZL and ZR"
[ "$(value "$data/empty" bind_start)" = 8 ] || fail "rebinding L1 changed Start"

rm -f "$data/empty/config.ini"
echo "all UI runs passed"
