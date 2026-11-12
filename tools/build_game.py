#!/usr/bin/env python3
"""Builds the decompiled game code for the port.

The PlayStation build places every global at a fixed RAM address and loads
17 overlays into two shared address ranges. This script reproduces that
arrangement for a native build:

1. Copies the decompilation's sources, rewriting hard-coded PlayStation
   addresses into PSX_ADDR() and laying the port's replacement headers on top.
2. Compiles every file and links each module (the main executable and each
   overlay) into one relocatable object.
3. Renames every data symbol that has a PlayStation address to PSX_<address>.
   Modules that share an address range then agree by construction, and
   symbols with the same name in different overlays no longer clash.
4. Writes a linker script that places each PSX_<address> symbol inside the
   port's image of PlayStation RAM, and a report of what is still undefined.

Usage: build_game.py --decomp DIR --out DIR --target host|3ds [--jobs N]
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import shutil
import subprocess
import sys

import gen_relocs

PORT = pathlib.Path(__file__).resolve().parents[1]
OVERRIDES = PORT / "src" / "game" / "overrides"
PRELUDE = PORT / "src" / "game" / "prelude.h"

VERSION = "us"
MAIN_SYMBOLS = "slus_01013_symbol_addrs.txt"

# Files that need a header generated from the disc; not built yet.
SKIP_SOURCES = set()

# Game functions the port wraps. The definition is renamed game_<name> in
# its own object; callers elsewhere then reach the port's psx_<name>, which
# calls the original (see src/psx/reloc.c).
HOOKS = {
    "src/main/cdrom.c": ["cdrom_stream"],
    "src/overlays/movie/movie.c": ["movie_play"],
    "src/main/audio/game_audio.c": ["record_game_diagnostic"],
}

# Places where the game reads through a null pointer. On the console that
# reads the start of RAM; PSX_NULL_OK() (src/game/prelude.h) does the same.
# Each entry is text that must still occur in the file.
NULL_READS = {
    "src/main/audio/akao_voice.c": [
        "g_akao_seq_channel1->noise_mask",
        "g_akao_seq_channel1->reverb_mask",
        "g_akao_seq_channel1->pitch_mod_mask",
    ],
}

# Calls that pass fewer arguments than the callee takes. The original code
# got away with it because the missing arguments were still in the MIPS
# argument registers from the caller's own parameters; other processors do
# not work that way, so the arguments are written out. Found with a C23
# syntax check, where an empty parameter list means "no parameters".
# (file, original text, replacement); the original must occur exactly once.
ARGUMENT_FIXES = [
    ("src/overlays/wsel/wsel.c", "    wsel_init();", "    wsel_init(context);"),
    ("src/overlays/field/scene/field_interaction_start.c", "actor = field_find_actor_record();",
     "actor = field_find_actor_record(actor_id);"),
    ("src/overlays/field/scripts/field_script_ops.c",
     "void field_script_op_54(void)\n{\n    field_play_song_section();",
     "void field_script_op_54(s32 section_index, s32 channel_mask)\n{\n"
     "    field_play_song_section(section_index, channel_mask);"),
    # The other three arguments held whatever the caller's caller left behind.
    ("src/overlays/field/records/field_party_state.c", "field_revive_actor(record->meta.bytes.id);",
     "field_revive_actor(record->meta.bytes.id, 0, 0, 0);"),
]

# Calls into the port inserted where the game finishes loading a file that
# stores PlayStation addresses (see src/psx/reloc.c). Same format as above.
LOAD_NOTIFICATIONS = [
    ("src/overlays/field/scene/field_scene_load.c", "    texture = FIELD_MAP_HEADER->texture;\n",
     "    psx_field_map_loaded();\n    texture = FIELD_MAP_HEADER->texture;\n"),
]

# The field keeps a flag in the top bit of sprite frame pointers. On the
# console every RAM address has that bit set, and the game clears it to mark
# frames with 16-bit coordinates (the cleared address still reads the same
# memory there). The port's addresses have the bit clear, so the port sets
# it as the mark instead, tests the other way round, and takes it off again
# before the pointer is used. Every occurrence of the original is replaced.
POINTER_TAG_FIXES = [
    ("src/overlays/field/actors/field_actor_runtime.c",
     "(uintptr_t)FIELD_ANIMATION_FRAME_DATA(resource_base, frame_offsets) & ~FIELD_FRAME_DATA_8BIT",
     "(uintptr_t)FIELD_ANIMATION_FRAME_DATA(resource_base, frame_offsets) | FIELD_FRAME_DATA_8BIT"),
    ("src/overlays/field/actors/field_actor_runtime.c", "if (!(actor->frame_data & FIELD_FRAME_DATA_8BIT))",
     "if (actor->frame_data & FIELD_FRAME_DATA_8BIT)"),
    ("src/overlays/field/actors/field_actor_runtime.c",
     "field_render_effect_frame16((FieldMotionRecord*)actor, packet_cursor, ordering_table, (u8*)actor->frame_data,",
     "field_render_effect_frame16((FieldMotionRecord*)actor, packet_cursor, ordering_table, "
     "(u8*)(actor->frame_data & ~FIELD_FRAME_DATA_8BIT),"),
    ("src/overlays/field/render/field_effect_update.c", "if (!((uintptr_t)frame_data & FIELD_FRAME_DATA_8BIT))",
     "if ((uintptr_t)frame_data & FIELD_FRAME_DATA_8BIT)"),
    ("src/overlays/field/render/field_effect_update.c",
     "field_render_effect_frame16(effect, packet_cursor, ordering_table, frame_data,",
     "field_render_effect_frame16(effect, packet_cursor, ordering_table, "
     "(u8*)((uintptr_t)frame_data & ~FIELD_FRAME_DATA_8BIT),"),
    ("src/overlays/field/render/field_effect_update.c",
     "field_render_effect_sprite_frames(effect, packet_cursor, ordering_table, frame_data);",
     "field_render_effect_sprite_frames(effect, packet_cursor, ordering_table, "
     "(u8*)((uintptr_t)frame_data & ~FIELD_FRAME_DATA_8BIT));"),
]

# Address-shaped literals that are not addresses.
NOT_ADDRESSES = {"0x801FFFFF"}

ADDRESS_LITERAL = re.compile(r"\b0x(?:80[01][0-9A-Fa-f]{5}|1[Ff]80[0-9A-Fa-f]{4})\b")
SYMBOL_LINE = re.compile(r"^\s*([A-Za-z_.][\w.]*)\s*=\s*0x([0-9A-Fa-f]+)\s*;")
AUTO_DATA_NAME = re.compile(r"^D_([0-9A-F]{8})$")

COMMON_CFLAGS = [
    "-std=gnu89", "-g", "-w",
    "-fno-builtin", "-fsigned-char", "-fno-strict-aliasing", "-fwrapv", "-fcommon",
    "-fno-delete-null-pointer-checks", "-fno-toplevel-reorder",
    "-DM2CTX", "-DVERSION_US",
]

TARGETS = {
    "host": {
        # -malign-double: 64-bit members align to 8 bytes, as on the console.
        "cc": ["gcc", "-m32", "-O1", "-fno-pic", "-fno-stack-protector", "-malign-double"],
        "ld": ["ld", "-m", "elf_i386"],
        "nm": "nm",
        "objcopy": "objcopy",
    },
    "3ds": {
        # -fno-short-enums: this compiler otherwise packs enums into one
        # byte, and the game's structures have them as four, like the data.
        "cc": ["{DEVKITARM}/bin/arm-none-eabi-gcc", "-O2", "-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard",
               "-mtp=soft", "-fno-short-enums"],
        "ld": ["{DEVKITARM}/bin/arm-none-eabi-ld"],
        "nm": "{DEVKITARM}/bin/arm-none-eabi-nm",
        "objcopy": "{DEVKITARM}/bin/arm-none-eabi-objcopy",
    },
}


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"command failed: {' '.join(str(c) for c in cmd)}\n{r.stderr}")
    return r.stdout


def image_offset(addr):
    """Offset of a PlayStation address inside the port's RAM image.

    Must match PSX_ADDR() in src/game/prelude.h: main RAM maps directly; the
    scratchpad and the hardware registers borrow the unused first 64 KiB."""
    if 0x80000000 <= addr < 0x80200000:
        return addr - 0x80000000
    if 0x1F800000 <= addr < 0x1F800400:
        return 0x1000 + (addr - 0x1F800000)
    if 0x1F801000 <= addr < 0x1F803000:
        return 0x2000 + (addr - 0x1F801000)
    return None


def stage_sources(decomp, stage):
    """Copy src/ and include/ with addresses rewritten; returns the C files."""
    rewritten = []
    # Files the port replaces are staged from the overrides only; writing the
    # original first would touch them on every run and force a full rebuild.
    overridden = {p.relative_to(OVERRIDES) for p in OVERRIDES.rglob("*") if p.is_file()}
    for top in ("src", "include"):
        for path in sorted((decomp / top).rglob("*")):
            if not path.is_file() or path.suffix not in (".c", ".h"):
                continue
            rel = path.relative_to(decomp)
            if rel.parts[:2] == ("src", "psyq") or rel in overridden:
                continue
            text = path.read_text(encoding="utf-8", errors="surrogateescape")
            # Everything in the memory map is an address; elsewhere only
            # hardware registers are written as literals.
            only_hw = rel.name != "memory_map.h"

            def repl(m):
                lit = m.group(0)
                if lit in NOT_ADDRESSES or (only_hw and not lit.lower().startswith("0x1f80")):
                    return lit
                rewritten.append(f"{rel}: {lit}")
                return f"PSX_ADDR({lit})"

            new = "\n".join(line if line.lstrip().startswith(("*", "/*", "//")) else ADDRESS_LITERAL.sub(repl, line)
                            for line in text.split("\n"))
            for expr in NULL_READS.get(str(rel), []):
                base, member = expr.split("->")
                if expr not in new:
                    sys.exit(f"{rel}: expected to find {expr}")
                new = new.replace(expr, f"PSX_NULL_OK({base})->{member}")
            for path, before, after in ARGUMENT_FIXES + LOAD_NOTIFICATIONS:
                if path == str(rel):
                    if new.count(before) != 1:
                        sys.exit(f"{rel}: expected exactly one {before!r}")
                    new = new.replace(before, after)
            for path, before, after in POINTER_TAG_FIXES:
                if path == str(rel):
                    if before not in new:
                        sys.exit(f"{rel}: expected to find {before!r}")
                    new = new.replace(before, after)
            dst = stage / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists() or dst.read_text(encoding="utf-8", errors="surrogateescape") != new:
                dst.write_text(new, encoding="utf-8", errors="surrogateescape")
    for path in sorted(OVERRIDES.rglob("*")):
        if path.is_file():
            dst = stage / path.relative_to(OVERRIDES)
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists() or dst.read_bytes() != path.read_bytes():
                shutil.copyfile(path, dst)
    (stage / "rewritten_addresses.txt").write_text("\n".join(rewritten) + "\n")
    write_zukan_tables(decomp, stage)
    return sorted(p.relative_to(stage) for p in (stage / "src").rglob("*.c")
                  if not p.name.endswith(".inc.c") and str(p.relative_to(stage)) not in SKIP_SOURCES)


def write_zukan_tables(decomp, stage):
    """The encyclopedia copies four tables into local arrays. The decompilation
    builds their initialisers from the disc; here each element is read from
    the overlay as loaded, so the build needs no game data. The tables sit
    back to back and the last runs up to the code (it ends at a marker the
    game looks for)."""
    table = read_symbol_table(decomp, "zukan")
    names = ["g_zukan_category_ranges", "g_zukan_group_ranges", "g_zukan_entry_values", "g_zukan_display_order", "zukan_run"]
    addrs = [table[n] for n in names]
    macros = [("ZUKAN_CATEGORY_RANGES", "int", 4), ("ZUKAN_GROUP_RANGES", "int", 4),
              ("ZUKAN_RESOURCE_VALUES", "short", 2), ("ZUKAN_DISPLAY_ORDER", "unsigned short", 2)]
    text = "/* Generated by tools/build_game.py. */\n#ifndef ZUKAN_CATEGORY_TABLES_H\n#define ZUKAN_CATEGORY_TABLES_H\n\n"
    for (macro, ctype, size), start, end in zip(macros, addrs, addrs[1:]):
        count = (end - start) // size
        items = [f"((const {ctype} *)PSX_ADDR(0x{start:08X}))[{i}]" for i in range(count)]
        text += f"#define {macro} \\\n    " + ", \\\n    ".join(items) + "\n\n"
    text += "#endif\n"
    dst = stage / "include" / "zukan_category" / "tables.h"
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.read_text() != text:
        dst.write_text(text)


def module_of(src):
    parts = src.parts
    return "main" if parts[1] == "main" else parts[2]


def compile_all(tools, stage, objdir, sources, jobs):
    newest_header = max(p.stat().st_mtime for p in list(stage.rglob("*.h")) + list(stage.rglob("*.inc.c")) + [PRELUDE])
    flags = tools["cc"] + COMMON_CFLAGS + ["-include", str(PRELUDE), f"-I{stage}/include", f"-I{stage}/include/sdk"]

    def one(src):
        obj = objdir / src.with_suffix(".o")
        if obj.exists() and obj.stat().st_mtime > max(newest_header, (stage / src).stat().st_mtime):
            return None
        obj.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(flags + ["-c", str(src), "-o", str(obj)], cwd=stage, capture_output=True, text=True)
        if r.returncode:
            obj.unlink(missing_ok=True)
            return (src, r.stderr)
        for name in HOOKS.get(str(src), []):
            run([tools["objcopy"], f"--redefine-sym={name}=game_{name}", obj])
        return None

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        failures = [f for f in pool.map(one, sources) if f]
    if failures:
        for src, err in failures:
            first = next((line for line in err.split("\n") if "error" in line), err.strip().split("\n")[0])
            print(f"  {src}: {first}")
        sys.exit(f"{len(failures)} file(s) failed to compile")


def read_symbol_table(decomp, module):
    table = {}
    names = ["shared_symbol_addrs.txt", MAIN_SYMBOLS if module == "main" else f"{module}_symbol_addrs.txt"]
    for name in names:
        for line in (decomp / "config" / VERSION / "symbols" / name).read_text().split("\n"):
            m = SYMBOL_LINE.match(line)
            if m:
                table[m.group(1)] = int(m.group(2), 16)
    return table


def main_text_range(decomp):
    """PlayStation address range of the main executable's code, which holds
    the SDK: a symbol in it is a function the port must provide, not data."""
    text = (decomp / "config" / VERSION / "SLUS_010.13.yaml").read_text()
    entries = [(int(m.group(1), 16), m.group(2)) for m in re.finditer(r"- \[(0x[0-9A-Fa-f]+), *([\w.]+)", text)]
    start = min(off for off, kind in entries if kind == "c")
    end = min(off for off, kind in entries if kind == "data" and off > start)
    file_start = int(re.search(r"start: (0x[0-9A-Fa-f]+)\s+vram: 0x80010000", text).group(1), 16)
    return 0x80010000 + start - file_start, 0x80010000 + end - file_start


PRIMARY_SLOT, SECONDARY_SLOT, SLOTS_END = 0x8004FC70, 0x80140000, 0x80180000


def pointer_layouts(raw, tables, vrams):
    """{module: (slot addresses, typed spans)}: where each module's file
    holds addresses, gathered from every module's declarations of its data."""
    result = {name: (set(), []) for name in raw}
    for name, obj in raw.items():
        slots, spans = gen_relocs.module_layout(obj, tables[name])

        def owner(addr):
            if addr < PRIMARY_SLOT:
                return "main"
            if name == "main" or addr >= SLOTS_END:
                return None  # an overlay's data seen from outside, or a work area
            if (addr >= SECONDARY_SLOT) == (vrams[name] == SECONDARY_SLOT):
                return name
            # A sub-overlay naming data in the primary slot: it runs over FIELD.
            return "field" if addr < SECONDARY_SLOT else None

        for addr in slots:
            if owner(addr):
                result[owner(addr)][0].add(addr)
        for start, end in spans:
            if owner(start):
                result[owner(start)][1].append((start, end))
    return {name: (sorted(s), gen_relocs.merge_spans(p)) for name, (s, p) in result.items()}


