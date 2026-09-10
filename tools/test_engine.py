"""
test_engine.py - the smallest thing that fails if the lift breaks.

Two checks, both cheap:

1. Every import the binary references has a stack-purge value. Win16 is PASCAL,
   so a missing purge does not fail at the call site - the callee leaves its
   arguments on the stack, the CALLER's frame shifts, and its epilogue restores
   DS from the wrong slot. The damage lands somewhere else entirely, which is
   why this is checked statically rather than waited for.

2. The built harness runs LibMain and gets a non-zero AX back, and a handful
   of exports return with the stack exactly balanced - which is the one
   observable that catches a wrong purge anywhere on the path.

    python tools/test_engine.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
# win16 (the import resolver + PASCAL purge table) lives in the toolbox.
sys.path.insert(0, os.path.abspath(os.path.join(
    os.path.dirname(__file__), '..', '..', 'tools', 'tools', 'ne')))
from ne_parse import parse_ne
import win16

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DLL = os.path.join(ROOT, 'work', 'DANCE02.DLL')
EXE = os.path.join(ROOT, 'build', 'ejay.exe')
DATA = os.path.join(ROOT, 'original', 'ejay1', 'DANCE', 'DMACHINE')


def test_every_import_has_a_purge():
    ne = parse_ne(DLL)
    missing = []
    for seg in ne.segments:
        for r in seg.relocations:
            if (r.flags & 3) not in (1, 2):
                continue
            mod = win16.module_name(ne, r.module_idx)
            imp = win16.get_import(mod, r.ordinal)
            if win16.get_purge(imp.module, imp.api) is None:
                missing.append(f'{imp.module}.{imp.api} (@{imp.ordinal})')
    assert not missing, 'imports with no PASCAL purge: ' + ', '.join(sorted(set(missing)))
    print('  every import has a purge')


def run(*args):
    return subprocess.run([EXE, '--dir', DATA, *args],
                          capture_output=True, text=True, timeout=120)


def test_libmain_initialises():
    r = run()
    assert 'LibMain returned ax=' in r.stdout, r.stdout + r.stderr
    ax = r.stdout.split('LibMain returned ax=')[1][:4]
    assert int(ax, 16) != 0, f'LibMain refused to initialise (ax={ax})'
    print(f'  LibMain ax={ax}')


# Argument sizes come from each callee's own RETF, read out of the binary by
# IDA (work/argsize.json). Calling one with the wrong number of bytes is not a
# harmless mistake: the callee pops what it was compiled to pop, so the stack
# comes back shifted by the difference - which is how this list was wrong the
# first time it was written.
CALLS = [
    ('AGetTime', 'AGetTime'),        # 0 bytes
    ('AGetFree', 'AGetFree:d:0'),    # 4
    ('AGetFull', 'AGetFull:d:0'),    # 4
    ('ALautGet', 'ALautGet:d:0'),    # 4
    ('AInit',    'AInit:1'),         # 2
    ('ADevice',  'ADevice:d:0'),     # 4
]


def test_exports_return_and_balance_the_stack():
    for name, spec in CALLS:
        r = run(spec)
        assert r.returncode == 0, f'{name} exited {r.returncode}\n{r.stdout}{r.stderr}'
        assert f'{name.upper()} returned ax=' in r.stdout, r.stdout + r.stderr
        line = r.stdout.split(f'{name.upper()} returned ')[1].split('\n')[0].strip()
        # The whole PASCAL purge story in one assertion: a callee must leave SP
        # exactly 4 + argbytes higher than it found it.
        assert '***' not in line, f'{name} unbalanced the stack: {line}'
        print(f'  {name}: {line}')


if __name__ == '__main__':
    test_every_import_has_a_purge()
    if not os.path.exists(EXE):
        print(f'  {EXE} not built - skipping the runtime checks')
        sys.exit(0)
    test_libmain_initialises()
    test_exports_return_and_balance_the_stack()
    print('ok')
