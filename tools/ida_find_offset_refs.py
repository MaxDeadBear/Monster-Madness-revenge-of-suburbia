# ida_find_offset_refs.py — find functions that read/write specific device-struct
# offsets. Used to locate the D3D End* submitters: BeginVertices/BeginIndexedVertices
# store pending-draw state at device offsets 3357..3361 (*4 = 0x3474,0x3478,0x347C,
# 0x3480,0x3484); the End* functions READ those to emit the deferred draw.
#
# Batch: set IDA_DUMP_BATCH=1 & idat.exe -A -Stools\ida_find_offset_refs.py <db.i64>
# Output: assets\ida_dump\offset_refs.txt

import os
import idautils
import idc
import ida_funcs

OUT_DIR = os.path.join(os.path.dirname(idc.get_idb_path()), "ida_dump")
os.makedirs(OUT_DIR, exist_ok=True)

# device-struct binding slots. 0x308C (idx 3107) = bound index-buffer descriptor
# (DrawIndexedVertices reads it) => its writer is SetIndices. Neighbors of that
# writer should be the rest of the Set* cluster.
TARGET_DISP = {0x39D8: 14808}  # render-target slot (GetRenderTarget 0x82205738 reads it) -> writer = SetRenderTarget

# accumulate per function: which offsets it loads vs stores
funcs = {}  # ea -> {"r": set(), "w": set()}

for fea in idautils.Functions():
    fn = ida_funcs.get_func(fea)
    if not fn:
        continue
    for head in idautils.Heads(fn.start_ea, fn.end_ea):
        mnem = idc.print_insn_mnem(head)
        if not mnem:
            continue
        # PPC memory ops: lwz/lbz/lhz (load), stw/stb/sth (store). operand 1 is [disp, reg].
        is_load = mnem in ("lwz", "lbz", "lhz", "lwzx", "lwzu")
        is_store = mnem in ("stw", "stb", "sth", "stwu")
        if not (is_load or is_store):
            continue
        for opn in (0, 1):
            if idc.get_operand_type(head, opn) == idc.o_displ:
                disp = idc.get_operand_value(head, opn) & 0xFFFFFFFF
                if disp in TARGET_DISP:
                    rec = funcs.setdefault(fn.start_ea, {"r": set(), "w": set()})
                    rec["w" if is_store else "r"].add(TARGET_DISP[disp])

rows = []
for ea, rec in funcs.items():
    rows.append((len(rec["r"]), ea, sorted(rec["r"]), sorted(rec["w"])))
rows.sort(reverse=True)

with open(os.path.join(OUT_DIR, "offset_refs.txt"), "w", encoding="utf-8") as f:
    f.write("# funcs touching pending-draw fields 3357..3361. fmt: <ea>\treads=[..]\twrites=[..]\tsize\n")
    for _, ea, rds, wrs in rows:
        fn = ida_funcs.get_func(ea)
        size = fn.end_ea - fn.start_ea
        f.write("0x%08X\treads=%s\twrites=%s\t%d\n" % (ea, rds, wrs, size))

print("[offset_refs] %d functions touch pending-draw fields -> offset_refs.txt" % len(funcs))

if os.environ.get("IDA_DUMP_BATCH"):
    import ida_pro
    ida_pro.qexit(0)
