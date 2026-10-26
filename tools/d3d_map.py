"""Tables for docs/D3D_MAP.md from the recompiled guest D3D library.

usage: python -I tools/d3d_map.py <census log> [shadow | states]

shadow  the GPU register shadow inside the D3D device: each block sub_821AE158
        flushes (device offset, first register, count) with Xenos names from
        the SDK's register_table.inc.
states  the render-state and sampler-state tables sub_821B38F8 applies
        (dumped by --d3d_census), with each setter's device fields and bits.

The census log supplies the two state tables; they live in guest memory at
0x82CD5E00 and 0x82CD62C0 and are not in generated/.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc_xref  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTERS = os.path.join(ROOT, "thirdparty", "rexglue-sdk", "include", "rex", "graphics", "register_table.inc")

# (device offset, first register, count, dirty qword at device+N, bits) as
# read from sub_821AE158; counts are the widths of its masks.
SHADOW_BLOCKS = [
    (1152, 0x4800, 32 * 6, 24, "0-31, one per fetch slot (6 dwords)"),
    (1920, 0x4000, 256 * 4, 0, "one per float4"),
    (6016, 0x4400, 256 * 4, 8, "one per float4"),
    (10112, 0x4900, 8, 32, "56, all 8 at once (mask 0xFF000000)"),
    (10368, 0x2000, 16, 16, "42-57"),
    (10444, 0x2100, 21, 16, "21-41"),
    (10528, 0x2180, 5, 16, "12-16"),
    (10548, 0x2200, 12, 16, "0-11"),
    (10596, 0x2280, 21, 24, "34-54"),
    (10680, 0x2300, 38, 32, "0-37"),
    (10832, 0x2380, 8, 32, "38-45"),
]


def register_names():
    names = {}
    with open(REGISTERS, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)", line)
            if m:
                names[int(m.group(1), 16)] = m.group(2)
    return names


REGISTER_FIELDS = os.path.join(ROOT, "thirdparty", "rexglue-sdk", "include", "rex", "graphics", "registers.h")


def register_fields():
    """{register name: {(lo, width): field}} from the SDK's bitfield unions."""
    fields = {}
    current = None
    with open(REGISTER_FIELDS, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"union alignas\(uint32_t\) (\w+) \{", line)
            if m:
                current = fields.setdefault(m.group(1), {})
                continue
            if current is None:
                continue
            m = re.match(r"\s+[\w:]+ (\w+) : (\d+);\s*// \+(\d+)", line)
            if m and not m.group(1).startswith("_pad"):
                current[(int(m.group(3)), int(m.group(2)))] = m.group(1)
            if line.startswith("};"):
                current = None
    return fields


def shadow_register(offset):
    """(GPU register, byte within it) a device offset shadows, or None."""
    for base, first, count, _, _ in SHADOW_BLOCKS:
        if first in (0x4000, 0x4400, 0x4800):
            continue
        if base <= offset < base + count * 4:
            return first + (offset - base) // 4, (offset - base) % 4
    return None


MEM = re.compile(r"^(l\w+|st\w+) (r\d+),(-?\d+)\(r3\)$")
RLWIMI = re.compile(r"^rlwimi (r\d+),(r\d+),(\d+),(\d+),(\d+)$")
STORE_WIDTH = {"stb": 8, "sth": 16, "stw": 32}


def setter_fields(asm, names, fields):
    """Device fields a render-state setter writes, with the bits the value lands in."""
    out = []
    inserts = {}
    for op, args in asm:
        insn = f"{op} {args}"
        m = RLWIMI.match(insn)
        if m:
            dst, src, mb, me = m.group(1), m.group(2), int(m.group(4)), int(m.group(5))
            if src == "r4" or dst == "r4":
                # PowerPC numbers bits mb..me from the MSB of the word.
                inserts[dst if src == "r4" else src] = (31 - me, me - mb + 1)
            continue
        m = MEM.match(insn)
        if not (m and m.group(1) in STORE_WIDTH):
            continue
        reg, off = m.group(2), int(m.group(3))
        if off in (0, 8, 16, 24, 32, 40):
            continue  # dirty masks
        target = shadow_register(off)
        if target is None:
            label, reg_name, shift = f"device+{off}", None, 0
        else:
            index, byte = target
            reg_name = names.get(index, f"{index:#06x}")
            # The guest is big-endian: byte 0 of a dword holds bits 24-31.
            shift = (4 - byte - STORE_WIDTH[m.group(1)] // 8) * 8
            label = reg_name if byte == 0 and m.group(1) == "stw" else f"{reg_name} byte {byte}"
        if reg in inserts:
            lo, width = inserts[reg]
            lo += shift
        elif reg == "r4":
            lo, width = shift, STORE_WIDTH[m.group(1)]
        else:
            out.append(label)
            continue
        field = fields.get(reg_name, {}).get((lo, width)) if reg_name else None
        bits = f"bits {lo}-{lo + width - 1}" if width > 1 else f"bit {lo}"
        out.append(f"{label}.{field}" if field else f"{label} {bits}")
    return "; ".join(dict.fromkeys(out)) or "(no device field)"


def main():
    log, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "shadow"
    names = register_names()
    if mode == "shadow":
        print("| device offset | registers | count | dirty qword | bits |")
        print("|---|---|---|---|---|")
        for base, first, count, dirty, bits in SHADOW_BLOCKS:
            last = first + count - 1
            print(f"| +{base} | `{first:#06x}` {names.get(first, '')} .. `{last:#06x}` {names.get(last, '')} "
                  f"| {count} | +{dirty} | {bits} |")
        return
    rows = []
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"d3d (renderstate|samplerstate)\s+(\d+) tag=(\w+) setter=(\w+) default=(\w+)", line)
            if m:
                rows.append(m.groups())
    seen = set()
    fields = register_fields()
    funcs = ppc_xref.load()["funcs"]
    print("| table | index | setter | default | writes |")
    print("|---|---|---|---|---|")
    for table, index, getter, setter, default in rows:
        if (table, index) in seen:
            continue
        seen.add((table, index))
        writes = setter_fields(funcs[int(setter, 16)], names, fields) if setter != "82492D68" else "no-op"
        print(f"| {table[:-5]} | {index} | `{setter}` | `{default}` | {writes} |")


main()
