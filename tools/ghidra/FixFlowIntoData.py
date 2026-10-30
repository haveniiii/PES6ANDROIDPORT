# @category PES6
# Stop functions decompiling as "halt_baddata" (docs/20 §6): tail jumps into another
# program become call + return, and code Ghidra took for data is re-disassembled. For every instruction of every function, a fall-through or
# branch target that holds defined data (or nothing) instead of an instruction is cleared
# and disassembled; the containing function's body is then recomputed.
# Usage: -postScript FixFlowIntoData.py
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.listing import FlowOverride
from ghidra.program.model.address import AddressSet

listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()
fixed = 0
touched = set()
# functions whose entry itself was never disassembled
for f in list(fm.getFunctions(True)):
    e = f.getEntryPoint()
    if listing.getInstructionAt(e) is None and mem.contains(e):
        d = listing.getDataContaining(e)
        if d is not None and d.isDefined():
            clearListing(d.getMinAddress(), d.getMaxAddress())
        if disassemble(e):
            fixed += 1
            touched.add(e)
# tail jumps (j) into another program (an overlay jumping into the EBOOT): Ghidra cannot
# follow them and truncates the function ("halt_baddata"); treat them as call + return
tails = 0
for ins in listing.getInstructions(True):
    ft = ins.getFlowType()
    if ft.isJump() and not ft.isConditional() and not ft.isComputed():
        flows = ins.getFlows()
        if flows and not mem.contains(flows[0]):
            ins.setFlowOverride(FlowOverride.CALL_RETURN)
            tails += 1
            f = fm.getFunctionContaining(ins.getAddress())
            if f is not None:
                touched.add(f.getEntryPoint())
for f in list(fm.getFunctions(True)):
    for ins in listing.getInstructions(f.getBody(), True):
        targets = list(ins.getFlows())
        ft = ins.getFallThrough()
        if ft is not None:
            targets.append(ft)
        for t in targets:
            if not mem.contains(t) or listing.getInstructionAt(t) is not None:
                continue
            if fm.getFunctionAt(t) is not None:
                continue
            d = listing.getDataContaining(t)
            if d is not None and d.isDefined():
                clearListing(d.getMinAddress(), d.getMaxAddress())
            if disassemble(t):
                fixed += 1
                touched.add(f.getEntryPoint())
for entry in touched:
    f = fm.getFunctionAt(entry)
    if f is not None:
        CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor)
print('FixFlowIntoData: %d external tail jumps, %d flow targets disassembled, %d functions touched' % (tails, fixed, len(touched)))
