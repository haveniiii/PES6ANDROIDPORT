#!/usr/bin/env bash
# Where the game thread spends its CPU (docs/18 §6-7): runs the reference match on the GPU path
# (--window --gpu, no real-time cap), samples it with macOS `sample` during the match and prints
# the self time of the game thread by function and by layer.
#   tools/perf_profile.sh [build dir, default build-prof] [start s, default 16] [length s, default 12]
# build-prof = the desktop host configured with -DCMAKE_C_FLAGS=-DPES6_GE_PROFILE, which keeps the
# GE stages (draw_prim, vertex_transform, draw_setup, gl_draw...) out of line so they show apart.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
B=${1:-build-prof}
START=${2:-16}
LEN=${3:-12}
OUT=build/perf/profile_$B
rm -rf "$OUT" && mkdir -p "$OUT/io"
PES6_BUILD="$B" PES6_UNLIMITED=1 PES6_VERSION=original PES6_MENU=0 PES6_IO_ROOT="$OUT/io" \
    PES6_STOP_AT=8@3000 PES6_SECONDS=1200 tools/ref_match.sh --window --gpu > "$OUT/log.txt" 2>&1 &
sleep "$START"
pid=$(pgrep -f "$B/pes6 build/EBOOT.elf" | head -1)
sample "$pid" "$LEN" -file "$OUT/sample.txt" > /dev/null 2>&1
kill "$pid" 2> /dev/null
wait 2> /dev/null
python3 - "$OUT/sample.txt" <<'EOF'
import collections, re, sys
L = open(sys.argv[1]).read().split('\n')
starts = [i for i, l in enumerate(L) if re.match(r'^ +\d+ Thread_', l)]
block = L[starts[0] + 1:starts[1] if len(starts) > 1 else len(L)]       # the main (game) thread
nodes = []
for l in block:
    m = re.match(r'^([ +!:|]*)(\d+) (\S+)', l)
    if m:
        nodes.append((len(m.group(1)), int(m.group(2)), m.group(3)))
own = collections.Counter()
for i, (d, n, f) in enumerate(nodes):
    j, child_depth, children = i + 1, None, 0
    while j < len(nodes) and nodes[j][0] > d:
        child_depth = child_depth or nodes[j][0]
        if nodes[j][0] == child_depth:
            children += nodes[j][1]
        j += 1
    own[f] += n - children
total = sum(own.values())
def layer(f):
    if f.startswith(('psp_body_', 'ovl_', 'psp_func_', 'psp_at_', 'psp_import', 'psp_orig')): return 'game code'
    if f.startswith(('psp_v', 'psp_lv', 'psp_sv')) or f in ('get_src', 'put_dst'): return 'VFPU'
    if f in ('run_list',): return 'GE: command loop'
    if f in ('draw_prim', 'vertex_transform', 'vformat_decode'): return 'GE: vertices'
    if f in ('draw_setup', 'tex_setup', 'tex_fetch'): return 'GE: state'
    if f.startswith('gl_'): return 'GE: GL backend'
    if 'psynch' in f or 'wait' in f.lower() or 'mach_msg' in f or 'trap' in f: return 'waits/kernel'
    return 'other'
by = collections.Counter()
for f, n in own.items():
    by[layer(f)] += n
print('game thread, %d samples' % total)
for k, n in by.most_common():
    print('  %-20s %6d  %5.1f%%' % (k, n, 100.0 * n / total))
print('top functions:')
for f, n in own.most_common(15):
    print('  %-34s %6d  %5.1f%%' % (f, n, 100.0 * n / total))
EOF
