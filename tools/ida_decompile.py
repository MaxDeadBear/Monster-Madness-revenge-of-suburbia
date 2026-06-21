# ida_decompile.py — dump Hex-Rays pseudocode for a set of target functions and
# list their direct callees, to map the D3D device-init / present path.
#
# Batch:  set IDA_DUMP_BATCH=1 & idat.exe -A -Stools\ida_decompile.py <db_copy.i64>
# Output: assets\ida_dump\decompiled\<ea>.c  and  callees.txt

import os
import idautils
import idc
import ida_funcs
import ida_hexrays

OUT_DIR = os.path.join(os.path.dirname(idc.get_idb_path()), "ida_dump", "decompiled")
os.makedirs(OUT_DIR, exist_ok=True)

TARGETS = [
    0x82219718,  # RT-slot read+write (SetRenderTarget candidate)
    0x82219460,  # RT-slot writer
]

ida_hexrays.init_hexrays_plugin()


def callees(ea):
    out = []
    fn = ida_funcs.get_func(ea)
    if not fn:
        return out
    for head in idautils.Heads(fn.start_ea, fn.end_ea):
        for xr in idautils.XrefsFrom(head, 0):
            if xr.type in (idc.fl_CN, idc.fl_CF):  # near/far call
                tname = idc.get_func_name(xr.to) or ("sub_%08X" % xr.to)
                out.append((xr.to, tname))
    # dedupe, keep order
    seen, uniq = set(), []
    for a, n in out:
        if a not in seen:
            seen.add(a)
            uniq.append((a, n))
    return uniq


cf = open(os.path.join(OUT_DIR, "..", "callees.txt"), "w", encoding="utf-8", errors="replace")
for ea in TARGETS:
    name = idc.get_func_name(ea) or ("sub_%08X" % ea)
    try:
        cfunc = ida_hexrays.decompile(ea)
        text = str(cfunc) if cfunc else "// decompile failed"
    except Exception as e:
        text = "// decompile exception: %s" % e
    with open(os.path.join(OUT_DIR, "%08X.c" % ea), "w", encoding="utf-8", errors="replace") as f:
        f.write("// %s @ 0x%08X\n\n" % (name, ea))
        f.write(text)
    for tea, tname in callees(ea):
        cf.write("0x%08X\t%s\t0x%08X\t%s\n" % (ea, name, tea, tname))
cf.close()

print("[ida_decompile] dumped %d functions" % len(TARGETS))

if os.environ.get("IDA_DUMP_BATCH"):
    import ida_pro
    ida_pro.qexit(0)
