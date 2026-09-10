"""
ida_export.py - Export accurate code structure + Win16 imports from IDA.

Run headless with the idalib Python (3.11):
    py -3.11 tools/ida_export.py work/DANCE02.DLL work/ida_funcs.json

Produces TWO files:
  1. <out.json> (arg2): per NE CODE segment (NE number = IDA seg index + 1):
       - functions:   sorted function entry offsets (segment-relative)
       - heads:       sorted instruction-head offsets (segment-relative)
       - code_ranges: merged [start,end) ranges IDA classified as code
     ne_decode syncs to these heads/ranges so the linear sweep cannot desync on
     data-in-code, and treats function entries as authoritative.

  2. work/win16_imports.json: {MODULE: {ordinal: api_name}} -- IDA's
     authoritative Win16 ordinal->API resolution, loaded by tools/win16.py so
     KERNEL/USER/GDI imports get real names instead of MODULE_OrdN stubs.
"""
import sys
import os
import json
import idapro  # MUST precede any ida_* import
import ida_auto, ida_segment, ida_funcs, ida_bytes, ida_nalt, idautils

binary = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else "ida_funcs.json"

ANALYSIS = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'work'))
os.makedirs(ANALYSIS, exist_ok=True)
imports_path = os.path.join(ANALYSIS, 'win16_imports.json')

if idapro.open_database(binary, run_auto_analysis=True):
    raise SystemExit("open failed")
ida_auto.auto_wait()

result = {}
imports = {}
try:
    # --- Code structure per NE CODE segment ---
    n = ida_segment.get_segm_qty()
    for i in range(n):
        s = ida_segment.getnseg(i)
        cls = ida_segment.get_segm_class(s) or ""
        if cls != "CODE":
            continue
        ne = i + 1
        base = s.start_ea
        funcs = sorted(int(ea - base) for ea in idautils.Functions(s.start_ea, s.end_ea))
        heads = []
        ranges = []
        cur_start = None
        prev_end = None
        ea = s.start_ea
        while ea < s.end_ea:
            flags = ida_bytes.get_flags(ea)
            sz = ida_bytes.get_item_size(ea)
            if sz <= 0:
                sz = 1
            if ida_bytes.is_code(flags):
                heads.append(int(ea - base))
                if cur_start is None:
                    cur_start = ea
                prev_end = ea + sz
            else:
                if cur_start is not None:
                    ranges.append([int(cur_start - base), int(prev_end - base)])
                    cur_start = None
            ea += sz
        if cur_start is not None:
            ranges.append([int(cur_start - base), int(prev_end - base)])
        result[str(ne)] = {"functions": funcs, "heads": heads, "code_ranges": ranges}

    # --- Win16 imports: {MODULE: {ordinal: api_name}} ---
    nmods = ida_nalt.get_import_module_qty()
    for mi in range(nmods):
        mod = (ida_nalt.get_import_module_name(mi) or f"MOD{mi}").upper()
        entries = imports.setdefault(mod, {})

        def cb(ea, name, ordinal, _entries=entries):
            if ordinal and name:
                _entries[str(ordinal)] = name
            return True

        ida_nalt.enum_import_names(mi, cb)
finally:
    idapro.close_database(save=False)

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(result, f)
# Merge imports into the shared map (multiple modules share the shim layer).
merged = {}
if os.path.exists(imports_path):
    try:
        with open(imports_path, encoding="utf-8") as f:
            merged = json.load(f)
    except Exception:
        merged = {}
for mod, ords in imports.items():
    merged.setdefault(mod, {}).update(ords)
with open(imports_path, "w", encoding="utf-8") as f:
    json.dump(merged, f, indent=1, sort_keys=True)

tot_f = sum(len(v["functions"]) for v in result.values())
tot_h = sum(len(v["heads"]) for v in result.values())
tot_i = sum(len(v) for v in imports.values())
print(f"Wrote {out_path}: {len(result)} code segments, {tot_f} functions, {tot_h} code heads")
print(f"Wrote {imports_path}: {len(imports)} modules, {tot_i} named ordinals")
for mod in sorted(imports):
    print(f"  {mod:<12s} {len(imports[mod])} names")
