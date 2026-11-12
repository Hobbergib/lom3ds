"""Finds where the game's data holds addresses, from the decompilation's own
type declarations.

The executable and overlays store PlayStation addresses that the port must
rewrite when it loads them (src/psx/reloc.c). Guessing them by value damages
ordinary data: tables of 16-bit pairs often read as 0x8005000A and the like.
The compiled game objects carry debug information describing every data
symbol the code refers to, so for each of them this module works out

- the pointer slots: addresses of the fields typed as pointers, and
- the typed span: the bytes the declaration covers, which are known not to
  hold a pointer anywhere else.

Data no declaration covers stays "untyped"; the loader treats it with a
strict value test instead.
"""
import re
import subprocess

DIE_LINE = re.compile(r"^(0x[0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)")
ATTR_LINE = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")
AUTO_DATA_NAME = re.compile(r"^D_([0-9A-F]{8})$")
POINTER_SIZE = 4
# An array declared without a length runs to the next symbol, but no further
# than this.
MAX_OPEN_ARRAY = 0x10000


class Die:
    __slots__ = ("tag", "attrs", "children")

    def __init__(self, tag):
        self.tag, self.attrs, self.children = tag, {}, []


def parse_debug_info(obj):
    """{offset: Die} for an object's .debug_info, plus the top-level variables."""
    text = subprocess.run(["llvm-dwarfdump", "--debug-info", str(obj)], capture_output=True, text=True, check=True).stdout
    dies, variables, stack, current = {}, [], [], None
    for line in text.split("\n"):
        m = DIE_LINE.match(line)
        if m:
            depth = (len(m.group(2)) - 1) // 2  # the unit is depth 0, its children depth 1
            if m.group(3) == "NULL":
                current = None
                continue
            current = Die(m.group(3))
            dies[int(m.group(1), 16)] = current
            del stack[depth:]
            if stack:
                stack[-1].children.append(current)
            stack.append(current)
            if depth == 1 and current.tag == "DW_TAG_variable":
                variables.append(current)
            continue
        m = ATTR_LINE.match(line)
        if m and current is not None:
            current.attrs[m.group(1)] = m.group(2)
    return dies, variables


def number(text):
    return int(text.split()[0], 0)


def name_of(die):
    value = die.attrs.get("DW_AT_name")
    return value.strip('"') if value else None


class Layouts:
    """Size and pointer offsets of each type, worked out once per type."""

    def __init__(self, dies):
        self.dies, self.cache = dies, {}

    def of(self, ref):
        """(size or None if open-ended, element size, pointer offsets) of a type."""
        if ref not in self.cache:
            self.cache[ref] = (0, 0, ())  # stops a self-referential type
            self.cache[ref] = self.compute(self.dies.get(ref))
        return self.cache[ref]

    def type_of(self, die):
        ref = die.attrs.get("DW_AT_type")
        return self.of(number(ref)) if ref else (0, 0, ())

    def compute(self, die):
        if die is None:
            return (0, 0, ())
        tag = die.tag
        if tag in ("DW_TAG_pointer_type", "DW_TAG_reference_type"):
            return (POINTER_SIZE, POINTER_SIZE, (0,))
        if tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type"):
            return self.type_of(die)
        if tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_class_type"):
            size = number(die.attrs.get("DW_AT_byte_size", "0"))
            offsets = set()
            for member in die.children:
                if member.tag != "DW_TAG_member" or "DW_AT_bit_size" in member.attrs:
                    continue
                at = number(member.attrs.get("DW_AT_data_member_location", "0"))
                msize, _, moffsets = self.type_of(member)
                if msize is not None:
                    offsets.update(at + o for o in moffsets)
            return (size, size, tuple(sorted(offsets)))
        if tag == "DW_TAG_array_type":
            esize, _, eoffsets = self.type_of(die)
            count = 1
            for sub in die.children:
                if sub.tag != "DW_TAG_subrange_type":
                    continue
                if "DW_AT_count" in sub.attrs:
                    count *= number(sub.attrs["DW_AT_count"])
                elif "DW_AT_upper_bound" in sub.attrs:
                    count *= number(sub.attrs["DW_AT_upper_bound"]) + 1
                else:
                    count = None
                    break
            if esize is None or not esize:
                return (0, 0, ())
            if count is None:
                return (None, esize, eoffsets)
            return (esize * count, esize, tuple(i * esize + o for i in range(count) for o in eoffsets))
        # Base types, enumerations, subroutine types: no pointers inside.
        return (number(die.attrs.get("DW_AT_byte_size", "0")), 0, ())


def module_layout(obj, table):
    """(pointer slot addresses, typed spans) for the data symbols an object
    declares. `table` maps symbol names to PlayStation addresses."""
    dies, variables = parse_debug_info(obj)
    layouts = Layouts(dies)
    boundaries = sorted(set(table.values()))
    slots, spans = set(), []
    for var in variables:
        name = name_of(var)
        if not name:
            continue
        auto = AUTO_DATA_NAME.match(name)
        addr = int(auto.group(1), 16) if auto else table.get(name)
        if addr is None:
            continue
        size, esize, offsets = layouts.type_of(var)
        if size is None:
            # Open-ended array: whole elements up to the next symbol.
            following = next((b for b in boundaries if b > addr), addr)
            count = min(following - addr, MAX_OPEN_ARRAY) // esize
            size = count * esize
            offsets = tuple(i * esize + o for i in range(count) for o in offsets)
        if size:
            spans.append((addr, addr + size))
            slots.update(addr + o for o in offsets)
    return slots, spans


def merge_spans(spans):
    out = []
    for start, end in sorted(spans):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return out
