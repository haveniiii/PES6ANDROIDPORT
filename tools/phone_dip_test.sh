#!/usr/bin/env bash
# The isolated-dip test on the phone, end to end (AGENTS.md, 4/11): what the user runs at home, with
# the phone on wireless adb. Messages in Spanish: the user reads them.
#
#   tools/phone_dip_test.sh            everything: prepare, watch the match, report, restore env.txt
#   tools/phone_dip_test.sh prep       only: keep the last pes6.log, write the logging env.txt
#   tools/phone_dip_test.sh watch      only: follow the match (Enter, Ctrl-C or closing the app ends it)
#   tools/phone_dip_test.sh report     only: bring pes6.log, analyse it, restore env.txt
#   tools/phone_dip_test.sh restore    put back the env.txt kept by prep
#   tools/phone_dip_test.sh analyze <pes6.log> [thermal.tsv] [freq.txt]   the report of a log already on the Mac
#
# prep pulls pes6.log BEFORE the app is opened (every start overwrites it), so the log of the last
# session (maybe the one with the dip) is kept and analysed too. The app needs a restart to read
# env.txt: if it is open, prep closes it (after pulling its log).
# Environment: ANDROID_SERIAL (several devices), PKG (default: com.pes6port.iso if installed, else
# com.pes6port), ADB (the adb binary), OUT (the results' folder), KEEP_ENV=1 (report leaves the
# logging env.txt on the phone), MAX_MIN (watch gives up after this many minutes, default 90),
# NO_SPIN=1 (prep leaves PES6_SPIN out of the test's env.txt: the cores are not kept busy between
# frames, as on a phone that takes the performance hints; the original comes back with report).
# watch also samples, every second on the phone, each CPU cluster's frequency and the ceiling the
# thermal daemon allows (scaling_max_freq: Xiaomi caps it without Android's thermal status moving)
# and the CPU / GPU / skin temperatures, with the log's line count to place them (freq.txt).
# Results: build/phone_dip/<date>/ (build/phone_dip/latest points to the last one).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
MODE=${1:-all}
KEYS="PES6_SLOW_LOG PES6_COMMENTARY_LOG PES6_FACE_LOG"     # what prep adds to env.txt

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

if [ "$MODE" = analyze ]; then
    [ -n "${2:-}" ] || die "uso: tools/phone_dip_test.sh analyze <pes6.log> [thermal.tsv]"
    exec python3 "$ROOT/tools/dip_report.py" "$2" ${3:+--thermal "$3"} ${4:+--freq "$4"}
fi

# ---- the phone ---------------------------------------------------------------------------------
phone_help() {
    cat <<'EOF'

No hay ningún teléfono conectado por adb. En el teléfono (Redmi):
  1. Misma red WiFi que el Mac.
  2. Ajustes > Ajustes adicionales > Opciones de desarrollador > Depuración inalámbrica: activarla
     (si pregunta, "Permitir siempre en esta red").
  3. Si el Mac nunca se vinculó o dejó de reconocerlo: dentro de Depuración inalámbrica,
     "Vincular dispositivo con código de vinculación", y en el Mac:
         ~/Library/Android/sdk/platform-tools/adb pair IP:PUERTO_DE_VINCULACIÓN CÓDIGO
  4. Conectar con la IP y el puerto que muestra la pantalla de Depuración inalámbrica
     (no es el de vincular):
         ~/Library/Android/sdk/platform-tools/adb connect IP:PUERTO
  5. Volver a correr: tools/phone_dip_test.sh
Si adb dice "offline" o no lo ve: adb kill-server; adb start-server, y repetir el paso 4.
EOF
}

