# Legend of Mana for Nintendo 3DS
#
#   make          build/lom3ds.3dsx   (Homebrew Launcher)
#   make host     build/host/lom3ds-host   (headless Linux build, 32-bit like the 3DS)
#   make test     host unit tests and a scripted run of the menus
#   make shots    screenshots of the menus as PNG, in build/shots/ui
#   make clean
#
# DEVKITPRO defaults to the toolchain unpacked next to this directory, and
# DECOMP to the decompilation checked out next to it.

DEVKITPRO ?= $(abspath ../toolchain/devkitpro)
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   ?= $(DEVKITPRO)/libctru
DKP_TOOLS := $(DEVKITPRO)/tools/bin
PORTLIBS  ?= $(DEVKITPRO)/portlibs/3ds
DECOMP    ?= $(abspath ../upstream/lom-decomp)
export DEVKITARM

APP_TITLE  := Legend of Mana 3DS
APP_DESC   := Unofficial port. Needs your own disc image.
APP_AUTHOR := lom3ds

BUILD := build
COMMON_SRCS := $(wildcard src/*.c src/core/*.c src/ui/*.c src/port/*.c src/psx/*.c src/net/*.c)
# Where the in-app updater looks for new builds, and `make release` puts them.
REPO ?= Hobbergib/lom3ds
DEFINES := -DUPDATE_REPO=\"$(REPO)\"
INCLUDES := -Ithird_party/font8x8 $(DEFINES)
WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes

# --- Nintendo 3DS -----------------------------------------------------------
CC_3DS   := $(DEVKITARM)/bin/arm-none-eabi-gcc
NM_3DS   := $(DEVKITARM)/bin/arm-none-eabi-nm
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# -fno-strict-aliasing: the port reads the game's memory image through
# pointers of many types, which the optimiser must not assume apart.
CFLAGS_3DS := -std=gnu11 -O2 -g $(WARN) -fno-strict-aliasing -ffunction-sections -fdata-sections $(ARCH) \
              -D__3DS__ -DCURL_STATICLIB $(INCLUDES) -isystem $(CTRULIB)/include -isystem $(PORTLIBS)/include
# The game code uses four-byte enums (see tools/build_game.py); libctru does
# not, and nothing enum-typed crosses between them.
LDFLAGS_3DS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,--no-enum-size-warning -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
# The updater: curl with mbedTLS, and the root certificates it trusts.
LIBS_3DS := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt
SRCS_3DS := $(COMMON_SRCS) src/plat/script.c src/plat/plat_3ds.c
OBJS_3DS := $(SRCS_3DS:%.c=$(BUILD)/3ds/%.o)
GAME_3DS := $(BUILD)/3ds/game

# --- Host -------------------------------------------------------------------
# Linked low in memory like a 3DS program: the game stores 24-bit addresses.
CC_HOST := gcc
# Optimised like the 3DS build, so the host tests meet the same compiler.
CFLAGS_HOST := -m32 -std=gnu11 -O2 -g $(WARN) -fno-strict-aliasing -fno-pie $(INCLUDES)
LDFLAGS_HOST := -m32 -no-pie -Wl,-Ttext-segment=0x100000
LIBS_HOST := -lcurl -lm
SRCS_HOST := $(COMMON_SRCS) src/plat/script.c src/plat/plat_host.c
OBJS_HOST := $(SRCS_HOST:%.c=$(BUILD)/host/%.o)
GAME_HOST := $(BUILD)/host/game
TEST_SRCS := tests/test_core.c $(wildcard src/core/*.c)
TEST_OBJS := $(TEST_SRCS:%.c=$(BUILD)/test/%.o)
CFLAGS_TEST := -m32 -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=undefined
TEST_DATA := $(BUILD)/testdata

# The SDK entry points are declared by the game's headers, not ours.
$(BUILD)/3ds/src/psx/%.o $(BUILD)/host/src/psx/%.o: EXTRA := -Wno-missing-prototypes

.PHONY: all 3dsx host test shots release clean FORCE
all: 3dsx
3dsx: $(BUILD)/lom3ds.3dsx
host: $(BUILD)/host/lom3ds-host

# The game code: tools/build_game.py rebuilds only what changed.
$(BUILD)/%/game/modules.txt: FORCE
	python3 tools/build_game.py --decomp $(DECOMP) --out $(dir $@) --target $*

# main.c carries the build stamp the log starts with, so it is recompiled
# whenever anything else is.
$(BUILD)/3ds/build_id.h: $(filter-out %/src/main.o,$(OBJS_3DS)) $(GAME_3DS)/modules.txt
$(BUILD)/host/build_id.h: $(filter-out %/src/main.o,$(OBJS_HOST)) $(GAME_HOST)/modules.txt
$(BUILD)/3ds/build_id.h $(BUILD)/host/build_id.h:
	@mkdir -p $(dir $@)
	echo "#define BUILD_ID \"$$(date -u +%Y%m%d-%H%M%S)\"" > $@
$(BUILD)/3ds/src/main.o: $(BUILD)/3ds/build_id.h
$(BUILD)/host/src/main.o: $(BUILD)/host/build_id.h
$(BUILD)/3ds/src/main.o: private EXTRA := -include $(BUILD)/3ds/build_id.h
$(BUILD)/host/src/main.o: private EXTRA := -include $(BUILD)/host/build_id.h

$(BUILD)/3ds/ca_bundle.o: $(CA_BUNDLE)
	@mkdir -p $(dir $@)
	printf '.section .rodata\n.global ca_bundle\nca_bundle:\n.incbin "%s"\n.global ca_bundle_end\nca_bundle_end:\n.byte 0\n' "$<" \
		| $(CC_3DS) -x assembler -c - -o $@

# The button icons are drawn by a script; the result is kept in the tree.
src/port/button_icons.inc: tools/mkbuttons.py
	python3 tools/mkbuttons.py $@ assets/button_icons_preview.png
$(BUILD)/3ds/src/port/icons.o $(BUILD)/host/src/port/icons.o: src/port/button_icons.inc

# Placeholders for SDK functions the port does not implement yet.
$(BUILD)/3ds/stubs.c: $(GAME_3DS)/modules.txt $(OBJS_3DS)
	python3 tools/gen_stubs.py $(NM_3DS) $@ --want $$(cat $<) --have $(OBJS_3DS)
$(BUILD)/host/stubs.c: $(GAME_HOST)/modules.txt $(OBJS_HOST)
	python3 tools/gen_stubs.py nm $@ --want $$(cat $<) --have $(OBJS_HOST)

$(BUILD)/3ds/stubs.o: $(BUILD)/3ds/stubs.c
	$(CC_3DS) $(CFLAGS_3DS) -Isrc/psx -c $< -o $@
$(BUILD)/host/stubs.o: $(BUILD)/host/stubs.c
	$(CC_HOST) $(CFLAGS_HOST) -Isrc/psx -c $< -o $@

# Generated tables of the game's functions by PlayStation address.
$(BUILD)/3ds/psx_modules.o: $(GAME_3DS)/modules.txt
	$(CC_3DS) $(CFLAGS_3DS) -w -Isrc/psx -c $(GAME_3DS)/psx_modules.c -o $@
$(BUILD)/host/psx_modules.o: $(GAME_HOST)/modules.txt
	$(CC_HOST) $(CFLAGS_HOST) -w -Isrc/psx -c $(GAME_HOST)/psx_modules.c -o $@

$(BUILD)/3ds/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_3DS) $(CFLAGS_3DS) $(EXTRA) -MMD -MP -c $< -o $@

$(BUILD)/lom3ds.elf: $(OBJS_3DS) $(BUILD)/3ds/stubs.o $(BUILD)/3ds/psx_modules.o $(BUILD)/3ds/ca_bundle.o $(GAME_3DS)/modules.txt
	$(CC_3DS) $(LDFLAGS_3DS) $(OBJS_3DS) $(BUILD)/3ds/stubs.o $(BUILD)/3ds/psx_modules.o $(BUILD)/3ds/ca_bundle.o \
		$$(cat $(GAME_3DS)/modules.txt) $(GAME_3DS)/psx_syms.ld $(LIBS_3DS) -o $@

$(BUILD)/lom3ds.smdh: assets/icon.png Makefile
	$(DKP_TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

$(BUILD)/lom3ds.3dsx: $(BUILD)/lom3ds.elf $(BUILD)/lom3ds.smdh
	$(DKP_TOOLS)/3dsxtool $< $@ --smdh=$(BUILD)/lom3ds.smdh

$(BUILD)/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_HOST) $(EXTRA) -MMD -MP -c $< -o $@

$(BUILD)/host/lom3ds-host: $(OBJS_HOST) $(BUILD)/host/stubs.o $(BUILD)/host/psx_modules.o $(GAME_HOST)/modules.txt
	$(CC_HOST) $(LDFLAGS_HOST) $(OBJS_HOST) $(BUILD)/host/stubs.o $(BUILD)/host/psx_modules.o $$(cat $(GAME_HOST)/modules.txt) \
		$(GAME_HOST)/psx_syms.ld $(LIBS_HOST) -o $@

$(BUILD)/test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_TEST) -MMD -MP -c $< -o $@

$(BUILD)/test/test_core: $(TEST_OBJS)
	$(CC_HOST) $(CFLAGS_TEST) $^ -o $@

$(TEST_DATA)/expected.txt: tools/mktestiso.py
	@mkdir -p $(TEST_DATA)
	python3 tools/mktestiso.py $(TEST_DATA)

test: $(BUILD)/test/test_core $(BUILD)/host/lom3ds-host $(TEST_DATA)/expected.txt
	$(BUILD)/test/test_core $(TEST_DATA)
	tests/run_ui.sh $(BUILD)/host/lom3ds-host $(TEST_DATA) $(BUILD)/shots/ui

shots: test
	python3 tools/shots2png.py $(BUILD)/shots/ui

# Publishes the current build as the newest release, where the updater in
# the program finds it. Needs the GitHub CLI, signed in.
release: 3dsx
	@id=$$(sed 's/.*"\(.*\)"/\1/' $(BUILD)/3ds/build_id.h); \
	printf 'build %s\nsize %s\nsha256 %s\n' "$$id" "$$(stat -c %s $(BUILD)/lom3ds.3dsx)" \
		"$$(sha256sum $(BUILD)/lom3ds.3dsx | cut -d' ' -f1)" > $(BUILD)/version.txt; \
	cat $(BUILD)/version.txt; \
	gh release create "build-$$id" --repo $(REPO) --title "Build $$id" --notes "" --latest \
		$(BUILD)/lom3ds.3dsx $(BUILD)/version.txt

clean:
	rm -rf $(BUILD)

-include $(OBJS_3DS:.o=.d) $(OBJS_HOST:.o=.d) $(TEST_OBJS:.o=.d)

# --- Emulator testing (temporary, for this machine) --------------------------
# Azahar cannot see folders outside its virtual SD card, so `make emu-setup`
# links the disc image into it; `make emu` then starts the build there.
# Remove the link with `make emu-clean`. Override DISC or AZAHAR_SD as needed.
DISC      ?= $(HOME)/Documents/roms/Legend of Mana (USA)/Legend of Mana (USA)/Legend of Mana (USA).bin
AZAHAR_SD ?= $(HOME)/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc
EMU_DIR   := $(AZAHAR_SD)/3ds/lom3ds

.PHONY: emu-setup emu emu-clean
emu-setup:
	mkdir -p "$(EMU_DIR)"
	ln -sfn "$(DISC)" "$(EMU_DIR)/disc.bin"
	@echo "linked $(EMU_DIR)/disc.bin; the log will be $(EMU_DIR)/log.txt"

emu: 3dsx emu-setup
	flatpak run org.azahar_emu.Azahar "$(abspath $(BUILD)/lom3ds.3dsx)"

emu-clean:
	rm -f "$(EMU_DIR)/disc.bin"
