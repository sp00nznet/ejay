#!/usr/bin/env python3
"""Lift an eJay 2-generation title's two DLLs to C, for the Linux and macOS host.

On Windows the host (host/ejay2.c) loads the title's own PXD32?4.DLL (audio)
and PXD32CL1.DLL (graphics) and runs them as they are. Off Windows nothing can
run their machine code, so both are lifted here and linked into the host on
pcrecomp's runtime/win32hle (host/posix.c maps the images, binds their imports
to win32hle, and dispatches into the lifted C).

Both DLLs ask for 0x10000000, so the engine keeps it and the graphics DLL is
rebased to 0x10100000 (only its HIGHLOW relocations change). Each is cataloged
with disasm32 (exports, plus the vtables cpp/vtable_scan finds: PXD32CL1 is
MFC, and a method reached only through a vtable is called by no CALL), then
every cataloged function is lifted with lift32, the way civ3's run_lift.py
lifts its DLLs.

    python tools/lift_ejay2.py <program folder> <title id>
    python tools/lift_ejay2.py original/ejay2/D_ejay2/ejay dance2

Writes work/<title>/{engine,gfx}.dll (the images the host maps) and
src/recomp/<title>/gen_{engine,gfx}/ (the generated C). Neither is committed:
both are derived from the title's own binaries. A rerun skips a module whose
output is newer than its DLL; --force redoes it.
"""
import argparse
import glob
import json
import os
import struct
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(_HERE)
# PCRECOMP picks the toolkit checkout; the lifter and the runtime header the
# host compiles against must come from the same tree.
PCRECOMP = os.environ.get('PCRECOMP') or next(
    (os.path.join(ROOT, '..', d) for d in ('pcrecomp', 'tools')
     if os.path.isdir(os.path.join(ROOT, '..', d, 'runtime', 'win32hle'))), '')
_TOOLS = os.path.join(PCRECOMP, 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

# Where each module is mapped, lifted and loaded. host/posix.c has the same two.
BASES = {'engine': 0x10000000, 'gfx': 0x10100000}


def find_dlls(folder):
    """The title's engine and graphics DLL, by name pattern, any case."""
    files = {os.path.basename(p).lower(): p for p in glob.glob(os.path.join(folder, '*'))}
    gfx = files.get('pxd32cl1.dll')
    # PXD32D4 (Dance), PXD32H4 (HipHop), PXD32R4 (Techno), pxd32d5 (Dance 3): named for
    # the title. Not pxd32ha, pxd32t2 or PXD32RA, which HipHop eJay 2 also ships.
    engines = sorted(p for n, p in files.items()
                     if len(n) == 11 and n.startswith('pxd32') and n[6] in '45'
                     and n.endswith('.dll'))
    return (engines[0] if engines else None), gfx


def rebase(src, dst, base):
    """Copy a DLL, moved to `base`: its HIGHLOW relocations and ImageBase.
    (pefile's relocate_image also rewrites the import lookup table, which the
    loader then cannot read: civ3's run_lift.py found that.)"""
    import pefile
    pe = pefile.PE(src)
    data = bytearray(open(src, 'rb').read())
    delta = base - pe.OPTIONAL_HEADER.ImageBase
    for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []) if delta else []:
        for e in blk.entries:
            if e.type == 3:
                o = pe.get_offset_from_rva(e.rva)
                v = (struct.unpack_from('<I', data, o)[0] + delta) & 0xFFFFFFFF
                struct.pack_into('<I', data, o, v)
    struct.pack_into('<I', data, pe.OPTIONAL_HEADER.get_file_offset() + 28, base)
    open(dst, 'wb').write(data)


def catalog(dll, work, name):
    """disasm32's function catalog, seeded with the vtables vtable_scan finds."""
    seeds = os.path.join(work, name + '_vtable_seeds.json')
    out = os.path.join(work, name + '_functions.json')
    py = sys.executable
    subprocess.run([py, os.path.join(_TOOLS, 'cpp', 'vtable_scan.py'), dll, '--seeds', seeds],
                   check=True, stdout=subprocess.DEVNULL)
    with open(os.path.join(work, name + '_disasm32.log'), 'w') as log:
        subprocess.run([py, os.path.join(_TOOLS, 'disasm', 'disasm32.py'), dll, '-o', out,
                        '--seed-functions', seeds], check=True, stdout=log)
    return out, seeds