def module_vram(decomp, name):
    text = (decomp / "config" / VERSION / "overlays" / f"{name.upper()}.BIN.yaml").read_text()
    return int(re.search(r"vram: (0x[0-9A-Fa-f]+)", text).group(1), 16)


def code_spans(decomp, name, vram, main_text):
    if name == "main":
        return [main_text]
    text = (decomp / "config" / VERSION / "overlays" / f"{name.upper()}.BIN.yaml").read_text()
    start = int(re.search(r"start: (0x[0-9A-Fa-f]+)", text).group(1), 16)
    entries = sorted((int(o, 16), k) for o, k in re.findall(r"- \[(0x[0-9A-Fa-f]+)(?:, *([\w.]+))?", text))
    spans = [(vram + o - start, vram + entries[i + 1][0] - start) for i, (o, k) in enumerate(entries[:-1]) if k == "c"]
    return [tuple(s) for s in gen_relocs.merge_spans(spans)]


def write_module_tables(out, decomp, modules, tables, definers, promoted, main_text, raw):
    """psx_modules.c: for each module, where it loads and the native function
    for each PlayStation function address, so pointers stored in the game's
    data can be redirected when the data is loaded."""
    c_name = re.compile(r"^[A-Za-z_]\w*$")
    decls, bodies, rows = set(), [], []
    vrams = {"main": 0x80010000}
    for name in modules:
        if name != "main":
            text = (decomp / "config" / VERSION / "overlays" / f"{name.upper()}.BIN.yaml").read_text()
            vrams[name] = int(re.search(r"vram: (0x[0-9A-Fa-f]+)", text).group(1), 16)
    layouts = pointer_layouts(raw, tables, vrams)
    for name in modules:
        vram = vrams[name]
        entries = {}
        for sym, addr in tables[name].items():
            if not c_name.match(sym) or addr in entries:
                continue
            if sym in promoted[name]:
                entries[addr] = f"{name}__{sym}"
            elif sym in definers:
                if len(definers[sym]) == 1:
                    entries[addr] = sym
                elif name in definers[sym]:
                    entries[addr] = f"{name}__{sym}"
            elif main_text[0] <= addr < main_text[1]:
                # An SDK function: present only if the port implements it.
                entries[addr] = f"psx_{sym}"
        for target in entries.values():
            decls.add(f"extern void {target}(void) __attribute__((weak));")
        bodies.append(f"static const PsxFunction {name}_functions[] = {{\n" +
                      "".join(f"    {{0x{a:08X}, {t}}},\n" for a, t in sorted(entries.items())) + "};\n")
        slots, typed = layouts[name]
        code = code_spans(decomp, name, vram, main_text)
        bodies.append(f"static const uint32_t {name}_slots[] = {{0, " + ", ".join(f"0x{a:08X}" for a in slots) + "};\n")
        bodies.append(f"static const PsxSpan {name}_typed[] = {{{{0, 0}}, " +
                      ", ".join(f"{{0x{a:08X}, 0x{b:08X}}}" for a, b in typed) + "};\n")
        bodies.append(f"static const PsxSpan {name}_code[] = {{{{0, 0}}, " +
                      ", ".join(f"{{0x{a:08X}, 0x{b:08X}}}" for a, b in code) + "};\n")
        ident = "NULL" if name == "main" else f"g_{name}_overlay_id"
        if name != "main":
            decls.add(f"extern const unsigned char g_{name}_overlay_id[4];")
        # Each table starts with a zero entry so that none is empty.
        rows.append(f'    {{"{name}", 0x{vram:08X}, {ident}, {name}_functions, {len(entries)}, {name}_slots + 1, '
                    f'{len(slots)}, {name}_typed + 1, {len(typed)}, {name}_code + 1, {len(code)}}},\n')
    text = ("/* Generated by tools/build_game.py. */\n#include \"psx.h\"\n\n" + "\n".join(sorted(decls)) + "\n\n" +
            "\n".join(bodies) + "\nconst PsxModule psx_modules[] = {\n" + "".join(rows) + "};\n" +
            f"const int psx_module_count = {len(rows)};\n")
    path = out / "psx_modules.c"
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def read_static_functions(tools, obj):
    """Names of file-local functions that occur exactly once in an object."""
    counts = {}
    for line in run([tools["nm"], obj]).split("\n"):
        parts = line.split()
        if len(parts) >= 3 and parts[-2] == "t":
            counts[parts[-1]] = counts.get(parts[-1], 0) + 1
    return {name for name, n in counts.items() if n == 1}


