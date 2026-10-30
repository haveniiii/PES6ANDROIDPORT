#!/usr/bin/env bash
# Check the boundaries between the layers of the port (docs/26). Exit 1 on a violation.
#   tools/check_layers.sh
# 1. Game addresses: only runtime/common/pes6_addrs.h spells a guest address (0x0880_0000 ..
#    0x09FF_FFFF) in code; comments may mention them.
# 2. Game-facing C (commentary, rules) reads the game through pes6_game.h, never psp_read/psp_write.
# 3. The toolkit (third_party/psprecomp) stays generic: it includes nothing from runtime/ and
#    uses no PES6_/pes6_ identifier (game knowledge reaches it only through env lists).
# 4. Generated code is not versioned: nothing under build/ is tracked by git.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0
strip_comments() { # C source on stdin, comments blanked, line numbers kept
    python3 -c '
import re, sys
s = sys.stdin.read()
s = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), s, flags=re.S)
s = re.sub(r"//[^\n]*", "", s)
sys.stdout.write(s)'
}

echo "1. guest addresses outside runtime/common/pes6_addrs.h"
for f in runtime/common/*.c runtime/common/*.h runtime/desktop/*.c runtime/desktop/*.h runtime/android/*.c; do
    [ "$f" = runtime/common/pes6_addrs.h ] && continue
    [ "$f" = runtime/common/pes6_tuning.h ] && continue      # the parameter list: addresses are its data
    [ "$f" = runtime/common/pes6_hooks.h ] && continue       # the hook list: idem
    hits=$(strip_comments < "$f" | grep -nE '0x0?8[89A-Fa-f][0-9A-Fa-f]{5}u?\b|0x09[0-9A-Fa-f]{6}u?\b' || true)
    [ -n "$hits" ] && { echo "   $f:"; echo "$hits" | sed 's/^/      /'; fail=1; }
done

echo "2. game-facing C uses pes6_game.h, not raw guest reads"
for f in runtime/common/commentary.c runtime/common/rules.c; do
    hits=$(strip_comments < "$f" | grep -nE 'psp_(read|write)(8|16|32|_f32)\(' || true)
    [ -n "$hits" ] && { echo "   $f:"; echo "$hits" | sed 's/^/      /'; fail=1; }
done

echo "3. the toolkit stays generic"
# (PSP-wide constants such as the user module base 0x08804000 are fine there; the game's are not)
hits=$(grep -rnE '#include *"(\.\./)*runtime/|pes6_(addrs|game|types)\.h' third_party/psprecomp/src third_party/psprecomp/include third_party/psprecomp/tools 2>/dev/null || true)
[ -n "$hits" ] && { echo "$hits" | sed 's/^/   /'; fail=1; }
for f in third_party/psprecomp/src/*.c third_party/psprecomp/src/hle/*.c third_party/psprecomp/tools/allegrexrecomp/*.c; do
    hits=$(strip_comments < "$f" | grep -nE '\b(PES6|pes6)_' | grep -v 'getenv("PES6_' || true)
    [ -n "$hits" ] && { echo "   $f:"; echo "$hits" | sed 's/^/      /'; fail=1; }
done
# known exception, kept on purpose: settings read from the environment under the port's name
envs=$(grep -rhoE 'getenv\("PES6_[A-Z_]+"\)' third_party/psprecomp/src | sort -u | sed -E 's/getenv\("(.*)"\)/\1/' | tr '\n' ' ')
[ -n "$envs" ] && echo "   (allowed: environment settings read by the toolkit's HLE: $envs)"

echo "4. nothing generated under version control"
hits=$(git ls-files build | head -5)
[ -n "$hits" ] && { echo "$hits" | sed 's/^/   /'; fail=1; }

[ $fail -eq 0 ] && echo "PASS: every layer keeps to its side" || echo "FAIL"
exit $fail
