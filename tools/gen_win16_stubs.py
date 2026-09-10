"""
gen_win16_stubs.py - this project's arguments to the toolbox generator.

The generator itself lives in pcrecomp at tools/ne/gen_win16_stubs.py: it reads
DANCE02's own relocations and writes the import surface, so the shim layer
cannot drift from what the binary imports. Everything project-specific is the
five paths below.

    python tools/gen_win16_stubs.py
"""
import os
import runpy
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
GEN = os.path.abspath(os.path.join(
    ROOT, '..', 'tools', 'tools', 'ne', 'gen_win16_stubs.py'))

sys.argv = [
    GEN,
    os.path.join(ROOT, 'work', 'DANCE02.DLL'),
    '--api',   os.path.join(ROOT, 'runtime', 'runtime_api.h'),
    '--stubs', os.path.join(ROOT, 'runtime', 'win16', 'win16_stubs.c'),
    '--shims', os.path.join(ROOT, 'runtime', 'win16'),
    '--guard', 'EJAY',
]
runpy.run_path(GEN, run_name='__main__')
