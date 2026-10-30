# @category PES6
# Dump the global data each function reads or writes, for finding the game's
# main structures (docs/20 §6).
# Usage: -postScript ExportDataRefs.py <out.tsv>
# Line: <function entry>\t<global>:<r|w|rw>,...   (addresses in user RAM, outside code)

out_path = getScriptArgs()[0]
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
refman = currentProgram.getReferenceManager()
LO, HI = 0x08800000, 0x0A000000

with open(out_path, 'w') as fh:
    for f in fm.getFunctions(True):
        acc = {}
        for i in listing.getInstructions(f.getBody(), True):
            for r in refman.getReferencesFrom(i.getAddress()):
                t = r.getReferenceType()
                if not t.isData():
                    continue
                a = r.getToAddress().getOffset()
                if not (LO <= a < HI) or fm.getFunctionContaining(r.getToAddress()) is not None:
                    continue
                k = acc.get(a, '')
                if t.isRead() and 'r' not in k:
                    k += 'r'
                if t.isWrite() and 'w' not in k:
                    k += 'w'
                acc[a] = k or 'a'          # 'a' = address taken
        if acc:
            fh.write('%08X\t%s\n' % (f.getEntryPoint().getOffset(),
                                     ','.join('%08X:%s' % (a, acc[a]) for a in sorted(acc))))
print('ExportDataRefs: done -> %s' % out_path)