find_phone() {
    [ -x "$ADB" ] || die "no encuentro adb en $ADB (ADB=... para otro)"
    if [ -n "${ANDROID_SERIAL:-}" ]; then
        st=$("$ADB" -s "$ANDROID_SERIAL" get-state 2>/dev/null)
        [ "$st" = device ] || { say "ANDROID_SERIAL=$ANDROID_SERIAL no está listo (estado: ${st:-sin conexión})."; phone_help; exit 2; }
        return
    fi
    list() { "$ADB" devices 2>/dev/null | awk 'NR > 1 && NF >= 2 && $1 !~ /^emulator-/ {print $1, $2}'; }
    devs=$(list)
    if ! printf '%s\n' "$devs" | grep -q ' device$'; then
        # wireless debugging announces itself on the network: try to connect to what is there
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
    ready=$(printf '%s\n' "$devs" | awk '$2 == "device" {print $1}')
    n=$(printf '%s\n' "$ready" | grep -c .)
    if [ "$n" -eq 0 ]; then
        [ -n "$devs" ] && say "adb ve: $devs"
        phone_help
        exit 2
    fi
    if [ "$n" -gt 1 ]; then
        say "Hay $n dispositivos conectados:"
        for s in $ready; do say "   $s  $("$ADB" -s "$s" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"; done
        die "elige uno: ANDROID_SERIAL=<serie> tools/phone_dip_test.sh" 2
    fi
    export ANDROID_SERIAL=$ready
}

adb_() { "$ADB" "$@"; }
sh_()  { "$ADB" shell "$@" 2>/dev/null | tr -d '\r'; }

find_app() {
    if [ -z "${PKG:-}" ]; then
        pk=$(sh_ pm list packages com.pes6port)
        if printf '%s\n' "$pk" | grep -qx 'package:com.pes6port.iso'; then PKG=com.pes6port.iso
        elif printf '%s\n' "$pk" | grep -qx 'package:com.pes6port'; then PKG=com.pes6port
        else die "PES6 no está instalada en el teléfono (ni com.pes6port.iso ni com.pes6port)."
        fi
    fi
    F=/sdcard/Android/data/$PKG/files
    VER=$(sh_ dumpsys package "$PKG" | awk -F= '/versionName=/ {print $2; exit}')
    sh_ ls "$F" >/dev/null || die "no puedo leer $F en el teléfono"
}

running() { [ -n "$(sh_ pidof "$PKG")" ]; }

pick_out() {
    if [ -z "${OUT:-}" ]; then
        if [ "$MODE" = all ] || [ "$MODE" = prep ] || [ ! -e build/phone_dip/latest ]; then
            OUT=build/phone_dip/$(date +%Y%m%d-%H%M%S)
        else
            OUT=build/phone_dip/$(readlink build/phone_dip/latest)
        fi
    fi
    mkdir -p "$OUT"
    [ "$OUT" = "build/phone_dip/$(basename "$OUT")" ] && ln -sfn "$(basename "$OUT")" build/phone_dip/latest
}

