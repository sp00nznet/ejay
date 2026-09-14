"""
conform.py - check what this project believes about eJay's file formats against
the files themselves.

Every claim the hosts rely on is checkable on a disc: a sample's length is a
whole number of beats at the tempo we think it is, a control table parses to the
controls the host looks up by name, an index lines up with its path list. This
runs those checks over a local install and reports a pass/fail count.

The discs are not redistributable, so there is no corpus in the repository.
Point this at your own install; with nothing to check it says so and exits 0,
which is what CI gets.

    python tools/conform.py                      # default: ./original
    python tools/conform.py --root D: --quiet
"""
import argparse
import json
import os
import re
import struct
import sys

BPM = 140.0
RATE = 44100
# A beat at 140 BPM as the byte count of 16-bit mono PCM that a tPxD header
# carries: 44100 * 2 * 60/140.
BEAT_BYTES = RATE * 2 * 60.0 / BPM


class Report(object):
    def __init__(self, quiet):
        self.passed = 0
        self.failed = 0
        self.quiet = quiet
        self.failures = []

    def check(self, ok, what, detail=''):
        if ok:
            self.passed += 1
        else:
            self.failed += 1
            self.failures.append('%s  %s' % (what, detail))
        return ok

    def group(self, name, passed, failed):
        if not self.quiet:
            mark = 'ok  ' if not failed else 'FAIL'
            print('  [%s] %-32s %6d passed, %d failed' % (mark, name, passed, failed))


def pxd_length(path):
    """Decoded length from a tPxD header: tag, Pascal name, 0x54, then a u32."""
    with open(path, 'rb') as f:
        h = f.read(320)
    if len(h) < 16 or h[:4] != b'tPxD':
        return None
    # The name is a Pascal string and runs to 72 characters on some samples, so
    # a 64-byte peek is not enough header to find the length behind it.
    k = 5 + h[4]
    if k + 5 > len(h) or h[k] != 0x54:
        return None
    return struct.unpack('<I', h[k + 1:k + 5])[0]


def check_samples(root, rep):
    """Every sample is a whole number of beats at 140 BPM.

    This is the check that found the tempo. At any other tempo the lengths come
    out fractional - all of them, not a few.
    """
    p0, f0 = rep.passed, rep.failed
    n = 0
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.lower().endswith('.pxd'):
                continue
            path = os.path.join(dirpath, name)
            n += 1
            length = pxd_length(path)
            if not rep.check(length is not None, 'tPxD header', path):
                continue
            beats = length / BEAT_BYTES
            rep.check(abs(beats - round(beats)) < 0.01 and round(beats) >= 1,
                      'whole beats at %g BPM' % BPM,
                      '%s is %.3f beats (%d bytes)' % (path, beats, length))
    if n:
        rep.group('samples, whole beats @ %g' % BPM, rep.passed - p0, rep.failed - f0)
    return n


def parse_ktable(path):
    """K_640 and friends: a name line, then ten numbers."""
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r', '\n')
    lines = [x.strip() for x in text.split('\n')]
    out, i = {}, 0
    while i < len(lines):
        line = lines[i]
        if line and line[0].isalpha():
            nums, j = [], i + 1
            while j < len(lines) and len(nums) < 10:
                t = lines[j].strip()
                if not t:
                    j += 1
                    continue
                if not t.lstrip('-')[:1].isdigit():
                    break
                nums.append(int(t))
                j += 1
            if len(nums) == 10:
                out[line.replace(' ', '')] = nums
                i = j
                continue
        i += 1
    return out


def check_layout(root, rep):
    """The control tables parse, and still carry the controls the host looks up.

    The host takes its grid and browser rectangles out of these by name. A table
    that parses but has lost a name is exactly the failure that puts a panel back
    where it was eye-measured from, which is a bug this project has had twice.
    """
    wanted = ('G_SPUREN_SAMPLE_640', 'G_SAMPLE_WINDOW', 'K_SPUR_HSCROLL',
              'K_TAKTE', 'B_LAUT_01L', 'B_LAUT_16R', 'B_EJAY')
    p0, f0 = rep.passed, rep.failed
    n = 0
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.startswith('K_') or not name[2:].isdigit():
                continue
            path = os.path.join(dirpath, name)
            ctrls = parse_ktable(path)
            n += 1
            if not rep.check(len(ctrls) > 500, 'control table parses',
                             '%s gave %d controls' % (path, len(ctrls))):
                continue
            for w in wanted:
                if w.endswith('_640') and name != 'K_640':
                    continue
                rep.check(w in ctrls, 'control present', '%s in %s' % (w, path))
            # Every rectangle is in the 1280x960 space the art sets halve from.
            for cn, v in ctrls.items():
                # Negative is legal: K_MATRIX_SCHLITTEN_* dock above their
                # panel and sit at y -6. The check is that a coordinate is in
                # the 1280x960 space at all, not that it is inside the screen.
                rep.check(-64 <= v[0] <= 1280 and -64 <= v[1] <= 960,
                          'control in the 1280x960 space',
                          '%s %s at %d,%d' % (path, cn, v[0], v[1]))
    if n:
        rep.group('control tables', rep.passed - p0, rep.failed - f0)
    return n


