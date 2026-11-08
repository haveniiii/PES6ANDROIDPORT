#!/bin/bash
# The patches' importer (docs/38 §9): makes test patches and checks that each one is accepted or
# refused, with its reason. Needs data/pes6.iso (the PSP archives are compared with it) and
# optionally E_SOUND=<a PS2 e_sound.afs> for the commentary bank. Nothing is written outside
# build/patchtest/.
#
#   tools/patch_test.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/build/patchtest"
ISO="${ISO:-$ROOT/data/pes6.iso}"
E_SOUND="${E_SOUND:-$HOME/Downloads/e_sound.afs}"
TOOL="$T/pes6patch"
mkdir -p "$T"
clang -O1 -Wall -Wextra -Werror -I"$ROOT/runtime/common" "$ROOT/runtime/desktop/patch_tool.c" \
    "$ROOT/runtime/common/patch.c" "$ROOT/runtime/common/pcof.c" "$ROOT/runtime/common/lang.c" -lz -o "$TOOL" || exit 1
rm -rf "$T/mods" "$T/audio" "$T/in"
mkdir -p "$T/mods" "$T/in"

python3 - "$ROOT" "$T/in" "$ISO" <<'EOF' || exit 1
import json, os, struct, sys, zipfile
root, out, iso = sys.argv[1:]
pech = os.path.join(root, 'mods/pech2026')
tex = sorted(os.listdir(os.path.join(pech, 'textures')))[:3]
save = os.path.join(pech, 'save/ULES004760013000')

def meta(**kw):
    m = {"format": 1, "id": "prueba", "name": "Parche de prueba", "author": "Test", "version": "1.0",
         "description": "Para tools/patch_test.sh", "base": "ULES00476", "replaces": ["textures", "save"]}
    m.update(kw)
    return json.dumps(m, ensure_ascii=False)

def mk(name, files):
    with zipfile.ZipFile(os.path.join(out, name), 'w', zipfile.ZIP_DEFLATED) as z:
        for arc, src in files:
            if isinstance(src, bytes): z.writestr(arc, src)
            else: z.write(src, arc)

base = [('textures/' + t, os.path.join(pech, 'textures', t)) for t in tex]
base += [('save/ULES004760013000/DATA.BIN', os.path.join(save, 'DATA.BIN')),
         ('save/ULES004760013000/PARAM.BIN', os.path.join(save, 'PARAM.BIN'))]
mk('ok_json.zip', [('mod.json', meta().encode())] + base + [('LEEME.txt', b'hola')])
mk('ok_json_folder.zip', [('Mi Parche/mod.json', meta(id='carpeta', name='Con carpeta (tilde: Ñuñoa)').encode())] +
   [('Mi Parche/' + a, s) for a, s in base] + [('__MACOSX/._x', b'junk')])
mk('ok_update.zip', [('mod.json', meta(version='1.1').encode())] + base)
mk('bad_dotdot.zip', [('mod.json', meta().encode())] + base + [('textures/../../evil.pam', b'P7\nENDHDR\n')])
mk('bad_undeclared.zip', [('mod.json', meta(replaces=['textures']).encode())] + base)
mk('bad_missing.zip', [('mod.json', meta(replaces=['textures', 'save', 'faces']).encode())] + base)
mk('bad_unknown_dir.zip', [('mod.json', meta().encode())] + base + [('sonidos/x.bin', b'1')])
mk('bad_format.zip', [('mod.json', meta(format=9).encode())] + base)
mk('bad_id.zip', [('mod.json', meta(id='../x').encode())] + base)
mk('bad_base.zip', [('mod.json', meta(base='ULUS10039').encode())] + base)
mk('bad_json.zip', [('mod.json', b'{"format": 1, "id": ')] + base)
mk('bad_save_size.zip', [('mod.json', meta().encode())] + base[:3] +
   [('save/ULES004760013000/DATA.BIN', b'\0' * 1191952), ('save/ULES004760013000/PARAM.BIN', os.path.join(save, 'PARAM.BIN'))])
mk('bad_two_json.zip', [('a/mod.json', meta().encode()), ('b/mod.json', meta().encode())])
# raw zips
mk('Parche Bruto 2027.zip', [('Parche/ULES00476PATCH/DATA.BIN', os.path.join(save, 'DATA.BIN')),
                             ('Parche/ULES00476PATCH/PARAM.BIN', os.path.join(save, 'PARAM.BIN')), ('Parche/leeme.txt', b'x')])
