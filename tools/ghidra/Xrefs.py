# @category PES6
# List all references (call sites / data refs) to one or more addresses.
# Usage: -postScript Xrefs.py <address> [address2 ...]

for a in getScriptArgs():
    addr = toAddr(a)
    refs = getReferencesTo(addr)
    print("== refs to %s ==" % addr)
    count = 0
    for r in refs:
        fromAddr = r.getFromAddress()
        func = getFunctionContaining(fromAddr)
        fname = func.getName() if func else "?"
        fentry = func.getEntryPoint() if func else "?"
        print("  %s  %-18s  in %s (%s)" % (fromAddr, r.getReferenceType(), fname, fentry))
        count += 1
    print("  total: %d" % count)
