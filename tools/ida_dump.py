# ida_dump.py — export mapping data from the Monster Madness IDB so the renderer
# work can proceed without the analysis MCP.
#
# Run in IDA (the DB is already open): File > Script file... > select this file,
# OR in the Output window:  exec(open(r"G:\rex\MonsterMadness\tools\ida_dump.py").read())
#
# Writes three files next to the IDB under  assets\ida_dump\ :
#   functions.txt  - <hex_ea>\t<name>\t<size>      (every function)
#   strings.txt    - <hex_ea>\t<referencing_func_ea or '-'>\t<text>
#   imports.txt    - <hex_ea>\t<module>\t<name>
#
# strings.txt is the key artifact: UE3 leaves rich markers (material/shader
# names, RHI debug text, "PixelShader"/"VertexShader", asset paths). The xref
# column maps those strings back to the functions that use them, which lets us
# name the RHI / D3D layer even where symbols are missing.

import os
import idautils
import idaapi
import idc
import ida_bytes
import ida_funcs
import ida_strlist
import ida_nalt

OUT_DIR = os.path.join(os.path.dirname(idc.get_idb_path()), "ida_dump")
os.makedirs(OUT_DIR, exist_ok=True)


def w(name):
    return open(os.path.join(OUT_DIR, name), "w", encoding="utf-8", errors="replace")


# --- functions ---
nfunc = 0
with w("functions.txt") as f:
    for ea in idautils.Functions():
        name = idc.get_func_name(ea) or ""
        size = idc.get_func_attr(ea, idc.FUNCATTR_END) - ea
        f.write("0x%08X\t%s\t%d\n" % (ea, name, size))
        nfunc += 1

# --- strings (with first code xref's containing function) ---
nstr = 0
sc = ida_strlist.string_info_t()
ida_strlist.build_strlist()
with w("strings.txt") as f:
    qty = ida_strlist.get_strlist_qty()
    for i in range(qty):
        if not ida_strlist.get_strlist_item(sc, i):
            continue
        ea = sc.ea
        s = idc.get_strlit_contents(ea, sc.length, sc.type)
        if s is None:
            continue
        try:
            s = s.decode("utf-8", "replace")
        except Exception:
            s = str(s)
        s = s.replace("\t", " ").replace("\r", " ").replace("\n", " ")
        func_ea = "-"
        for xref in idautils.XrefsTo(ea, 0):
            fn = ida_funcs.get_func(xref.frm)
            if fn:
                func_ea = "0x%08X" % fn.start_ea
                break
        f.write("0x%08X\t%s\t%s\n" % (ea, func_ea, s))
        nstr += 1

# --- imports ---
nimp = 0
with w("imports.txt") as f:
    def cb(ea, name, ordinal):
        global nimp
        f.write("0x%08X\t%s\t%s\n" % (ea, mod[0], name or ("#%d" % ordinal)))
        nimp += 1
        return True
    for k in range(idaapi.get_import_module_qty()):
        mod = [idaapi.get_import_module_name(k) or "?"]
        idaapi.enum_import_names(k, cb)

print("[ida_dump] wrote %d functions, %d strings, %d imports to %s"
      % (nfunc, nstr, nimp, OUT_DIR))

# Auto-close only when invoked in batch (IDA_DUMP_BATCH set). When a user runs
# this interactively via File > Script file, leave their IDA session untouched.
if os.environ.get("IDA_DUMP_BATCH"):
    import ida_pro
    ida_pro.qexit(0)