thermal_sample() {    # status, battery C, hottest sensor C (sensors from the HAL, implausible values skipped)
    sh_ dumpsys thermalservice | awk '
        /^Thermal Status:/ { st = $3 }
        /^Current temperatures from HAL/ { hal = 1; next }
        hal && /Temperature\{/ {
            v = $0; sub(/.*mValue=/, "", v); sub(/,.*/, "", v); v += 0
            n = $0; sub(/.*mName=/, "", n); sub(/,.*/, "", n)
            if (n == "battery") bat = v
            if (v > 0 && v < 130 && v > mx) mx = v
        }
        hal && /^[A-Z]/ && !/Temperature/ { hal = 0 }
        END {
            if (st == "") st = "-"
            if (bat == "") bat = "-"
            if (mx == "") mx = "-"
            printf "%s\t%s\t%s\n", st, bat, mx
        }'
}

# ---- steps -------------------------------------------------------------------------------------
do_prep() {
    step "1/3 Preparar ($PKG ${VER:-?}, ${ANDROID_SERIAL})"
    if running; then
        say "La app está abierta: guardo su log y la cierro (tiene que arrancar de nuevo para leer env.txt)."
    fi
    if adb_ pull "$F/pes6.log" "$OUT/antes_pes6.log" >/dev/null 2>&1; then
        say "pes6.log de la sesión anterior guardado en $OUT/antes_pes6.log ($(sh_ stat -c %y "$F/pes6.log" | cut -c1-16))."
        python3 tools/dip_report.py "$OUT/antes_pes6.log" > "$OUT/antes_informe.txt" 2>&1
        say "   Su informe: $OUT/antes_informe.txt"
        grep -E '^Partido:|^  fps:|^Cuadros lentos|^Ventanas' "$OUT/antes_informe.txt" | sed 's/^/   /'
    else
        say "No había pes6.log en el teléfono (la app no se abrió nunca con este paquete)."
    fi
    running && adb_ shell am force-stop "$PKG"

    if [ -e "$OUT/env.txt.orig" ] || [ -e "$OUT/env.txt.none" ]; then
        say "env.txt original ya guardado en $OUT (no lo piso)."
    elif adb_ pull "$F/env.txt" "$OUT/env.txt.orig" >/dev/null 2>&1; then
        say "env.txt actual guardado en $OUT/env.txt.orig:"
        sed 's/^/   /' "$OUT/env.txt.orig"
    else
        : > "$OUT/env.txt.none"
        say "El teléfono no tenía env.txt."
    fi
    base=$OUT/env.txt.orig; [ -e "$base" ] || base=/dev/null
    {
        drop=$KEYS; [ "${NO_SPIN:-0}" = 1 ] && drop="$drop PES6_SPIN"
        grep -v -E "^($(echo $drop | tr ' ' '|'))=" "$base" | grep -v '^$'
        for k in $KEYS; do echo "$k=1"; done
    } > "$OUT/env.txt.test"
    adb_ push "$OUT/env.txt.test" "$F/env.txt" >/dev/null || die "no pude escribir env.txt en el teléfono"
    say "env.txt de la prueba en el teléfono:"
    sed 's/^/   /' "$OUT/env.txt.test"
}

user_steps() {
    cat <<EOF

== 2/3 En el teléfono
  1. Abre PES6 (desde el icono; si ya estaba abierta, el script la cerró).
  2. Elige lo mismo que la vez del bajón: Partido 60 fps, resolución ×2, calidad Normal.
  3. Juega un partido entero contra la CPU, como siempre. Si notas un bajón, fíjate en qué pasaba
     (repetición, primer plano, salida de los equipos, un gol, un cambio...) y más o menos cuándo.
  4. Al terminar el partido, vuelve al menú o cierra la app, como prefieras.
  5. Aquí: pulsa Enter (o Ctrl-C) cuando termines; si cierras la app, el script lo nota solo.
  No abras la app otra vez antes de que el script traiga el log: cada arranque lo borra.
EOF
}

# One line a second, run on the phone (no adb round trip per sample): uptime, pes6.log's line count,
# per cluster cur/ceiling/hardware max in kHz, the hottest CPU zone, the GPU zone and the skin sensor.
SAMPLER='i=0; while [ $i -lt 7200 ]; do
  l="t=$(cut -d" " -f1 /proc/uptime) n=$(wc -l < '"'"'LOG'"'"' 2>/dev/null || echo 0)"
  for c in /sys/devices/system/cpu/cpufreq/policy*; do
    l="$l ${c##*/}=$(cat $c/scaling_cur_freq)/$(cat $c/scaling_max_freq)/$(cat $c/cpuinfo_max_freq)"
  done
  cpu=0; gpu=; skin=
  for z in /sys/class/thermal/thermal_zone*; do
    read -r ty < $z/type 2>/dev/null || continue
    case "$ty" in
      cpu*|cpuss*) read -r v < $z/temp 2>/dev/null && [ "${v:-0}" -gt "$cpu" ] && cpu=$v ;;
      gpu) read -r gpu < $z/temp ;;
      quiet-therm-usr|skin*) read -r skin < $z/temp ;;
    esac
  done
  echo "$l cpu=$cpu gpu=${gpu:-0} skin=${skin:-0}"
  i=$((i + 1)); sleep 1
done'

