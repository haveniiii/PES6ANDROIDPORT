# @category PES6
# Print the memory blocks of the current program (name, start, end, initialised).
for b in currentProgram.getMemory().getBlocks():
    print('MemBlocks: %-12s %s %s %s' % (b.getName(), b.getStart(), b.getEnd(), b.isInitialized()))
