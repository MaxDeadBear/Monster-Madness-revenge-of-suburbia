# ida_xrefs.py — climb the call graph from the GPU seam to find the real D3D
# entry points (which carry no strings on 360). For a curated set of "anchor"
# addresses (Vd* kernel-import thunks + the D3D runtime internals found via
# strings), list every function that calls into them.
#
# Batch:  set IDA_DUMP_BATCH=1 then
#   idat.exe -A -Stools\ida_xrefs.py <db_copy.i64>
# Output: assets\ida_dump\xrefs.txt   ->  <anchor_ea>\t<anchor_name>\t<caller_func_ea>\t<caller_name>

import os
import idautils
import idc
import idaapi
import ida_funcs
import ida_name

OUT_DIR = os.path.join(os.path.dirname(idc.get_idb_path()), "ida_dump")
os.makedirs(OUT_DIR, exist_ok=True)

# Resolve Vd* import thunks by name, plus a few D3D-runtime functions found via
# strings. Names() lets us turn the FLIRT-named kernel thunks into addresses.
ANCHOR_NAMES = [
    "VdInitializeRingBuffer", "VdGetSystemCommandBuffer", "VdSwap",
    "VdInitializeEngines", "VdSetGraphicsInterruptCallback",
    "VdEnableRingBufferRPtrWriteBack", "VdInitializeScalerCommandBuffer",
    "VdSetSystemCommandBufferGpuIdentifierAddress",
]
ANCHOR_EAS = {}
for ea, name in idautils.Names():
    base = name.lstrip("_")
    for a in ANCHOR_NAMES:
        if base == a or base == a + "_entry":
            ANCHOR_EAS[ea] = name
# string-identified D3D runtime internals worth knowing callers of
for ea in (0x8220E038,):  # D3D GPU-hang / device debug
    ANCHOR_EAS[ea] = idc.get_func_name(ea) or ("sub_%08X" % ea)


def caller_func(frm):
    fn = ida_funcs.get_func(frm)
    if not fn:
        return None
    return fn.start_ea


n = 0
with open(os.path.join(OUT_DIR, "xrefs.txt"), "w", encoding="utf-8", errors="replace") as f:
    for aea, aname in sorted(ANCHOR_EAS.items()):
        seen = set()
        for xref in idautils.XrefsTo(aea, 0):
            cf = caller_func(xref.frm)
            if cf is None or cf in seen:
                continue
            seen.add(cf)
            cname = idc.get_func_name(cf) or ""
            f.write("0x%08X\t%s\t0x%08X\t%s\n" % (aea, aname, cf, cname))
            n += 1

print("[ida_xrefs] wrote %d caller edges for %d anchors" % (n, len(ANCHOR_EAS)))

if os.environ.get("IDA_DUMP_BATCH"):
    import ida_pro
    ida_pro.qexit(0)
