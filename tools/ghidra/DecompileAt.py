# @category PES6
# Decompile the function containing <address> and write C to <output_file>.
# Usage (headless): -postScript DecompileAt.py <address> [output_file]
# If no function exists at the address, one is created (disassembly first).

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
addr_str = args[0]
out_path = args[1] if len(args) > 1 else os.path.join(os.getcwd(), "decomp.c")

addr = toAddr(addr_str)
func = getFunctionContaining(addr)
if func is None:
    fm = currentProgram.getFunctionManager()
    disassemble(addr)
    func = createFunction(addr, None)
    if func is None:
        func = fm.getFunctionAt(addr)
    if func is not None:
        print("created function at %s" % addr_str)

if func is None:
    print("no function contains %s" % addr_str)
else:
    iface = DecompInterface()
    iface.openProgram(currentProgram)
    res = iface.decompileFunction(func, 60, ConsoleTaskMonitor())
    if not res.decompileCompleted():
        print("decompile failed: %s" % res.getErrorMessage())
    else:
        c = res.getDecompiledFunction().getC()
        with open(out_path, "w") as f:
            f.write(c)
        print("decompiled %s (%s) -> %s" % (func.getName(), func.getEntryPoint(), out_path))
