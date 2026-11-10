"""Cross-references over the recompiled guest code in generated/default.

Codegen keeps each PowerPC instruction as a comment above its C++, so the
call graph and the memory operands can be read back from the .cpp files.
The index is cached in out/ppc_xref.pickle and rebuilt when generated/
changes.

usage: python -I tools/ppc_xref.py <command> [args]
  callers <addr>            functions that bl to <addr>
  callees <addr>            functions <addr> calls with bl
  asm <addr>                instruction listing of <addr>
  tree <addr> [depth]       callers of <addr>, recursively
  stores <base> <lo> <hi>   functions storing to <base>+off, lo<=off<=hi,
                            where <base> is the register holding the device
                            on entry (r3 by default convention)
  range <lo> <hi>           functions whose start lies in [lo, hi]
  imm <value>               functions loading an immediate (lis/li/ori)
  ptrs <lo> <hi>            .rdata words in the XEX pointing into [lo, hi]
                            (needs game/default.xex decrypted image, see below)

Addresses are hex without 0x (e.g. 8219E530).
"""
import os
import pickle
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "generated", "default")
CACHE = os.path.join(ROOT, "out", "ppc_xref.pickle")

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
ASM_RE = re.compile(r"^\t// (\S+)\s*(.*)$")
LABEL_RE = re.compile(r"^loc_([0-9A-F]{8}):")


def build():
    funcs = {}
    files = sorted(f for f in os.listdir(GEN) if f.startswith("broken_bond_recomp.") and f.endswith(".cpp"))
    for name in files:
        current = None
        with open(os.path.join(GEN, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    current = []
                    funcs[int(m.group(1), 16)] = current
                    continue
                if current is None:
                    continue
                m = ASM_RE.match(line)
                if m:
                    current.append((m.group(1), m.group(2)))
                    continue
                m = LABEL_RE.match(line)
                if m:
                    current.append(("loc", m.group(1)))
    calls = {}
    for addr, asm in funcs.items():
        out = set()
        for op, args in asm:
            if op == "bl":
                out.add(int(args, 16))
        calls[addr] = out
    callers = {}
    for addr, out in calls.items():
        for target in out:
            callers.setdefault(target, set()).add(addr)
    stamp = max(os.path.getmtime(os.path.join(GEN, f)) for f in files)
    return {"stamp": stamp, "funcs": funcs, "calls": calls, "callers": callers}


def load():
    files = [f for f in os.listdir(GEN) if f.startswith("broken_bond_recomp.") and f.endswith(".cpp")]
    stamp = max(os.path.getmtime(os.path.join(GEN, f)) for f in files)
    if os.path.exists(CACHE):
        with open(CACHE, "rb") as f:
            data = pickle.load(f)
        if data["stamp"] == stamp:
            return data
    data = build()
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, "wb") as f:
        pickle.dump(data, f)
    return data


def h(addr):
    return f"{addr:08X}"


MEM_RE = re.compile(r"^(r\d+),(-?\d+)\((r\d+)\)$")


def stores_to(asm, base, lo, hi, loads=False):
    """Offsets stored (or also loaded) relative to <base> before <base> is
    overwritten.

    A cheap forward scan: tracks registers that are plain copies of <base>
    (mr rX,base) and stops tracking a register once something else writes it.
    """
    aliases = {base}
    hits = set()
    for op, args in asm:
        if op == "loc":
            continue
        parts = args.split(",")
        is_store = op.startswith("st")
        is_load = loads and op.startswith("l") and op not in ("li", "lis")
        if is_store or is_load:
            m = MEM_RE.match(args)
            if m and m.group(3) in aliases:
                off = int(m.group(2))
                if lo <= off <= hi:
                    hits.add(off)
            if is_store:
                continue
        if op == "mr" and len(parts) == 2:
            if parts[1] in aliases:
                aliases.add(parts[0])
            else:
                aliases.discard(parts[0])
            continue
        if parts and re.fullmatch(r"r\d+", parts[0]) and not op.startswith(("cmp", "b", "mt")):
            aliases.discard(parts[0])
    return hits


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd, args = sys.argv[1], sys.argv[2:]
    data = load()
    funcs, calls, callers = data["funcs"], data["calls"], data["callers"]
    if cmd == "callers":
        for c in sorted(callers.get(int(args[0], 16), ())):
            print(h(c))
    elif cmd == "callees":
        for c in sorted(calls.get(int(args[0], 16), ())):
            print(h(c))
    elif cmd == "asm":
        for op, a in funcs[int(args[0], 16)]:
            print(f"{op}:" if op == "loc" and False else (f"loc_{a}:" if op == "loc" else f"    {op} {a}"))
    elif cmd == "tree":
        depth = int(args[1]) if len(args) > 1 else 3
        seen = set()

        def walk(addr, d):
            for c in sorted(callers.get(addr, ())):
                print("  " * d + h(c) + ("  (seen)" if c in seen else ""))
                if c not in seen and d + 1 < depth:
                    seen.add(c)
                    walk(c, d + 1)

        print(h(int(args[0], 16)))
        walk(int(args[0], 16), 1)
    elif cmd == "stores":
        base, lo, hi = args[0], int(args[1]), int(args[2])
        for addr in sorted(funcs):
            hits = stores_to(funcs[addr], base, lo, hi)
            if hits:
                print(h(addr), " ".join(str(o) for o in sorted(hits)))
    elif cmd == "touches":
        # Like stores, but loads count too: functions that use <base> as a
        # pointer into [lo, hi].
        base, lo, hi = args[0], int(args[1]), int(args[2])
        for addr in sorted(funcs):
            hits = stores_to(funcs[addr], base, lo, hi, loads=True)
            if hits:
                print(h(addr), len(funcs[addr]), "insns", len(callers.get(addr, ())), "callers",
                      len(hits), "offsets", " ".join(str(o) for o in sorted(hits)[:12]))
    elif cmd == "entries":
        # Functions in [lo, hi] that code outside [lo, hi] calls: the API
        # surface of a statically linked library.
        lo, hi = int(args[0], 16), int(args[1], 16)
        for addr in sorted(funcs):
            if not lo <= addr <= hi:
                continue
            outside = sorted(c for c in callers.get(addr, ()) if not lo <= c <= hi)
            if outside:
                inside = len(callers.get(addr, ())) - len(outside)
                print(h(addr), len(funcs[addr]), "insns", len(outside), "outside", inside, "inside",
                      " ".join(h(c) for c in outside[:6]))
    elif cmd == "range":
        lo, hi = int(args[0], 16), int(args[1], 16)
        for addr in sorted(funcs):
            if lo <= addr <= hi:
                print(h(addr), len(funcs[addr]), "insns", len(callers.get(addr, ())), "callers")
    elif cmd == "imm":
        value = args[0].lower()
        for addr in sorted(funcs):
            for op, a in funcs[addr]:
                if op in ("lis", "li", "ori", "oris", "addi", "addis") and a.lower().endswith("," + value):
                    print(h(addr), op, a)
                    break
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
