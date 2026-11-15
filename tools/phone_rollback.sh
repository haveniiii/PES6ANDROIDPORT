#!/usr/bin/env bash
# Rollback measurement on the phone, end to end (docs/27 §12, AGENTS.md "medir el rollback en el Redmi").
# What the user runs with the Redmi on wireless adb; messages in Spanish, the user reads them.
#
#   tools/phone_rollback.sh                    everything: find the phone, install the probe APK if the
#                                              phone has another, two scripted matches (30 and 60 Hz)
#                                              with PES6_RB=dirty,replay, bring the logs, print the numbers
#   tools/phone_rollback.sh analyze <30.log> <60.log>   the numbers from logs already on the Mac
#
# The app starts itself, goes to an Exhibition match with tools/ref_match.sh's input and measures; the
# phone must stay unlocked with the screen on (about 4 min per rate). env.txt is put back at the end,
# also on Ctrl-C. Never runs on the Redmi 15C (TWHENVNNX4PZYT6L).
# Environment: ANDROID_SERIAL, ADB, PKG (default com.pes6port.iso), APK (default
# build/android/PES6-1.3.0-rb1.apk), NO_INSTALL=1, RATES ("30 60"), MAX_MIN (per rate, default 10).
# Results: build/phone_rb/<date>/ (build/phone_rb/latest).
set -uo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
APK=${APK:-build/android/PES6-1.3.0-rb1.apk}
RATES=${RATES:-30 60}
MAX_MIN=${MAX_MIN:-10}
NEVER=TWHENVNNX4PZYT6L                        # the Redmi 15C: not to be touched
KEYS="PES6_INPUT PES6_VERSION PES6_MENU PES6_RB PES6_RB_FROM PES6_RB_FRAMES PES6_RB_K PES6_RB_SKIPDRAW PES6_RB_TAIL PES6_SIM60 PES6_SPIN PES6_BOT"

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

