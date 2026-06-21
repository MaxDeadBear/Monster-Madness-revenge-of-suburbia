import os, idautils, idc, ida_funcs, ida_hexrays
OUT=os.path.join(os.path.dirname(idc.get_idb_path()),"ida_dump","decompiled"); os.makedirs(OUT,exist_ok=True)
ida_hexrays.init_hexrays_plugin()
for ea in (0x8220CF00, 0x8220ADA8, 0x8221C2D8):
    try: t=str(ida_hexrays.decompile(ea))
    except Exception as e: t="// fail %s"%e
    open(os.path.join(OUT,"%08X.c"%ea),"w",encoding="utf-8",errors="replace").write(t)
print("done")
import os as _o
if _o.environ.get("IDA_DUMP_BATCH"):
    import ida_pro; ida_pro.qexit(0)