def lift(dll, cat_path, seeds, out_dir, prefix):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from generate import (EXTENT_REACH, find_splits, true_extent,
                          linear_disassemble_function, lift_function_linear, write_chunk)
    from lift32 import Lifter
    from pe_analyze import analyze_pe, build_iat_map

    info = analyze_pe(dll)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    cat = json.load(open(cat_path))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    # Vtable slots the catalog missed, each bounded by the next known entry: a
    # slot handed `ce` would make the extent walk descend the whole of .text.
    want = set()
    if os.path.exists(seeds):
        want = {e['address'] for e in json.load(open(seeds))
                if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'entry_kind': 'start'}

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(dll, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    # precise carry: adc/sbb take their carry from the flag state that set it
    # (civ3 and The Movies needed it; it is only off by default for Fury3).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(out_dir, exist_ok=True)
    for fn in os.listdir(out_dir):
        if fn.startswith('recomp_'):
            os.remove(os.path.join(out_dir, fn))

    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)
    entries, chunk, idx, errors = [], [], 0, 0
    # A branch leaving a body for an address nothing cataloged is a tail call
    # or shared code the catalog missed: it has to be dispatchable, so each
    # round's outside targets are lifted in the next, until none are new.
    todo = sorted(byaddr)
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, _ = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                 reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            try:
                lo = min(reached) if reached else addr
                insns, leaders = ([], None)
                if end > addr:
                    insns, leaders = linear_disassemble_function(md, code, cs, lo, end,
                                                                 reached=reached)
                if leaders is not None:
                    leaders.add(addr)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
                errors += 1
            chunk.append((body, addr, name))
            entries.append((addr, name))
            if len(chunk) >= 400:
                write_chunk(out_dir, idx, chunk)
                idx, chunk = idx + 1, []
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'entry_kind': 'start'}
    if chunk:
        write_chunk(out_dir, idx, chunk)

    with open(os.path.join(out_dir, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* AUTO-GENERATED by tools/lift_ejay2.py: not committed */\n#pragma once\n\n')
        for _, n in entries:
            f.write('void %s(void);\n' % n)
    # The table's rows on their own: host/dispatch.c puts the engine's, then
    # the graphics DLL's, which is address order (recomp_lookup bisects).
    with open(os.path.join(out_dir, 'recomp_dispatch.inc'), 'w', newline='\n') as f:
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
    return len(entries), errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', help="the title's program folder (the one with PXD32CL1.DLL)")
    ap.add_argument('title', help='an id for this title: dance2, hiphop2, ...')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    if not os.path.isdir(os.path.join(_TOOLS, 'lift')):
        sys.exit('no pcrecomp toolkit beside this repo: set PCRECOMP, or clone it to ../pcrecomp')
    engine, gfx = find_dlls(args.folder)
    if not engine or not gfx:
        sys.exit('%s has no PXD32?4.DLL and PXD32CL1.DLL: not an eJay 2-generation program folder'
                 % args.folder)
    work = os.path.join(ROOT, 'work', args.title)
    os.makedirs(work, exist_ok=True)
    for name, src in (('engine', engine), ('gfx', gfx)):
        dll = os.path.join(work, name + '.dll')
        out = os.path.join(ROOT, 'src', 'recomp', args.title, 'gen_' + name)
        done = os.path.join(out, 'recomp_dispatch.inc')
        fresh = os.path.exists(done) and os.path.getmtime(done) > os.path.getmtime(src)
        if not args.force and fresh:
            print('  %-6s %s: lifted already' % (name, os.path.basename(src)))
            continue
        t0 = time.time()
        rebase(src, dll, BASES[name])
        cat, seeds = catalog(dll, work, name)
        n, err = lift(dll, cat, seeds, out, name)
        print('  %-6s %s at 0x%08X: %d functions, %d errors, %.0fs'
              % (name, os.path.basename(src), BASES[name], n, err, time.time() - t0))
        if err:
            sys.exit('the lift had errors: see the ERROR comments in %s' % out)


if __name__ == '__main__':
    main()