if [ "${1:-}" = analyze ]; then
    [ $# -eq 3 ] || die "uso: tools/phone_rollback.sh analyze <pes6_30.log> <pes6_60.log>"
    exec python3 tools/rb_report.py "$2" "$3"
fi
[ $# -eq 0 ] || die "uso: tools/phone_rollback.sh   (o: analyze <30.log> <60.log>)"

phone_help() {
    cat <<'EOF'

No hay ningún teléfono conectado por adb. En el Redmi:
  1. Misma red WiFi que el Mac.
  2. Ajustes > Ajustes adicionales > Opciones de desarrollador > Depuración inalámbrica: activarla
     (si pregunta, "Permitir siempre en esta red").
  3. Si el Mac nunca se vinculó o dejó de reconocerlo: dentro de Depuración inalámbrica,
     "Vincular dispositivo con código de vinculación", y en el Mac:
         ~/Library/Android/sdk/platform-tools/adb pair IP:PUERTO_DE_VINCULACIÓN CÓDIGO
  4. Conectar con la IP y el puerto que muestra la pantalla de Depuración inalámbrica
     (no es el de vincular):
         ~/Library/Android/sdk/platform-tools/adb connect IP:PUERTO
  5. Volver a correr: tools/phone_rollback.sh
Si adb dice "offline" o no lo ve: adb kill-server; adb start-server, y repetir el paso 4.
EOF
}

serialno() { "$ADB" -s "$1" shell getprop ro.serialno 2>/dev/null | tr -d '\r'; }

find_phone() {
    [ -x "$ADB" ] || die "no encuentro adb en $ADB (ADB=... para otro)"
    if [ -n "${ANDROID_SERIAL:-}" ]; then
        st=$("$ADB" -s "$ANDROID_SERIAL" get-state 2>/dev/null)
        [ "$st" = device ] || { say "ANDROID_SERIAL=$ANDROID_SERIAL no está listo (estado: ${st:-sin conexión})."; phone_help; exit 2; }
        [ "$(serialno "$ANDROID_SERIAL")" = "$NEVER" ] && die "ese es el Redmi 15C: este script no lo usa." 2
        return
    fi
    list() { "$ADB" devices 2>/dev/null | awk 'NR > 1 && NF >= 2 && $1 !~ /^emulator-/ {print $1, $2}'; }
    devs=$(list)
    if ! printf '%s\n' "$devs" | grep -q ' device$'; then
        for ep in $("$ADB" mdns services 2>/dev/null | awk '/_adb-tls-connect/ {print $NF}'); do
            say "Probando adb connect $ep ..."
            "$ADB" connect "$ep" 2>&1 | sed 's/^/   /'
        done
        devs=$(list)
    fi
    if printf '%s\n' "$devs" | grep -q ' unauthorized$'; then
        say "El teléfono está, pero no autorizó a este Mac: acepta el aviso «¿Permitir depuración?» en su pantalla y vuelve a correr el script."
        exit 2
    fi
    ready=""
    for s in $(printf '%s\n' "$devs" | awk '$2 == "device" {print $1}'); do
        if [ "$(serialno "$s")" = "$NEVER" ]; then say "(el Redmi 15C está conectado: se deja fuera)"; else ready="$ready $s"; fi
    done
    n=$(printf '%s\n' $ready | grep -c .)
    if [ "$n" -eq 0 ]; then
        [ -n "$devs" ] && say "adb ve: $devs"
        phone_help
        exit 2
    fi
    if [ "$n" -gt 1 ]; then
        say "Hay $n teléfonos conectados:"
        for s in $ready; do say "   $s  $("$ADB" -s "$s" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"; done
        die "elige uno: ANDROID_SERIAL=<serie> tools/phone_rollback.sh" 2
    fi
    export ANDROID_SERIAL=${ready# }
}

sh_() { "$ADB" shell "$@" 2>/dev/null | tr -d '\r'; }

# ---- 1. phone, app, APK -------------------------------------------------------------------------
step "1. Teléfono"
find_phone
say "Teléfono: $ANDROID_SERIAL ($(sh_ getprop ro.product.model))"
PKG=${PKG:-com.pes6port.iso}
F=/sdcard/Android/data/$PKG/files
sh_ pm list packages "$PKG" | grep -qx "package:$PKG" || die "$PKG no está instalada en el teléfono (hace falta la app con la ISO ya puesta)."
sh_ ls "$F" >/dev/null || die "no puedo leer $F en el teléfono"

if [ "${NO_INSTALL:-}" != 1 ]; then
    [ -f "$APK" ] || die "no está la APK de la sonda: $APK (o NO_INSTALL=1 si el teléfono ya la tiene)"
    here=$(shasum -a 256 "$APK" | cut -d' ' -f1)
    there=$(sh_ sha256sum "$(sh_ pm path "$PKG" | sed -n 's/^package://p' | head -1)" | cut -d' ' -f1)
    if [ "$here" != "$there" ]; then
        step "Instalando la APK de la sonda ($APK; conserva la ISO y el guardado)"
        sh_ am force-stop "$PKG"
        "$ADB" install -r "$APK" 2>&1 | tail -2
        [ "${PIPESTATUS[0]}" -eq 0 ] || die "no se pudo instalar. No desinstales la app: borraría la ISO y el guardado."
    else
        say "La APK del teléfono ya es la de la sonda."
    fi
fi

OUT=build/phone_rb/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT" && ln -sfn "$(basename "$OUT")" build/phone_rb/latest

# ---- 2. env.txt, kept and always put back ---------------------------------------------------------
sh_ am force-stop "$PKG"
HAD_ENV=0
if "$ADB" pull "$F/env.txt" "$OUT/env.txt.orig" >/dev/null 2>&1; then HAD_ENV=1; else : > "$OUT/env.txt.orig"; fi
restore_env() {
    sh_ am force-stop "$PKG"
    if [ "$HAD_ENV" = 1 ]; then "$ADB" push "$OUT/env.txt.orig" "$F/env.txt" >/dev/null 2>&1 && say "env.txt del teléfono devuelto."
    else sh_ rm -f "$F/env.txt"; fi
}
trap restore_env EXIT
trap 'say ""; say "Cortado."; exit 130' INT TERM

INPUT=$(sed -n '/export PES6_INPUT=/,/EXTRA_INPUT/p' tools/ref_match.sh | tr -d '\\\n ' | sed 's/.*PES6_INPUT="//; s/\${EXTRA_INPUT}".*//')
case "$INPUT" in 400:cross,*8@711:cross:4) ;; *) die "no pude leer el guion de tools/ref_match.sh" ;; esac

# ---- 3. one scripted match per rate ---------------------------------------------------------------
run_rate() {
    local hz=$1 log=$OUT/pes6_$1.log
    step "Partido a $hz Hz (unos 4 min; deja el teléfono desbloqueado y con la pantalla encendida)"
    grep -v -E "^($(echo $KEYS | tr ' ' '|'))=" "$OUT/env.txt.orig" > "$OUT/env_$hz.txt"
    cat >> "$OUT/env_$hz.txt" <<EOF
PES6_INPUT=$INPUT
PES6_VERSION=original
PES6_RB=dirty,replay
PES6_RB_FROM=1200
PES6_RB_FRAMES=600
PES6_RB_K=8,32,64,64
PES6_RB_SKIPDRAW=2
PES6_SIM60=$([ "$hz" = 60 ] && echo 1 || echo 0)
PES6_SPIN=0
EOF
    "$ADB" push "$OUT/env_$hz.txt" "$F/env.txt" >/dev/null || die "no pude escribir env.txt en el teléfono"
    sh_ rm -f "$F/pes6.log"
    sh_ input keyevent KEYCODE_WAKEUP
    sh_ monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null
    local t0=$SECONDS last="" end=$((MAX_MIN * 60))
    while :; do
        sleep 10
        "$ADB" pull "$F/pes6.log" "$log" >/dev/null 2>&1 || : > "$log"
        if grep -q -E "\[rb\] (replay: done|restore refused)" "$log"; then break; fi
        if [ -z "$(sh_ pidof "$PKG")" ] && [ $((SECONDS - t0)) -gt 20 ]; then say "   la app se cerró sola"; break; fi
        if [ $((SECONDS - t0)) -ge "$end" ]; then say "   pasaron $MAX_MIN min sin terminar"; break; fi
        now=$(grep -o "scene [0-9]* frame [0-9]*" "$log" | tail -1)
        rb=$(grep -c "^\[rb\] K=.*CPU per vblank" "$log")
        grep -q "^\[rb\] dirty:" "$log" && st="midiendo el rebobinado ($rb de 4 ventanas)" || st="llegando al partido"
        grep -o -E "\[android\] (paused|resumed)" "$log" | tail -1 | grep -q paused \
            && st="EN PAUSA: desbloquea el teléfono y vuelve a la app"
        msg="   $((SECONDS - t0)) s: $st${now:+ ($now)}"
        [ "$msg" != "$last" ] && say "$msg"; last=$msg
    done
    sh_ am force-stop "$PKG"
    if ! grep -q "\[rb\] replay: done" "$log"; then
        "$ADB" exec-out screencap -p > "$OUT/pantalla_$hz.png" 2>/dev/null
        say "   No terminó la medición a $hz Hz. Última escena: $(grep -o 'scene [0-9]* frame [0-9]*' "$log" | tail -1)."
        grep -q "\[rb\] probe on" "$log" || say "   El log no tiene «[rb] probe on»: la app del teléfono no trae la sonda."
        say "   Log: $log; pantalla: $OUT/pantalla_$hz.png"
    fi
}
LOG30=- LOG60=-
for hz in $RATES; do
    case "$hz" in 30) run_rate 30; LOG30=$OUT/pes6_30.log ;; 60) run_rate 60; LOG60=$OUT/pes6_60.log ;; *) die "RATES: 30 y/o 60" ;; esac
done

# ---- 4. the numbers ------------------------------------------------------------------------------
step "Resultado"
python3 tools/rb_report.py "$LOG30" "$LOG60" | tee "$OUT/informe.txt"
say ""
say "Guardado en $OUT/ (informe.txt, pes6_30.log, pes6_60.log)."