mk('raw_pc_afs.zip', [('dat/0_text.afs', b'AFS\0' + struct.pack('<I', 3) + b'\0' * 4096)])
mk('raw_kitserver.zip', [('kitserver/GDB/uni/123/ga/all.png', b'png'), ('readme.txt', b'x')])
mk('raw_pcof.zip', [('save/KONAMI-WIN32PES6OPT', b'\0' * 100)])
mk('raw_dkz.zip', [('kits/a.dkz', b'\0' * 100)])
mk('raw_unknown.zip', [('setup.exe', b'MZ')])
mk('raw_empty.zip', [('leeme.txt', b'x')])
# PSP archives from the disc: e_text.afs with one member changed, and one grown
f = open(iso, 'rb')
def rd(o, n): f.seek(o); return f.read(n)
pvd = rd(16 * 2048, 2048)
def ls(lba, size):
    d = {}
    for s in range(0, size, 2048):
        sec = rd(lba * 2048 + s, 2048); o = 0
        while o < 2048 and sec[o]:
            r = sec[o:o + sec[o]]; n = r[33:33 + r[32]].decode('latin1').split(';')[0]
            d[n] = struct.unpack('<I', r[2:6])[0], struct.unpack('<I', r[10:14])[0]; o += sec[o]
    return d
d = ls(struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0])
d = ls(*d['PSP_GAME']); d = ls(*d['USRDIR'])
lba, size = d['e_text.afs']
afs = bytearray(rd(lba * 2048, size))
s3, n3 = struct.unpack('<II', afs[3 * 8:3 * 8 + 8])
afs[s3 * 2048 + 100] ^= 0xFF
open(os.path.join(out, 'e_text.afs'), 'wb').write(afs)
mk('raw_psp_afs.zip', [('PSP_GAME/USRDIR/e_text.afs', bytes(afs)), ('info.nfo', b'x')])
grown = bytearray(afs)
struct.pack_into('<I', grown, 3 * 8 + 4, n3 + 64)
open(os.path.join(out, 'grown/e_text.afs') if os.makedirs(os.path.join(out, 'grown'), exist_ok=True) is None else '', 'wb').write(grown)
open(os.path.join(out, 'other_count/e_text.afs') if os.makedirs(os.path.join(out, 'other_count'), exist_ok=True) is None else '', 'wb').write(afs[:8 * 2] + b'\0' * (len(afs) - 16))
open(os.path.join(out, 'same/e_text.afs') if os.makedirs(os.path.join(out, 'same'), exist_ok=True) is None else '', 'wb').write(rd(lba * 2048, size))
# a bank-shaped archive nobody catalogued
open(os.path.join(out, 'x_sound.afs'), 'wb').write(b'AFS\0' + struct.pack('<I', 13127) + bytes(range(256)) * 420)
open(os.path.join(out, 'notes.pdf'), 'wb').write(b'%PDF-1.4')
EOF

# PC option files (docs/38 §8): a stand-in made by encrypting a plain database with tools/pes6_pcof.py
# (no real PC option file in the repo), over the original's save of this Mac (BASE).
BASE="${BASE:-$ROOT/build/SAVEDATA/ULES004760013000}"
mkdir -p "$T/base" && cp "$BASE/DATA.BIN" "$BASE/PARAM.BIN" "$T/base/" || exit 1
python3 - "$ROOT" "$T/in" <<'EOF2' || exit 1
import os, subprocess, sys, zipfile, json
root, out = sys.argv[1:]
plain = bytearray(open(os.path.join(root, 'mods/pech2026/save/ULES004760013000/DATA.BIN'), 'rb').read())
plain[-64:] = bytes(64)                       # a plain database has a zero tail
open(os.path.join(out, 'plain.bin'), 'wb').write(plain)
of = os.path.join(out, 'KONAMI-WIN32PES6OPT')
subprocess.check_call([sys.executable, os.path.join(root, 'tools/pes6_pcof.py'), 'encrypt', os.path.join(out, 'plain.bin'), of])
data = open(of, 'rb').read()
bad = bytearray(data); bad[700000] ^= 1
os.makedirs(os.path.join(out, 'bad'), exist_ok=True)
open(os.path.join(out, 'bad/KONAMI-WIN32PES6OPT'), 'wb').write(bad)
plain2 = bytearray(plain); plain2[700001] ^= 0x55     # a second season of the same patch
open(os.path.join(out, 'plain2.bin'), 'wb').write(plain2)
of2 = os.path.join(out, 'of2')
subprocess.check_call([sys.executable, os.path.join(root, 'tools/pes6_pcof.py'), 'encrypt', os.path.join(out, 'plain2.bin'), of2])
def mk(name, files):
    with zipfile.ZipFile(os.path.join(out, name), 'w', zipfile.ZIP_DEFLATED) as z:
        for arc, data in files: z.writestr(arc, data)
