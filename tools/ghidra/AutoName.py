# @category PES6
# Propose names for the game's functions from firmware evidence (docs/20 §5).
# Usage: -postScript AutoName.py <out.tsv> [EBOOT names file, for an overlay]
# Writes "<addr>\t<name>\t<rule>\t<evidence>" lines; it does not rename anything.
#
# Rules (each needs a constant argument recovered at the call site):
#   thread    sceKernelCreateThread(name, entry, ...)      -> thread_<name>
#   callback  sceKernelCreateCallback(name, func, arg)     -> callback_<name>
#   alarm     sceKernelSetAlarm(clock, handler, arg)       -> alarm_<caller>
#   intr      sceKernelRegisterSubIntrHandler(i, n, h, a)  -> intr_<i>_<n>
#   wrapper   a short function whose only call is one firmware stub -> <stub>_w
#   task      task_create(init, main, exit, work, arg, "name") (0x0885060C,
#             docs/20 §5; the 6th argument is in $t1) -> <name>_init/_main/_exit.
#             Found by scanning every jal, so it also works on an overlay program.
import re

out_path = getScriptArgs()[0]
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()

REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3']


def stub_name(f):
    n = f.getName()
    return n[3:] if n.startswith('zz_') else None


def cstring(addr):
    try:
        b = []
        a = toAddr(addr)
        for _ in range(64):
            c = mem.getByte(a) & 0xff
            if c == 0:
                break
            if c < 0x20 or c > 0x7e:
                return None
            b.append(chr(c))
            a = a.add(1)
        s = ''.join(b)
        return s if len(s) >= 2 else None
    except Exception:
        return None


def ident(s):
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')
    return s[:40]


def regs_at_call(call_ins, func):
    """Constant a0..a3 at a jal: simulate lui/addiu/ori/move from up to 24
    instructions before it, plus the delay slot. None = unknown."""
    ins = call_ins
    seq = []
    for _ in range(24):
        ins = ins.getPrevious()
        if ins is None or not func.getBody().contains(ins.getAddress()):
            break
        if ins.getFlowType().isJump() or ins.getFlowType().isCall():
            break
        seq.append(ins)
    seq.reverse()
    nxt = call_ins.getNext()
    if nxt is not None:
        seq.append(nxt)          # delay slot
    val = {}
    for i in seq:
        m = i.getMnemonicString().lstrip('_')
        ops = [i.getDefaultOperandRepresentation(k) for k in range(i.getNumOperands())]
        if not ops:
            continue
        dst = ops[0]
        try:
            if m == 'lui':
                val[dst] = (int(ops[1], 0) << 16) & 0xffffffff
            elif m in ('addiu', 'addi') and len(ops) == 3:
                src = ops[1]
                imm = int(ops[2], 0)
                if imm >= 0x8000:
                    imm -= 0x10000
                base = 0 if src == 'zero' else val.get(src)
                val[dst] = None if base is None else (base + imm) & 0xffffffff
            elif m == 'ori' and len(ops) == 3:
                base = 0 if ops[1] == 'zero' else val.get(ops[1])
                val[dst] = None if base is None else base | int(ops[2], 0)
            elif m in ('move', 'or', 'addu') and len(ops) >= 2:
                srcs = [o for o in ops[1:] if o != 'zero']
                val[dst] = val.get(srcs[0]) if len(srcs) == 1 else (0 if not srcs else None)
            elif m == 'li':
                val[dst] = int(ops[1], 0) & 0xffffffff
            else:
                val[dst] = None
        except Exception:
            val[dst] = None
    return [val.get(r) for r in ('a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3')]


def func_at(v):
    if v is None:
        return None
    try:
        return fm.getFunctionAt(toAddr(v))
    except Exception:
        return None


props = []   # (addr, name, rule, evidence)
stubs = {}
for f in fm.getFunctions(True):
    s = stub_name(f)
    if s:
        stubs[f.getEntryPoint()] = s

wrappers = {}
# wrappers: <= 24 instructions, exactly one call target and it is a stub
for f in fm.getFunctions(True):
    if f.getEntryPoint() in stubs:
        continue
    body = f.getBody()
    if body.getNumAddresses() > 24 * 4:
        continue
    targets = set()
    for i in listing.getInstructions(body, True):
        ft = i.getFlowType()
        if ft.isCall() or (ft.isJump() and not ft.isConditional()):
            for t in i.getFlows():
                if not body.contains(t):
                    targets.add(t)
    if len(targets) == 1:
        t = list(targets)[0]
        if t in stubs:
            props.append((f.getEntryPoint(), stubs[t] + '_w', 'wrapper', 'only call: %s' % stubs[t]))
            wrappers[f.getEntryPoint()] = stubs[t]


