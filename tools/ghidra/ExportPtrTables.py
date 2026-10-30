# @category PES6
# Function pointers stored in data (any aligned word, outside instructions, equal to a
# function entry), and which code uses the table they sit in, so that
# ownership can follow indirect calls (tools/names/derive_parts.py, docs/20 §6).
# Usage: -postScript ExportPtrTables.py <out.tsv>
# Line: <target function>\t<slot address>\t<table base>\t<user function,user function...>
# The table base is the nearest address at or below the slot (within 0x1000 bytes) that an
# instruction refers to; its users are the functions holding those instructions.
out_path = getScriptArgs()[0]
fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()
refman = currentProgram.getReferenceManager()
mem = currentProgram.getMemory()


def code_users(addr):
    users = set()
    for r in refman.getReferencesTo(addr):
        if listing.getInstructionAt(r.getFromAddress()) is None:
            continue
        f = fm.getFunctionContaining(r.getFromAddress())
        if f is not None:
            users.add(f.getEntryPoint().getOffset())
    return users


def table_base(slot):
    a = slot
    for _ in range(0x400):                    # 0x1000 bytes, word by word
        users = code_users(a)
        if users:
            return a, users
        if a.getOffset() < 4:
            break
        a = a.subtract(4)
        if not mem.contains(a):
            break
    return None, set()


n = 0
entries = set(f.getEntryPoint().getOffset() for f in fm.getFunctions(True))
with open(out_path, 'w') as fh:
    for block in mem.getBlocks():
        if not block.isInitialized() or not block.getStart().isMemoryAddress() or block.getStart().getOffset() < 0x08800000:
            continue
        a = block.getStart()
        end = block.getEnd().getOffset()
        if a.getOffset() & 3:
            a = a.add(4 - (a.getOffset() & 3))
        while a.getOffset() + 3 <= end:
            if listing.getInstructionContaining(a) is None:
                try:
                    v = mem.getInt(a) & 0xFFFFFFFF
                except Exception:
                    v = 0
                if v in entries:
                    base, users = table_base(a)
                    if base is not None:
                        fh.write('%08X\t%08X\t%08X\t%s\n' % (v, a.getOffset(), base.getOffset(),
                                                             ','.join('%08X' % u for u in sorted(users))))
                        n += 1
            a = a.add(4)
print('ExportPtrTables: %d pointer slots -> %s' % (n, out_path))
