# @category PES6
# Export all functions (address + name) to a text file for diffing/reporting.
# Usage (headless): -postScript ExportFunctions.py <output_file>

import os

args = getScriptArgs()
out_path = args[0] if len(args) > 0 else os.path.join(os.getcwd(), "ghidra_symbols.txt")

fm = currentProgram.getFunctionManager()
count = 0
with open(out_path, "w") as f:
    for func in fm.getFunctions(True):
        name = func.getName()
        addr = func.getEntryPoint().toString()
        f.write("%s\t%s\n" % (addr, name))
        count += 1
print("exported %d functions to %s" % (count, out_path))
