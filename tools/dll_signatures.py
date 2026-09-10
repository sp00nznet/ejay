"""
dll_signatures.py - recover the argument count of every export in eJay 2's DLLs.

A __stdcall callee pops its own arguments, so `ret N` gives N/4 arguments and
there is no need to guess. The catch is finding the right `ret`: a linear sweep
from the entry point desyncs on the first jump table or inline data and then
reports whatever byte happens to decode as a return, which is how an earlier
pass of this had GFX_IntroClose taking two arguments when it takes none.

So walk the control-flow graph instead - follow branches, stop at returns - and
report every distinct `ret N` reached. A __stdcall function has one purge value,
so more than one answer means the walk went somewhere it should not have, and
the export is reported as unknown rather than guessed at.

    python tools/dll_signatures.py [path-to-dll ...]
"""
import os
import sys

import capstone
import pefile

DEFAULT = [
    'original/ejay2/D_ejay2/ejay/PXD32D4.DLL',
    'original/ejay2/D_ejay2/ejay/PXD32CL1.DLL',
]

STOP = {'ret', 'retf', 'jmp', 'iret'}


def walk(md, read, entry, limit=6000):
    """Reachable-instruction walk from one entry point. Returns the set of
    `ret N` purge values found."""
    seen, todo, purges = set(), [entry], set()
    while todo:
        pc = todo.pop()
        while pc not in seen:
            seen.add(pc)
            if len(seen) > limit:
                return purges
            code = read(pc, 16)
            if not code:
                break
            ins = next(md.disasm(code, pc), None)
            if ins is None:
                break
            m = ins.mnemonic
            if m.startswith('ret'):
                purges.add(int(ins.op_str, 16) if ins.op_str else 0)
                break
            if m.startswith('j') and ins.op_str.startswith('0x'):
                todo.append(int(ins.op_str, 16))
                if m == 'jmp':
                    break
            elif m == 'jmp':
                break               # indirect: cannot follow, stop this path
            pc = ins.address + ins.size
    return purges


def signatures(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = [(base + s.VirtualAddress, s.get_data()) for s in pe.sections]

    def read(va, n):
        for sva, data in secs:
            if sva <= va < sva + len(data):
                return data[va - sva:va - sva + n]
        return b''

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = {}
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if not e.name:
            continue
        p = walk(md, read, base + e.address)
        out[e.name.decode()] = (p.pop() // 4) if len(p) == 1 else None
    return out


def main(argv):
    for path in (argv or DEFAULT):
        print('== %s' % os.path.basename(path))
        for name, args in sorted(signatures(path).items()):
            print('  %-24s %s' % (name, args if args is not None else '?'))


if __name__ == '__main__':
    main(sys.argv[1:])
