# @category PES6
# Decompile many functions in one headless run.
# Usage: -postScript DecompileMany.py <outdir> <addr1> <addr2> ...
# For each address: create function if needed, decompile, write <outdir>/<addr>.c

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
outdir = args[0]
addrs = args[1:]
if not os.path.isdir(outdir):
    os.makedirs(outdir)

iface = DecompInterface()
iface.openProgram(currentProgram)
mon = ConsoleTaskMonitor()

for addr_str in addrs:
    addr = toAddr(addr_str)
    func = getFunctionContaining(addr)
    if func is None:
        disassemble(addr)
        func = createFunction(addr, None) or getFunctionAt(addr)
    if func is None:
        print("no function at %s" % addr_str)
        continue
    res = iface.decompileFunction(func, 120, mon)
    if not res.decompileCompleted():
        print("decompile failed %s: %s" % (addr_str, res.getErrorMessage()))
        continue
    c = res.getDecompiledFunction().getC()
    out = os.path.join(outdir, addr_str.lower() + ".c")
    with open(out, "w") as f:
        f.write(c)
    print("decompiled %s %s -> %s" % (addr_str, func.getName(), out))
