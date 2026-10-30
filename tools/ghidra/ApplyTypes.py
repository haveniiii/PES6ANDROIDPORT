# @category PES6
# Give the decompiled C the game's own types (docs/20 §8): the structs of
# runtime/common/pes6_types.h, the named globals of tools/names/data.txt and, for the functions
# listed by tools/ghidra/type_targets.py, a pes6_chara * first parameter.
# Usage: -postScript ApplyTypes.py <pes6_types.h> <data.txt> [type_targets.tsv]
# Only what Ghidra shows changes: the recompiled code never reads the Ghidra project.
from ghidra.app.util.cparser.C import CParser
from ghidra.program.model.data import (ArrayDataType, DataTypeConflictHandler, FloatDataType,
                                       IntegerDataType, PointerDataType, ShortDataType,
                                       UnsignedCharDataType, UnsignedIntegerDataType,
                                       UnsignedShortDataType)
from ghidra.program.model.symbol import SourceType
from ghidra.app.decompiler import DecompInterface
from ghidra.program.model.pcode import HighFunctionDBUtil
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
header, data_txt = args[0], args[1]
targets = args[2] if len(args) > 2 else None
dtm = currentProgram.getDataTypeManager()

# 1. the structs, parsed from the header's GHIDRA block
text = open(header).read()
block = text.split('GHIDRA-BEGIN', 1)[1].split('*/', 1)[1].split('/* GHIDRA-END', 1)[0]
prelude = ('typedef unsigned char uint8_t; typedef unsigned short uint16_t; typedef unsigned int uint32_t;\n'
           'typedef signed char int8_t; typedef short int16_t; typedef int int32_t;\n')
parser = CParser(dtm, False, None)
parser.parse(prelude + block)
types = {}
for name, dt in parser.getComposites().items():
    types[name] = dtm.addDataType(dt, DataTypeConflictHandler.REPLACE_HANDLER)
for name, dt in parser.getTypes().items():           # the typedefs (pes6_chara -> struct pes6_chara)
    if name.startswith('pes6_'):
        types[name] = dtm.addDataType(dt, DataTypeConflictHandler.REPLACE_HANDLER)
# guest pointers are uint32_t in the header (the port reads them as addresses); in Ghidra they
# become real pointers so the code reads chara->anim->motion
GUEST_PTRS = {('pes6_chara', 'anim'): 'pes6_chara_anim'}
for (sname, field), target in GUEST_PTRS.items():
    st = types.get(sname)
    st = st.getDataType() if hasattr(st, 'getDataType') else st      # through the typedef
    for c in st.getComponents():
        if c.getFieldName() == field:
            st.replaceAtOffset(c.getOffset(), PointerDataType(types[target], 4), 4, field, c.getComment())
print('ApplyTypes: %d types from %s' % (len(types), header))

BUILTIN = {'uint8_t': UnsignedCharDataType(), 'uint16_t': UnsignedShortDataType(),
           'uint32_t': UnsignedIntegerDataType(), 'int16_t': ShortDataType(),
           'int32_t': IntegerDataType(), 'float': FloatDataType()}


def resolve(spec):
    n = None
    if spec.endswith(']'):
        spec, n = spec[:-1].split('[')
    ptr = spec.endswith('*')
    base = spec.rstrip('*').strip()
    dt = BUILTIN.get(base) or types.get(base)
    if dt is None:
        return None
    if ptr:
        dt = PointerDataType(dt, 4)
    if n is not None:
        dt = ArrayDataType(dt, int(n), dt.getLength())
    return dt


# 2. the globals. An overlay's program only holds the overlay: give it a view of the EBOOT's
#    data (uninitialised, nothing is read from it) so its references to the EBOOT's globals
#    can carry names and types too.
mem = currentProgram.getMemory()
EBOOT_DATA = (0x08990000, 0x000B0000)     # 0x08990000..0x08A40000: EBOOT .data/.bss below the overlay regions
EBOOT_BSS = (0x089D6070, 0x08A40000 - 0x089D6070)  # the EBOOT's .bss, which the ELF import left out
view = EBOOT_BSS if currentProgram.getName() == 'EBOOT.elf' else EBOOT_DATA
lo, size = toAddr(view[0]), view[1]
if mem.getBlock('eboot_data') is None and not mem.intersects(lo, lo.add(size - 1)):
    mem.createUninitializedBlock('eboot_data', lo, size, False)
    print('ApplyTypes: created the eboot_data view 0x%08X..0x%08X' % (view[0], view[0] + size))
placed = skipped = 0
for line in open(data_txt):
    line = line.split('#', 1)[0].split()
    if len(line) < 3:
        continue
    addr, name, dt = toAddr(int(line[0], 16)), line[1], resolve(line[2])
    if dt is None or not mem.contains(addr) or not mem.contains(addr.add(dt.getLength() - 1)):
        skipped += 1
        continue
    createLabel(addr, name, True, SourceType.USER_DEFINED)
    try:
        clearListing(addr, addr.add(dt.getLength() - 1))
        createData(addr, dt)
        placed += 1
    except Exception as e:
        print('ApplyTypes: %s %s: %s' % (line[0], name, e))
print('ApplyTypes: %d globals typed, %d not in this program' % (placed, skipped))

# 3. parameters
if targets:
    chara_ptr = PointerDataType(types['pes6_chara'], 4)
    fm = currentProgram.getFunctionManager()
    ifc = DecompInterface()
    ifc.openProgram(currentProgram)
    monitor = ConsoleTaskMonitor()
    done = failed = 0
    for line in open(targets):
        a, idx, _t = line.rstrip('\n').split('\t')
        f = fm.getFunctionAt(toAddr(int(a, 16)))
        if f is None:
            failed += 1
            continue
        if f.getParameterCount() < int(idx):           # commit what the decompiler found first
            res = ifc.decompileFunction(f, 60, monitor)
            if res and res.getHighFunction():
                HighFunctionDBUtil.commitParamsToDatabase(res.getHighFunction(), True,
                    HighFunctionDBUtil.ReturnCommitOption.NO_COMMIT, SourceType.ANALYSIS)
        if f.getParameterCount() < int(idx):
            failed += 1
            continue
        p = f.getParameter(int(idx) - 1)
        try:
            p.setDataType(chara_ptr, SourceType.USER_DEFINED)
            p.setName('chara', SourceType.USER_DEFINED)
            done += 1
        except Exception as e:
            print('ApplyTypes: %s: %s' % (a, e))
            failed += 1
    print('ApplyTypes: %d parameters typed pes6_chara *, %d failed (%s)' % (done, failed, targets))
