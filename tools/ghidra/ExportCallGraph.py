# @category PES6
# Dump every function's size, direct callees and referenced strings, for offline
# analysis (tools/names/module_map.py, docs/20 §5).
# Usage: -postScript ExportCallGraph.py <out.tsv>
# Line: <entry>\t<bytes>\t<name>\t<callee,callee...>\t<string|string...>
# Callees outside this program (EBOOT <-> overlay) are kept as raw addresses.
import re

out_path = getScriptArgs()[0]
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
refman = currentProgram.getReferenceManager()

with open(out_path, 'w') as fh:
    for f in fm.getFunctions(True):
        body = f.getBody()
        callees, strs = set(), []
        for i in listing.getInstructions(body, True):
            ft = i.getFlowType()
            if ft.isCall() or (ft.isJump() and not ft.isConditional()):
                for t in i.getFlows():
                    if not body.contains(t):
                        callees.add(t.getOffset())
            for r in refman.getReferencesFrom(i.getAddress()):
                if r.getReferenceType().isData():
                    d = listing.getDataAt(r.getToAddress())
                    if d is not None and d.hasStringValue() and len(strs) < 8:
                        strs.append(re.sub(r'[\s|]+', ' ', str(d.getValue()))[:48])
        fh.write('%08X\t%d\t%s\t%s\t%s\n' % (
            f.getEntryPoint().getOffset(), body.getNumAddresses(), f.getName(),
            ','.join('%08X' % c for c in sorted(callees)), '|'.join(strs)))
print('ExportCallGraph: %d functions -> %s' % (fm.getFunctionCount(), out_path))