def read_nm(tools, obj):
    """[(name, type)] for every global symbol of an object."""
    out = []
    for line in run([tools["nm"], "-g", obj]).split("\n"):
        parts = line.split()
        if len(parts) >= 2:
            out.append((parts[-1], parts[-2]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--target", required=True, choices=TARGETS)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()

    devkitarm = os.environ.get("DEVKITARM", str(PORT.parent / "toolchain" / "devkitpro" / "devkitARM"))
    tools = {k: [x.replace("{DEVKITARM}", devkitarm) for x in v] if isinstance(v, list)
             else v.replace("{DEVKITARM}", devkitarm) for k, v in TARGETS[args.target].items()}
    decomp, out = args.decomp.resolve(), args.out.resolve()
    stage, objdir = out / "src", out / "obj"

    sources = stage_sources(decomp, stage)
    compile_all(tools, stage, objdir, sources, args.jobs)

    modules = {}
    for src in sources:
        modules.setdefault(module_of(src), []).append(objdir / src.with_suffix(".o"))

    # Nothing compiled since the modules were last linked: leave every output
    # alone, so make sees nothing to redo.
    stamp = out / "modules.txt"
    inputs = [o for objs in modules.values() for o in objs] + list((PORT / "tools").glob("*.py"))
    if stamp.exists() and all(p.stat().st_mtime <= stamp.stat().st_mtime for p in inputs):
        return

    raw = {}
    for name, objs in sorted(modules.items()):
        raw[name] = out / f"{name}.raw.o"
        run(tools["ld"] + ["-r", "-o", raw[name]] + objs)
    symbols = {name: read_nm(tools, obj) for name, obj in raw.items()}

    # A name is a function if any module defines it as code.
    definers = {}
    for name, syms in symbols.items():
        for sym, kind in syms:
            if kind == "T":
                definers.setdefault(sym, []).append(name)

    text_lo, text_hi = main_text_range(decomp)
    placed = {}        # PlayStation address -> a name it had, for the report
    unresolved = {}    # undefined name -> modules that want it
    native_data = []   # data defined by the C code although it has an address
    hooked = {name for names in HOOKS.values() for name in names}
    tables, promoted = {}, {}
    # Addresses of each module's own functions, to recognise a call another
    # module makes to one of them under a different name.
    function_addrs = {}
    for name in sorted(raw):
        tables[name] = read_symbol_table(decomp, name)
        own = {sym for sym, kind in symbols[name] if kind == "T"} | read_static_functions(tools, raw[name])
        function_addrs[name] = {tables[name][sym] for sym in own if sym in tables[name]}
    aliases = {}       # PlayStation address -> a name it is called by
    for name in sorted(raw):
        table = tables[name]
        renames = []
        own_code = [] if name == "main" else code_spans(decomp, name, module_vram(decomp, name), (text_lo, text_hi))
        for sym, kind in symbols[name]:
            auto = AUTO_DATA_NAME.match(sym)
            addr = int(auto.group(1), 16) if auto else table.get(sym)
            if addr is not None and (text_lo <= addr < text_hi or sym in hooked):
                addr = None
            if kind == "T":
                # The same function compiled into several overlays: each keeps its own.
                if len(definers[sym]) > 1:
                    renames.append((sym, f"{name}__{sym}"))
            elif (kind == "U" and sym not in definers and addr is not None and addr >= PRIMARY_SLOT
                  and not sym.startswith("g_") and not AUTO_DATA_NAME.match(sym)
                  and (name == "main" or (addr >= SECONDARY_SLOT) != (module_vram(decomp, name) >= SECONDARY_SLOT))
                  and any(addr in function_addrs[other] for other in raw if other != name)):
                # An entry point of whichever overlay is loaded in the other
                # slot, called by a name of this module's own: found at run
                # time (see psx_lookup_function).
                renames.append((sym, f"alias_{addr:08X}"))
                aliases.setdefault(addr, sym)
            elif kind in "UC" and sym not in definers and addr is not None and image_offset(addr) is not None:
                # A name inside this module's own code is a function whose
                # source was not built; as data it would be jumped into.
                if name != "main" and any(lo <= addr < hi for lo, hi in own_code):
                    sys.exit(f"{name}: {sym} at {addr:08X} is code, but no source file defines it")
                renames.append((sym, f"PSX_{addr:08X}"))
                placed.setdefault(addr, sym)
            elif kind == "U":
                # What is left is the SDK. A prefix keeps those names apart
                # from the host C library's (open, read, rand, ...).
                # Compiler helpers (__aeabi_idiv, __divdi3, ...) come from libgcc.
                if sym not in definers and not (sym.startswith("__") and sym != "__main"):
                    unresolved.setdefault(sym, []).append(name)
                    if not sym.startswith("psx_"):
                        renames.append((sym, f"psx_{sym}"))
            elif addr is not None:
                native_data.append(f"{name}: {sym} ({kind}) = {addr:08X}")
        # Static functions with a known address may be the target of a
        # pointer stored in the game's data: make them reachable by the
        # generated function tables, under a per-module name.
        globals_here = {sym for sym, _ in symbols[name]}
        promoted[name] = sorted(s for s in read_static_functions(tools, raw[name])
                                if s in table and s not in globals_here and image_offset(table[s]) is not None)
        renames += [(s, f"{name}__{s}") for s in promoted[name]]
        redefine = out / f"{name}.redefine"
        redefine.write_text("".join(f"{a} {b}\n" for a, b in renames))
        # Two passes, so the renames see the promoted symbols as globals.
        # objcopy rejects an empty symbol list file.
        staged = out / f"{name}.promoted.o"
        promote = out / f"{name}.promote"
        promote.write_text("".join(f"{s}\n" for s in promoted[name]))
        run([tools["objcopy"]] + ([f"--globalize-symbols={promote}"] if promoted[name] else []) + [raw[name], staged])
        run([tools["objcopy"]] + ([f"--redefine-syms={redefine}"] if renames else []) + [staged, out / f"{name}.o"])

    write_module_tables(out, decomp, sorted(raw), tables, definers, promoted, (text_lo, text_hi), raw)
    with open(out / "psx_modules.c", "a") as f:
        f.write("\n/* Calls into whichever overlay is loaded, by PlayStation address. The\n"
                " * arguments pass through untouched. */\n"
                "void (*psx_lookup_function(uint32_t addr))(void);\n")
        for addr, sym in sorted(aliases.items()):
            f.write(f"/* {sym} */\n__attribute__((naked)) void alias_{addr:08X}(void)\n{{\n"
                    "#ifdef __arm__\n"
                    f"    __asm__(\"push {{r0-r4, lr}}\\n ldr r0, =0x{addr:08X}\\n bl psx_lookup_function\\n\"\n"
                    "            \" mov r12, r0\\n pop {r0-r4, lr}\\n bx r12\\n .ltorg\");\n"
                    "#else\n"
                    f"    __asm__(\"subl $8, %esp\\n pushl $0x{addr:08X}\\n call psx_lookup_function\\n\"\n"
                    "            \" addl $12, %esp\\n jmp *%eax\");\n"
                    "#endif\n}\n")
    print("  run-time overlay calls: " + ", ".join(f"{sym}={addr:08X}" for addr, sym in sorted(aliases.items())))

    # A duplicated function cannot serve another module's reference.
    ambiguous = sorted(s for s in unresolved if len(definers.get(s, [])) > 1)

    with open(out / "psx_syms.ld", "w") as f:
        f.write("/* Generated by tools/build_game.py: game data symbols inside the RAM image. */\n")
        for addr in sorted(placed):
            f.write(f"PSX_{addr:08X} = psx_ram + 0x{image_offset(addr):X};\n")
    (out / "unresolved.txt").write_text("".join(f"{s}  [{', '.join(m)}]\n" for s, m in sorted(unresolved.items())))
    (out / "native_data.txt").write_text("\n".join(native_data) + "\n")
    (out / "modules.txt").write_text("".join(f"{out / name}.o\n" for name in sorted(raw)))

    print(f"game: {len(sources)} files, {len(raw)} modules, {len(placed)} placed symbols, "
          f"{len(unresolved)} unresolved, {len(native_data)} native data symbols with an address")
    if ambiguous:
        print("  ambiguous cross-module references: " + ", ".join(ambiguous))


if __name__ == "__main__":
    main()
