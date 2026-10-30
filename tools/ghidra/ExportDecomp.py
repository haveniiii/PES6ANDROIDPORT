# @category PES6
# Decompile every function of this program into one C file per function, plus an
# index, so the whole game can be read and searched offline (docs/20 §6).
# Usage: -postScript ExportDecomp.py <outdir>
# Out: <outdir>/<ADDR>_<name>.c and <outdir>/index.tsv (addr, bytes, name, ok)
# Derived from the game's code: it lives under build/ (git-ignored), never in git.
import os
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

outdir = getScriptArgs()[0]
if not os.path.isdir(outdir):
    os.makedirs(outdir)
for old in os.listdir(outdir):
    if old.endswith('.c'):
        os.remove(os.path.join(outdir, old))

iface = DecompInterface()
iface.setOptions(DecompileOptions())
iface.openProgram(currentProgram)
mon = ConsoleTaskMonitor()
ok = bad = 0
with open(os.path.join(outdir, 'index.tsv'), 'w') as idx:
    for f in currentProgram.getFunctionManager().getFunctions(True):
        a = '%08X' % f.getEntryPoint().getOffset()
        res = iface.decompileFunction(f, 60, mon)
        good = res.decompileCompleted()
        if good:
            with open(os.path.join(outdir, '%s_%s.c' % (a, f.getName())), 'w') as fh:
                fh.write(res.getDecompiledFunction().getC())
            ok += 1
        else:
            bad += 1
        idx.write('%s\t%d\t%s\t%d\n' % (a, f.getBody().getNumAddresses(), f.getName(), good))
print('ExportDecomp: %d decompiled, %d failed -> %s' % (ok, bad, outdir))
