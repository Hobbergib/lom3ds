# Legend of Mana for Nintendo 3DS (unofficial port)

A native port of the PlayStation game for the New Nintendo 3DS, built from the
[lom-decomp](https://github.com/celophi/lom-decomp) decompilation. Work in
progress.

No game data is included. Put a disc image of your own copy (North America,
`SLUS-01013`, `.bin` preferred) in `sdmc:/3ds/lom3ds/` and start
`lom3ds.3dsx` from the Homebrew Launcher. Press Y on the start screen to
update to the newest build.

Building: `make` (needs devkitARM with libctru and the 3DS curl and mbedTLS
portlibs, and a checkout of lom-decomp at `../upstream/lom-decomp`).
`make test` runs the host tests (needs 32-bit gcc and libcurl).
