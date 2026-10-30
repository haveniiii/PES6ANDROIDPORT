"""Read PES6_FNPROF profiles (runtime/desktop/main.c) and map their addresses to functions.

Profile lines, oldest format first:
    addr <TAB> nframes <TAB> frames
    addr <TAB> nframes <TAB> scene mask <TAB> frames
    addr <TAB> nframes <TAB> scene mask <TAB> program <TAB> frames
"program" is EBOOT.elf or the overlay that held the address at that moment (several
overlays share load addresses). Without it, an address is looked up in the EBOOT and then
in DEFAULT_OVERLAYS.
"""
import bisect
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EBOOT = 'EBOOT.elf'
DEFAULT_OVERLAYS = ['game.ovl', 'result.ovl', 'netlib.ovl']
_spans = {}


def tag_of(prog):
    return 'eboot' if prog == EBOOT else prog[:-4]


def spans(prog):
    """Sorted [(start, end)] of the functions of a program (build/decomp/<tag>/index.tsv)."""
    if prog not in _spans:
        out = []
        path = os.path.join(ROOT, 'build/decomp', tag_of(prog), 'index.tsv')
        if os.path.exists(path):
            for line in open(path):
                a, size, _n, _ok = line.rstrip('\n').split('\t')
                out.append((int(a, 16), int(a, 16) + int(size)))
        out.sort()
        _spans[prog] = (out, [a for a, _ in out])
    return _spans[prog]


def containing(prog, addr):
    """Start of the function of prog holding addr, or None."""
    sp, starts = spans(prog)
    i = bisect.bisect_right(starts, addr) - 1
    return sp[i][0] if i >= 0 and addr < sp[i][1] + 4 else None


def locate(addr, prog=None):
    """(program, function start) for a profiled address."""
    for p in ([prog] if prog else [EBOOT] + DEFAULT_OVERLAYS):
        f = containing(p, addr)
        if f is not None:
            return p, f
    return None, None


def read(path):
    """Yield (program, function start, nframes, scene mask or None, [frame strings])."""
    for line in open(path):
        p = line.rstrip('\n').split('\t')
        if len(p) < 3:
            continue
        addr, n = int(p[0], 16), int(p[1])
        mask, prog, frames = None, None, p[-1]
        if len(p) >= 4:
            mask = int(p[2], 16)
        if len(p) >= 5:
            prog = p[3]
        prog, func = locate(addr, prog)
        if func is None:
            continue
        yield prog, func, n, mask, [x for x in frames.split(',') if x]


def has_program_column(path):
    """True for profiles that record the program of each address (5 columns)."""
    for line in open(path):
        return len(line.rstrip('\n').split('\t')) >= 5
    return False