def check_index(root, rep):
    """PXD.TXT's records pair one-for-one with MAX.TXT's paths, and the paths are
    on the disc. The sample browser is built straight off that pairing."""
    p0, f0 = rep.passed, rep.failed
    n = 0
    for dirpath, _, names in os.walk(root):
        upper = [x.upper() for x in names]
        if 'PXD.TXT' not in upper or 'MAX.TXT' not in upper:
            continue
        n += 1
        base = os.path.dirname(dirpath)
        with open(os.path.join(dirpath, names[upper.index('PXD.TXT')]), 'rb') as f:
            idx = f.read().decode('latin-1')
        with open(os.path.join(dirpath, names[upper.index('MAX.TXT')]), 'rb') as f:
            mx = f.read().decode('latin-1')
        # Keep empty fields. A sample whose second name line is blank still
        # occupies its four fields, and dropping it slides every record after it.
        fields = re.findall(r'"([^"]*)"', idx)
        paths = [p for p in re.findall(r'"([^"]*)"', mx) if p]
        # nine (start, count) pairs, then four fields per sample
        records = (len(fields) - 18) // 4
        rep.check(records == len(paths), 'index and path list agree',
                  '%s: %d records against %d paths' % (dirpath, records, len(paths)))
        missing = [p for p in paths[:40]
                   if not os.path.exists(os.path.join(base, p.replace('\\', os.sep)))]
        rep.check(not missing, 'sample paths resolve',
                  '%s: %d of the first 40 missing' % (dirpath, len(missing)))
    if n:
        rep.group('library indexes', rep.passed - p0, rep.failed - f0)
    return n


def check_mix(root, rep):
    """A saved arrangement names the sample libraries it draws on, as id ranges.
    They have to be ordered and contiguous or a block's id is ambiguous."""
    p0, f0 = rep.passed, rep.failed
    n = 0
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.lower().endswith('.mix'):
                continue
            n += 1
            path = os.path.join(dirpath, name)
            with open(path, 'rb') as f:
                d = f.read()
            tag = d.find(b'Dance eJay')
            if not rep.check(tag > 0, 'MIX names its program', path):
                continue
            i = d.index(b'\x00', tag) + 1
            if not rep.check(d[i:i + 1] == b'\x01', 'MIX library table follows', path):
                continue
            i += 3
            libs = []
            while i + 10 < len(d):
                a, b = struct.unpack('<II', d[i:i + 8])
                ln = struct.unpack('<H', d[i + 8:i + 10])[0]
                if not (0 < a < b < 100000) or not (2 < ln < 200):
                    break
                libs.append((a, b, d[i + 10:i + 10 + ln - 2].decode('latin-1')))
                i += 10 + ln - 1
                if i < len(d) and d[i] == 1:
                    i += 1
            rep.check(len(libs) >= 1, 'MIX lists libraries', path)
            for j in range(1, len(libs)):
                rep.check(libs[j][0] == libs[j - 1][1] + 1,
                          'MIX library ids contiguous',
                          '%s: %d..%d then %d' % (path, libs[j - 1][0],
                                                  libs[j - 1][1], libs[j][0]))
    if n:
        rep.group('saved arrangements', rep.passed - p0, rep.failed - f0)
    return n


def main(argv):
    ap = argparse.ArgumentParser(description='eJay format conformance harness')
    ap.add_argument('--root', default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'original'),
        help='folder holding your own eJay installs (default: ./original)')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--update-baseline', action='store_true',
                    help='record the current counts as the regression baseline')
    args = ap.parse_args(argv)

    if not os.path.isdir(args.root):
        print('conform: no disc at %s - nothing to check against.' % args.root)
        print('conform: point --root at your own install to run the corpus.')
        return 0

    rep = Report(args.quiet)
    print('conform: checking %s' % args.root)
    found = (check_samples(args.root, rep) + check_layout(args.root, rep) +
             check_index(args.root, rep) + check_mix(args.root, rep))
    if not found:
        print('conform: %s holds no eJay files - nothing to check.' % args.root)
        return 0

    # Fail on regression, not on the known exceptions. Five library samples are
    # genuinely not whole beats, the two intros are songs rather than loops, and
    # METRO.PXD is a plain RIFF WAV - all real, none of them a reason for a red
    # build every run. A number that moves is.
    base_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'conform_baseline.json')
    base = None
    if args.update_baseline:
        with open(base_path, 'w') as f:
            json.dump({'passed': rep.passed, 'failed': rep.failed}, f, indent=2)
        print('conform: baseline written to %s' % base_path)
    elif os.path.exists(base_path):
        with open(base_path) as f:
            base = json.load(f)

    for f in rep.failures[:20]:
        print('  fail: %s' % f)
    if len(rep.failures) > 20:
        print('  ... and %d more' % (len(rep.failures) - 20))
    print('conform: %d passed, %d failed' % (rep.passed, rep.failed))
    if base:
        print('conform: baseline %d passed, %d failed' % (base['passed'], base['failed']))
        if rep.failed > base['failed'] or rep.passed < base['passed']:
            print('conform: REGRESSION against the baseline')
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
