# @category PES6
# Disassemble and create a function at every address of a seed file (one hex
# address per line), e.g. build/gen_ovl/ovl_11.seeds for a raw-imported overlay.
# Usage: -postScript SeedFunctions.py <seeds.txt>
from ghidra.app.cmd.disassemble import DisassembleCommand

made = 0
for line in open(getScriptArgs()[0]):
    line = line.strip()
    if not line:
        continue
    a = toAddr(int(line, 16))
    if not currentProgram.getMemory().contains(a):
        continue
    if getInstructionAt(a) is None:
        DisassembleCommand(a, None, True).applyTo(currentProgram, monitor)
    if getFunctionAt(a) is None and createFunction(a, None) is not None:
        made += 1
print('SeedFunctions: %d functions created (%d total)' % (
    made, currentProgram.getFunctionManager().getFunctionCount()))
