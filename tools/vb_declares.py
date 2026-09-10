"""
vb_declares.py - recover a VB5 program's DLL calls, and their arguments.

Dancejay.exe imports MSVBVM50 and KERNEL32 and nothing else, which makes it look
like it never touches PXD32D4 or PXD32CL1 at all. It does: VB's `Declare
Function` is resolved lazily, and the compiler emits one thunk per declared API
holding the DLL and export name in a descriptor beside it:

    A1 <cache>          mov  eax, [cache]       ; resolved address, or null
    0B C0 74 02 FF E0   or / jz / jmp eax       ; fast path
    68 <descriptor>     push offset descriptor  ; {dll*, proc*, flags, cache}
    B8 <resolver> FF D0 call resolver
    FF E0               jmp  eax

So the thunk address maps back to an export name, and every `call <thunk>` in
the program is a call to that export with its arguments pushed in front of it.

That is the difference between guessing at a 1998 DLL and reading its use. It is
where Dance eJay 2's real start-up order came from -

    ATyp(3) -> ADevice(0) -> AInit(hwnd)

- which is not the 1997 order, and why every earlier attempt got zero back from
an engine that was working exactly as designed.

    python tools/vb_declares.py                  # list the declared imports
    python tools/vb_declares.py GFX_Intro        # call sites for matching names
    python tools/vb_declares.py AInit --exe X    # against another program
"""
import os
import re
import struct
import sys

import capstone
import pefile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DEFAULT_EXE = os.path.join(ROOT, 'work', 'ejay2', 'Dancejay.exe')

THUNK = re.compile(
    b'\xa1(....)\x0b\xc0\x74\x02\xff\xe0\x68(....)\xb8(....)\xff\xd0\xff\xe0', re.S)


class Program(object):
    def __init__(self, path):
        self.pe = pefile.PE(path)
        base = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = [(base + s.VirtualAddress, s.get_data(), s.Characteristics)
                     for s in self.pe.sections]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.thunks = self._find_thunks()

    def read(self, va, n):
        for sva, data, _ in self.secs:
            if sva <= va < sva + len(data):
                return data[va - sva:va - sva + n]
        return b''

    def cstr(self, va):
        return self.read(va, 128).split(b'\x00')[0].decode('latin-1', 'replace')

    def _find_thunks(self):
        out = {}
        for sva, data, _ in self.secs:
            for m in THUNK.finditer(data):
                desc = struct.unpack('<I', m.group(2))[0]
                head = self.read(desc, 16)
                if len(head) < 16:
                    continue
                dllp, procp = struct.unpack('<II', head[:8])
                out[sva + m.start()] = (self.cstr(dllp), self.cstr(procp))
        return out

    def call_sites(self, match):
        """Every direct call to a thunk whose export name contains `match`."""
        for sva, data, ch in self.secs:
            if not (ch & 0x20000000):
                continue
            for m in re.finditer(b'\xe8', data):
                off = m.start()
                if off + 5 > len(data):
                    continue
                tgt = sva + off + 5 + struct.unpack('<i', data[off + 1:off + 5])[0]
                if tgt in self.thunks and match in self.thunks[tgt][1]:
                    yield sva, data, off, self.thunks[tgt][1]

    def context(self, sva, data, off, back=90):
        """Instructions leading up to the call at `off`.

        Disassembling backwards from an arbitrary byte desyncs, so try every
        start and keep the longest run that lands exactly on the call - the one
        that agrees with the real instruction boundaries.
        """
        best = None
        for start in range(max(0, off - back), off):
            run = list(self.md.disasm(data[start:off + 5], sva + start))
            if run and run[-1].address == sva + off and (best is None or len(run) > len(best)):
                best = run
        return best or []


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    exe = DEFAULT_EXE
    if '--exe' in argv:
        exe = argv[argv.index('--exe') + 1]
        args = [a for a in args if a != exe]

    prog = Program(exe)
    print('%s: %d VB Declare thunks' % (os.path.basename(exe), len(prog.thunks)))
    if not args:
        for addr, (dll, name) in sorted(prog.thunks.items(), key=lambda x: x[1]):
            print('  %08X  %-16s %s' % (addr, dll, name))
        return

    match = args[0]
    hits = 0
    for sva, data, off, name in prog.call_sites(match):
        hits += 1
        print('\n--- %s called @ %08X' % (name, sva + off))
        for ins in prog.context(sva, data, off):
            note = ''
            if ins.mnemonic == 'push' and ins.op_str.startswith('0x'):
                s = prog.cstr(int(ins.op_str, 16))
                if len(s) > 2 and s.isprintable():
                    note = '   ; "%s"' % s
            print('    %08X  %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
    print('\n%d call sites' % hits)


if __name__ == '__main__':
    main(sys.argv[1:])
