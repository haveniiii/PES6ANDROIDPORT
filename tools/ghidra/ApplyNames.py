# @category PES6
# Name this program's functions in the Ghidra project from one of our name files
# (tools/names/pes6_eboot.txt or tools/names/ovl/<ovl>.txt; docs/20 §5), so that
# decompiling shows them. Creates the function when the address has none.
# Usage: -postScript ApplyNames.py <names.txt>
from ghidra.program.model.symbol import SourceType

path = getScriptArgs()[0]
fm = currentProgram.getFunctionManager()
done = created = failed = 0
for line in open(path):
    line = line.split('#', 1)[0].split()
    if len(line) < 2:
        continue
    try:
        addr = toAddr(int(line[0], 16))
    except ValueError:
        continue
    f = fm.getFunctionAt(addr)
    if f is None:
        disassemble(addr)
        f = createFunction(addr, None)
        created += f is not None
    if f is None:
        failed += 1
        continue
    if f.getName() != line[1]:
        try:
            f.setName(line[1], SourceType.USER_DEFINED)
            done += 1
        except Exception as e:
            print('ApplyNames: %s %s: %s' % (line[0], line[1], e))
            failed += 1
print('ApplyNames: %d renamed, %d functions created, %d failed (%s)' % (done, created, failed, path))
