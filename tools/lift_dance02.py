"""
lift_dance02.py - Lift DANCE02.DLL's code segment to src/segNNN.c.

DANCE02.DLL is the 1997 Dance eJay audio engine: one CODE segment, one DATA
segment, small model, 34 named entry points. Everything the game can hear goes
through it.

    python tools/lift_dance02.py

Uses work/ida_funcs.json when present (byte-accurate instruction heads, so the
sweep cannot desync on data-in-code) - regenerate it with:

    py -3.11 tools/ida_export.py work/DANCE02.DLL work/ida_funcs.json
"""
import os
import sys
import contextlib

sys.path.insert(0, os.path.dirname(__file__))
from ne_parse import parse_ne
import ne_lift          # puts pcrecomp's tools/lift on sys.path, so lift16 imports
import lift16

# pcrecomp's lift16 emits a call to this at every DIV/IDIV by zero. Name it
# ours rather than inheriting another project's prefix.
lift16.DIV0_FN = 'ejay_div0'

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DLL = os.path.join(ROOT, 'work', 'DANCE02.DLL')
SRC = os.path.join(ROOT, 'src')
IDA = os.path.join(ROOT, 'work', 'ida_funcs.json')

if os.path.exists(IDA):
    os.environ['EJAY_IDA_JSON'] = IDA
    print(f'  IDA map: {IDA}')
else:
    print('  no IDA map - linear sweep will desync on data-in-code')

os.makedirs(SRC, exist_ok=True)
dll = parse_ne(DLL)
code = [s for s in dll.segments if s.is_code]
print(f'DANCE02 code segments: {[s.index for s in code]}')
for s in code:
    out = os.path.join(SRC, f'seg{s.index:03d}.c')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        with contextlib.redirect_stdout(f):
            ne_lift.lift_segment(dll, s.index)
    print(f'  seg{s.index:03d}.c  {os.path.getsize(out):,} bytes')