# a call to a wrapper counts as a call to its stub (args assumed forwarded;
# the string/function checks below reject the cases where they are not).
# Calls are found by scanning every jal, keyed by target address, so an overlay
# (which calls the EBOOT's stubs, wrappers and task_create by address) works too
# when the EBOOT's names file is given as the second argument.
TASK_CREATE = 0x0885060C
callees = {a.getOffset(): n for a, n in stubs.items()}
callees.update({a.getOffset(): n for a, n in wrappers.items()})
callees[TASK_CREATE] = 'task_create'
if len(getScriptArgs()) > 1:
    for line in open(getScriptArgs()[1]):
        p = line.split('#', 1)[0].split()
        if len(p) >= 2:
            n = re.sub(r'^zz_', '', p[1])
            n = n[:-2] if n.endswith('_w') else n
            if re.match(r'(__)?sce', n):
                callees.setdefault(int(p[0], 16), n)

for ins in listing.getInstructions(True):
    if ins.getMnemonicString().lstrip('_') != 'jal':
        continue
    fl = ins.getFlows()
    if len(fl) != 1 or fl[0].getOffset() not in callees:
        continue
    sname = callees[fl[0].getOffset()]
    site = ins.getAddress()
    caller = getFunctionContaining(site)
    if caller is None:
        continue
    a = regs_at_call(ins, caller)
    ev = '%s@%s' % (sname, site)
    if sname == 'sceKernelCreateThread':
        n, f = cstring(a[0]), func_at(a[1])
        if n and f:
            props.append((f.getEntryPoint(), 'thread_' + ident(n), 'thread', ev + ' "%s"' % n))
    elif sname == 'sceKernelCreateCallback':
        n, f = cstring(a[0]), func_at(a[1])
        if n and f:
            props.append((f.getEntryPoint(), 'callback_' + ident(n), 'callback', ev + ' "%s"' % n))
    elif sname == 'sceKernelSetAlarm':
        f = func_at(a[1])
        if f:
            props.append((f.getEntryPoint(), 'alarm_handler_%s' % str(caller.getEntryPoint()).upper()[-6:], 'alarm', ev))
    elif sname == 'sceKernelRegisterSubIntrHandler':
        f = func_at(a[2])
        if f and a[0] is not None and a[1] is not None:
            props.append((f.getEntryPoint(), 'intr_handler_%d_%d' % (a[0], a[1]), 'intr', ev))
    elif sname == 'task_create':
        n = cstring(a[5]) if a[5] is not None else None
        if not n:
            continue
        for k, role in enumerate(('init', 'main', 'exit')):
            f = func_at(a[k])
            if f is None and a[k]:
                f = createFunction(toAddr(a[k]), None)
            if f is not None:
                props.append((f.getEntryPoint(), '%s_%s' % (ident(n), role), 'task',
                              'task_create@%s "%s"' % (site, n)))

# string: a function that uses a debug string starting with an identifier
# ("TexAdd(%p): ...") -> str_<identifier>. Only a hint; reviewed before curating.
from ghidra.program.model.data import StringDataType
seen = set()
for d in listing.getDefinedData(True):
    if not d.hasStringValue():
        continue
    s = str(d.getValue())
    m = re.match(r'([A-Za-z_][A-Za-z0-9_]{3,})[ (:]', s)
    if not m or not re.search(r'[%:(]', s):
        continue
    users = set()
    for r in getReferencesTo(d.getAddress()):
        f = getFunctionContaining(r.getFromAddress())
        if f:
            users.add(f.getEntryPoint())
    for u in users:
        if (u, m.group(1)) not in seen:
            seen.add((u, m.group(1)))
            props.append((u, 'str_' + m.group(1), 'string', '"%s"' % re.sub(r'\s+', ' ', s[:60]).strip()))

with open(out_path, 'w') as fh:
    for addr, name, rule, ev in props:
        fh.write('%s\t%s\t%s\t%s\n' % (str(addr).upper(), name, rule, ev))
print('AutoName: %d proposals -> %s' % (len(props), out_path))
