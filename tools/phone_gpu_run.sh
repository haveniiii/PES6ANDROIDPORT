#!/bin/bash
# One scripted match on the phone at a given resolution and PES6_GLX mask (docs/32 §10).
#   ANDROID_SERIAL=... tools/phone_gpu_run.sh <res 0=auto|1|2|3> <glx mask|default> <finish|play> [last scene-8 frame]
# finish: PES6_GPU_FINISH, the GPU's real time per frame ([gpu] lines).
# play:   no waiting for the GPU, the frame rate a player gets ([fps] lines).
# EXTRA_ENV: more env.txt lines, separated by \\n. Prints the match's lines of pes6.log. The caller keeps and restores env.txt and settings.txt.
RES=$1; GLX=$2; MODE=$3; LAST=${4:-2400}
PKG=${PKG:-com.pes6port}
F=/sdcard/Android/data/$PKG/files
ADB=~/Library/Android/sdk/platform-tools/adb
INPUT="400:cross,3@200:cross:10,3@330:cross:10,4@631:cross:4,4@1031:cross:4,4@1431:cross:4,4@1731:right:4,4@1931:down:4,4@2131:cross:4,4@2431:cross:4,4@2631:down:4,4@2781:right:4,4@2931:cross:4,4@3131:right:4,4@3281:cross:4,4@3631:cross:4,4@4031:cross:4,5@300:cross:4,5@700:cross:4,8@411:cross:4,8@711:cross:4"
T=$(mktemp -d)
printf 'move=0\nrim_run=1\nhand=0\ngpu=1\ncommentary=1\ncontrols=2\nres=%s\nquality=1\nversion=pech2026\n' "$RES" > "$T/settings.txt"
{
    echo "PES6_INPUT=$INPUT"
    [ "$GLX" = default ] || echo "PES6_GLX=$GLX"
    echo "PES6_FPS_LOG=1"
    [ "$MODE" = finish ] && echo "PES6_GPU_FINISH=1"
    [ -n "$EXTRA_ENV" ] && printf '%b\n' "$EXTRA_ENV"
} > "$T/env.txt"
$ADB shell am force-stop $PKG
$ADB push "$T/settings.txt" $F/settings.txt >/dev/null
$ADB push "$T/env.txt" $F/env.txt >/dev/null
$ADB shell input keyevent KEYCODE_WAKEUP
$ADB shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for i in $(seq 1 120); do
    sleep 5
    $ADB pull $F/pes6.log "$T/pes6.log" >/dev/null 2>&1
    f=$(grep -E '^\[fps\].*scene 8 ' "$T/pes6.log" 2>/dev/null | tail -1 | sed -E 's/.*frame ([0-9]+).*/\1/')
    [ -n "$f" ] && [ "$f" -ge "$LAST" ] && break
done
$ADB shell am force-stop $PKG
grep -E 'ge_gl: fill|internal resolution|^gpu:' "$T/pes6.log" | head -4
awk '/^\[fps\].*scene 8 /{print} /^\[gpu\]/{print} /^\[slow\]/{print}' "$T/pes6.log"
$ADB shell dumpsys thermalservice 2>/dev/null | grep -m1 -E 'Temperature\{mValue=.*mName=(battery|xo|cpu)' | cut -c1-70
rm -rf "$T"