do_watch() {
    step "Vigilando el partido (cada 15 s: fps del log; cada 1 s: frecuencias y temperaturas en freq.txt)"
    printf 'seconds\tfps_lines\tthermal_status\tbattery_c\tmax_c\n' > "$OUT/thermal.tsv"
    local t0=$SECONDS seen=0 stop=0 max_s=$(( ${MAX_MIN:-90} * 60 ))
    "$ADB" shell "${SAMPLER//LOG/$F/pes6.log}" > "$OUT/freq.txt" 2>/dev/null &
    local sampler=$!
    trap 'stop=1' INT
    while [ $stop -eq 0 ]; do
        if running; then
            seen=1
            adb_ pull "$F/pes6.log" "$OUT/pes6.log" >/dev/null 2>&1
        elif [ $seen -eq 1 ]; then
            say "La app se cerró: fin de la vigilancia."
            break
        fi
        n=0; last=""
        if [ -s "$OUT/pes6.log" ] && grep -q '^env: PES6_SLOW_LOG' "$OUT/pes6.log"; then
            n=$(grep -c '^\[fps\]' "$OUT/pes6.log")
            last=$(grep '^\[fps\]' "$OUT/pes6.log" | tail -1 | sed -E 's/^\[fps\] ([0-9.]+) +\(scene ([0-9]+) .* worst ([0-9]+) ms.*/\1 fps, escena \2, peor \3 ms/')
        fi
        th=$(thermal_sample)
        printf '%s\t%s\t%s\n' $(( SECONDS - t0 )) "$n" "$th" >> "$OUT/thermal.tsv"
        hot=$(printf '%s' "$th" | cut -f3)
        if [ $seen -eq 0 ]; then
            printf '  %s  esperando a que abras la app...\n' "$(date +%H:%M:%S)"
        else
            printf '  %s  %s  (%s °C)\n' "$(date +%H:%M:%S)" "${last:-arrancando}" "$hot"
        fi
        [ $(( SECONDS - t0 )) -ge $max_s ] && { say "Llevo ${MAX_MIN:-90} min: paro de vigilar."; break; }
        if [ -t 0 ]; then
            read -r -t 15 _ && break
        else
            sleep 15
        fi
    done
    trap - INT
    kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
    say "Muestras por segundo: $(grep -c '^t=' "$OUT/freq.txt" 2>/dev/null || echo 0) en $OUT/freq.txt"
}

do_report() {
    step "3/3 Traer el log y analizarlo"
    adb_ pull "$F/pes6.log" "$OUT/pes6.log" >/dev/null 2>&1 || die "no pude traer $F/pes6.log"
    if ! grep -q '^env: PES6_SLOW_LOG' "$OUT/pes6.log"; then
        say "AVISO: el pes6.log del teléfono no viene de una sesión con el env.txt de la prueba"
        say "       (¿no se abrió la app después de prep?). Lo analizo igual."
    fi
    th=; [ -s "$OUT/thermal.tsv" ] && th=$OUT/thermal.tsv
    fq=; [ -s "$OUT/freq.txt" ] && fq=$OUT/freq.txt
    python3 tools/dip_report.py "$OUT/pes6.log" ${th:+--thermal "$th"} ${fq:+--freq "$fq"} | tee "$OUT/informe.txt"
    say ""
    say "Guardado en $OUT/: pes6.log, informe.txt${th:+, thermal.tsv}, antes_pes6.log (sesión anterior)."
    if [ "${KEEP_ENV:-0}" = 1 ]; then
        say "KEEP_ENV=1: el env.txt de la prueba se queda en el teléfono (tools/phone_dip_test.sh restore lo devuelve)."
    else
        do_restore
    fi
}

do_restore() {
    if [ -e "$OUT/env.txt.orig" ]; then
        adb_ push "$OUT/env.txt.orig" "$F/env.txt" >/dev/null && say "env.txt original devuelto al teléfono (vale desde el próximo arranque)."
    elif [ -e "$OUT/env.txt.none" ]; then
        adb_ shell rm -f "$F/env.txt" && say "env.txt de la prueba borrado (el teléfono no tenía uno)."
    else
        say "No hay env.txt guardado en $OUT: no toco el del teléfono."
    fi
}

case "$MODE" in
    all|prep|watch|report|restore) ;;
    *) sed -n '2,20p' "$0"; exit 1 ;;
esac
find_phone
find_app
pick_out
case "$MODE" in
    all)     do_prep; user_steps; do_watch; do_report ;;
    prep)    do_prep; user_steps; say ""; say "Después: tools/phone_dip_test.sh watch  (o directamente report al terminar)." ;;
    watch)   do_watch; say "Ahora: tools/phone_dip_test.sh report" ;;
    report)  do_report ;;
    restore) do_restore ;;
esac
