# ida_d3d_find.py — derive Monster Madness's D3D device methods from its own code.
#
# Every command-emitting 360 D3D method reserves push-buffer space via a small
# helper (here sub_8220A2B8, and a variant sub_822091A8 used by Swap), then
# writes PM4 packets. So the callers of those reserve helpers ARE the D3D device
# command methods. We list them with size, caller-count, and the PM4 type-3
# opcodes they emit (extracted from the function's immediate constants) so the
# Draw / SetConstant / etc. methods can be told apart.
#
# Batch: set IDA_DUMP_BATCH=1 & idat.exe -A -Stools\ida_d3d_find.py <db_copy.i64>
# Output: assets\ida_dump\d3d_methods.txt

import os
import idautils
import idc
import ida_funcs
import ida_bytes

OUT_DIR = os.path.join(os.path.dirname(idc.get_idb_path()), "ida_dump")
os.makedirs(OUT_DIR, exist_ok=True)

RESERVE_HELPERS = [0x8220A2B8, 0x822091A8]

# PM4 type-3 opcode -> name (subset relevant to identifying D3D methods).
PM4_OP = {
    0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x2D: "SET_CONSTANT", 0x2E: "SET_CONSTANT2",
    0x19: "ME_INIT", 0x10: "NOP", 0x3F: "INTERRUPT", 0x55: "INVALIDATE_STATE",
    0x12: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x21: "INDIRECT_BUFFER", 0x3C: "WAIT_REG_MEM",
    0x46: "EVENT_WRITE", 0x21: "INDIRECT", 0x1D: "SET_SHADER_CONSTANTS",
}


def caller_funcs(target):
    out = set()
    for xr in idautils.XrefsTo(target, 0):
        fn = ida_funcs.get_func(xr.frm)
        if fn:
            out.add(fn.start_ea)
    return out


def func_xref_count(ea):
    return sum(1 for _ in idautils.XrefsTo(ea, 0))


def pm4_opcodes_in(ea):
    """Scan a function's instruction immediates for PM4 type-3 headers (0xC0xx_xxxx)
    and extract opcode bits [8:15]. Reconstructs lis+ori/addi 32-bit immediates."""
    fn = ida_funcs.get_func(ea)
    if not fn:
        return set()
    highs = {}  # reg -> high<<16 from lis
    ops = set()
    for head in idautils.Heads(fn.start_ea, fn.end_ea):
        mnem = idc.print_insn_mnem(head)
        if mnem in ("lis",):
            reg = idc.get_operand_value(head, 0)
            hi = idc.get_operand_value(head, 1) & 0xFFFF
            highs[reg] = hi << 16
        elif mnem in ("ori", "addi", "addic"):
            base = idc.get_operand_value(head, 1)
            lo = idc.get_operand_value(head, 2) & 0xFFFF
            full = highs.get(base, 0) | lo
            if (full >> 24) == 0xC0:
                ops.add((full >> 8) & 0x7F)
        elif mnem == "li":
            v = idc.get_operand_value(head, 1) & 0xFFFFFFFF
            if (v >> 24) == 0xC0:
                ops.add((v >> 8) & 0x7F)
    return ops


candidates = set()
for h in RESERVE_HELPERS:
    candidates |= caller_funcs(h)

rows = []
for ea in candidates:
    fn = ida_funcs.get_func(ea)
    size = (fn.end_ea - fn.start_ea) if fn else 0
    ops = pm4_opcodes_in(ea)
    op_names = ",".join(sorted(PM4_OP.get(o, "op%02X" % o) for o in ops)) or "-"
    rows.append((func_xref_count(ea), size, ea, op_names))

rows.sort(reverse=True)  # most-called first (Draw/Set are hot)
with open(os.path.join(OUT_DIR, "d3d_methods.txt"), "w", encoding="utf-8") as f:
    f.write("# callers=%d  fmt: <xref_count>\t<size>\t<ea>\t<pm4_opcodes>\n" % len(candidates))
    for xc, size, ea, ops in rows:
        f.write("%d\t%d\t0x%08X\t%s\n" % (xc, size, ea, ops))

print("[ida_d3d_find] %d D3D-method candidates -> d3d_methods.txt" % len(candidates))

if os.environ.get("IDA_DUMP_BATCH"):
    import ida_pro
    ida_pro.qexit(0)