mk('Liga PC 2027.zip', [('Liga PC 2027/save/folder1/KONAMI-WIN32PES6OPT', data), ('Liga PC 2027/leeme.txt', b'x')])
meta = {"format": 1, "id": "liga-of", "name": "Liga con option file", "author": "Test", "version": "1.0",
        "base": "ULES00476", "replaces": ["save"]}
mk('of_json.zip', [('mod.json', json.dumps(meta)), ('save/KONAMI-WIN32PES6OPT', data)])
meta["version"] = "1.1"
mk('of_json_v2.zip', [('mod.json', json.dumps(meta)), ('save/KONAMI-WIN32PES6OPT', open(of2, 'rb').read())])
save = os.path.join(root, 'mods/pech2026/save/ULES004760013000')
mk('of_and_save.zip', [('save/KONAMI-WIN32PES6OPT', data), ('ULES004760013000/DATA.BIN', open(save + '/DATA.BIN', 'rb').read()),
                       ('ULES004760013000/PARAM.BIN', open(save + '/PARAM.BIN', 'rb').read())])
EOF2
python3 "$ROOT/tools/pcof_to_psp.py" "$T/in/KONAMI-WIN32PES6OPT" "$T/base/DATA.BIN" "$T/in/py.bin"

pass=0; fail=0
check() {   # want(ACCEPTED|REFUSED) words-in-message file [import args...]
    local want="$1" words="$2"; shift 2
    local out
    out=$(LANG=es_CL.UTF-8 "$TOOL" "$@" 2>/dev/null)
    if [[ "$out" == "$want"* ]] && grep -qi -- "$words" <<<"$out"; then
        pass=$((pass + 1)); printf 'ok    %-28s %s\n' "$(basename "${@: -1}")" "$out"
    else
        fail=$((fail + 1)); printf 'FAIL  %-28s want %s /%s/, got: %s\n' "$(basename "${@: -1}")" "$want" "$words" "$out"
    fi
}
M="$T/mods"; I="$T/in"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/ok_json.zip"
check ACCEPTED "actualizado"        import "$M" "$ISO" "$I/ok_update.zip"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/ok_json_folder.zip"
check REFUSED  "ruta no segura"     import "$M" "$ISO" "$I/bad_dotdot.zip"
check REFUSED  "no figura"          import "$M" "$ISO" "$I/bad_undeclared.zip"
check REFUSED  "no hay faces"       import "$M" "$ISO" "$I/bad_missing.zip"
check REFUSED  "carpeta desconocida" import "$M" "$ISO" "$I/bad_unknown_dir.zip"
check REFUSED  "mas nueva"          import "$M" "$ISO" "$I/bad_format.zip"
check REFUSED  "id"                 import "$M" "$ISO" "$I/bad_id.zip"
check REFUSED  "otro juego"         import "$M" "$ISO" "$I/bad_base.zip"
check REFUSED  "JSON"               import "$M" "$ISO" "$I/bad_json.zip"
check REFUSED  "cifrado"            import "$M" "$ISO" "$I/bad_save_size.zip"
check REFUSED  "mas de un"          import "$M" "$ISO" "$I/bad_two_json.zip"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/Parche Bruto 2027.zip"
check REFUSED  "PC o PS2"           import "$M" "$ISO" "$I/raw_pc_afs.zip"
check REFUSED  "Kitserver"          import "$M" "$ISO" "$I/raw_kitserver.zip"
check REFUSED  "tamano es otro"     import "$M" "$ISO" "$I/raw_pcof.zip"
check REFUSED  "dkz"                import "$M" "$ISO" "$I/raw_dkz.zip"
check REFUSED  "setup.exe"          import "$M" "$ISO" "$I/raw_unknown.zip"
check REFUSED  "nada"               import "$M" "$ISO" "$I/raw_empty.zip"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/raw_psp_afs.zip"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/e_text.afs"
check REFUSED  "agrandar"           import "$M" "$ISO" "$I/grown/e_text.afs"
check REFUSED  "otra version"       import "$M" "$ISO" "$I/other_count/e_text.afs"
check REFUSED  "igual al del disco" import "$M" "$ISO" "$I/same/e_text.afs"
check REFUSED  "Instala primero"    import "$M" -      "$I/e_text.afs"
check REFUSED  "no calzan"          import "$M" "$ISO" "$I/x_sound.afs"
check REFUSED  "no es un parche"    import "$M" "$ISO" "$I/notes.pdf"
# the option file converter: C against Python, byte for byte
check ACCEPTED "encrypted"          pcof "$I/KONAMI-WIN32PES6OPT" "$T/base/DATA.BIN" "$I/c.bin"
if cmp -s "$I/c.bin" "$I/py.bin"; then pass=$((pass + 1)); echo "ok    C == tools/pcof_to_psp.py"; else fail=$((fail + 1)); echo "FAIL  C != tools/pcof_to_psp.py"; fi
check ACCEPTED "plain"              pcof "$I/plain.bin" "$T/base/DATA.BIN" "$I/c2.bin"
if cmp -s "$I/c2.bin" "$I/py.bin"; then pass=$((pass + 1)); echo "ok    plain == encrypted"; else fail=$((fail + 1)); echo "FAIL  plain != encrypted"; fi
check REFUSED  "checksums"          pcof "$I/bad/KONAMI-WIN32PES6OPT" "$T/base/DATA.BIN" "$I/c3.bin"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/KONAMI-WIN32PES6OPT" "$T/audio" "$T/base"
if cmp -s "$M/option-file/save/ULES004760013000/DATA.BIN" "$I/py.bin"; then pass=$((pass + 1)); echo "ok    imported save == tools/pcof_to_psp.py"; else fail=$((fail + 1)); echo "FAIL  imported save differs"; fi
check REFUSED  "sumas de control"   import "$M" "$ISO" "$I/bad/KONAMI-WIN32PES6OPT" "$T/audio" "$T/base"
check REFUSED  "Juega una vez"      import "$M" "$ISO" "$I/KONAMI-WIN32PES6OPT" "$T/audio" -
check REFUSED  "no es un option file cifrado" import "$M" "$ISO" "$I/plain.bin" "$T/audio" "$T/base"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/Liga PC 2027.zip" "$T/audio" "$T/base"
check ACCEPTED "instalado"          import "$M" "$ISO" "$I/of_json.zip" "$T/audio" "$T/base"
OLD=$(python3 -c "import sys;x=0xcbf29ce484222325
for b in open(sys.argv[1],'rb').read(): x=((x^b)*0x100000001b3)&0xffffffffffffffff
print('%016x'%x)" "$M/liga-of/save/ULES004760013000/DATA.BIN")
check ACCEPTED "actualizado"        import "$M" "$ISO" "$I/of_json_v2.zip" "$T/audio" "$T/base"
if grep -q "ULES004760013000/DATA.BIN $OLD" "$M/liga-of/save/history.txt" 2>/dev/null; then pass=$((pass + 1)); echo "ok    update keeps the old template in history.txt"
else fail=$((fail + 1)); echo "FAIL  no old template in history.txt"; fi
check REFUSED  "mas de una base"    import "$M" "$ISO" "$I/of_and_save.zip" "$T/audio" "$T/base"
if [ -f "$E_SOUND" ]; then
    check ACCEPTED "Brackley"       bank "$T/audio" "$E_SOUND"
    check REFUSED  "No es un banco" bank "$T/audio" "$I/e_text.afs"
fi
echo
"$TOOL" list "$M"
ls "$M"/raw-psp-afs/afs/e_text.afs 2>/dev/null | sed 's/^/raw-psp-afs member: /'
[ -d "$M/.import-"* ] 2>/dev/null && { echo "FAIL leftovers in $M"; fail=$((fail + 1)); }
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
