"""
test_engine.py - the smallest thing that fails if the lift breaks.

Two checks, both cheap:

1. Every import the binary references has a stack-purge value. Win16 is PASCAL,
   so a missing purge does not fail at the call site - the callee leaves its
   arguments on the stack, the CALLER's frame shifts, and its epilogue restores
   DS from the wrong slot. The damage lands somewhere else entirely, which is
   why this is checked statically rather than waited for.

2. The built harness runs LibMain and gets a non-zero AX back, and the
   zero-argument exports return without taking the process down.

    python tools/test_engine.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
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


def test_zero_arg_exports_return():
    # These four take no arguments, so they can be called with nothing pushed
    # and still be asked a fair question. AGetFull reports whether the engine
    # has samples loaded; ALautGet reads the master volume back.
    for name in ('AGetTime', 'AGetFree', 'AGetFull', 'ALautGet'):
        r = run(name)
        assert r.returncode == 0, f'{name} exited {r.returncode}\n{r.stdout}{r.stderr}'
        assert f'{name.upper()} returned ax=' in r.stdout, r.stdout + r.stderr
        print(f'  {name}: ' + r.stdout.split(f'{name.upper()} returned ')[1].strip())


if __name__ == '__main__':
    test_every_import_has_a_purge()
    if not os.path.exists(EXE):
        print(f'  {EXE} not built - skipping the runtime checks')
        sys.exit(0)
    test_libmain_initialises()
    test_zero_arg_exports_return()
    print('ok')
