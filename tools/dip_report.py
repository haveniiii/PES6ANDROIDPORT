#!/usr/bin/env python3
"""Where a phone match lost its frames, from the app's pes6.log (tools/phone_dip_test.sh).

    tools/dip_report.py pes6.log [--thermal thermal.tsv] [--threshold-fps 55]

Reads, in file order (the log is written line by line, so the order is the time order):
  [fps] F (scene S frame N) worst W ms, H over 50 ms; GE thread G%, game waits for it X ms/frame
        every ~5 s, always on in the Android build;
  [slow] T ms: game a, waiting for the GE b, present cpu c, present blocked d, other e (scene S frame N)
        each game frame over 40 ms, with PES6_SLOW_LOG=1;
  [commentary] ...  with PES6_COMMENTARY_LOG=1: goals, whistles, kick-off, plays -- the context
        (a goal is followed by close-ups and the replay until "restart after the goal"; a whistle
        by the cut-scenes of the stoppage until play goes on);
  [faces] chara i: id name  with PES6_FACE_LOG=1: a player drawn for the first time, the moment his
        face file is read (faces.c, on the game thread).
thermal.tsv (written by the script while the match runs): seconds, [fps] lines in the log so far,
thermal status, battery and hottest sensor in C -- to put a temperature on
each 5 s window.
Prints the report in Spanish (the user reads it).
"""
import argparse
import re
import statistics
import sys

MATCH_SCENES = {8, 14, 15}          # match, training, WIRELESS (15 includes its lobby menus)
FPS_RE = re.compile(r"^\[fps\] ([\d.]+)\s+\(scene (\d+) frame (\d+)\) worst (\d+) ms, (\d+) over 50 ms"
                    r"(?:; GE thread (\d+)%, game waits for it ([\d.]+) ms/frame)?")
SLOW_RE = re.compile(r"^\[slow\] ([\d.]+) ms: game ([\d.-]+), waiting for the GE ([\d.-]+), present cpu ([\d.-]+), "
                     r"present blocked ([\d.-]+), other ([\d.-]+) \(scene (\d+) frame (\d+)\)")
FACE_RE = re.compile(r"^\[faces\] chara (\d+): (\d+) (.*?)( \(no face\))?$")
COMM_EVENT_RE = re.compile(r"^\[commentary\] (?:v\d+ )?(?:f\d+ )?([a-z_]+)(?: -> clip| skipped| too soon| \(no clips\))")
PLAY = {"pass", "dribble", "tackle", "shot", "shot_miss", "shot_near", "shot_chance", "shot_wild", "cross",
        "header", "through_pass", "back_pass", "save", "woodwork"}
GOAL = {"goal", "own_goal", "equalizer"}
WINDOW_S = 5.0                      # an [fps] line every ~5 s

PART = {"game": "hilo del juego", "ge": "esperando al hilo de dibujo (GE)",
        "present": "presentar el cuadro", "other": "el sistema (el hilo no corría)"}
PART_WHY = {
    "game": "la lógica del juego o una carga hecha en su hilo (caras al aparecer un jugador, lecturas del disco)",
    "ge": "el hilo de dibujo tardó: texturas nuevas que leer, decodificar o subir a la GPU",
    "present": "la GPU iba atrasada al mostrar el cuadro",
    "other": "Android no dejó correr al hilo: planificador, núcleos a baja frecuencia o calor",
}
TAG = {"goal": "gol: celebración y repetición", "stop": "parada: cortes y primeros planos",
       "pre": "antes del saque inicial (salida de los equipos)", "faces": "caras nuevas"}


def mmss(s):
    s = max(0, int(round(s)))
    return f"{s // 60}:{s % 60:02d}"


def load_thermal(path):
    rows = []
    if not path:
        return rows
    try:
        with open(path) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) < 5 or not p[1].isdigit():
                    continue
                num = lambda x: float(x) if x not in ("", "-") else None
                rows.append({"t": float(p[0]), "n": int(p[1]), "status": num(p[2]), "battery": num(p[3]), "max": num(p[4])})
    except OSError:
        pass
    return rows


def load_freq(path, fps):
    """freq.txt (phone_dip_test.sh watch): one sample a second, placed in the [fps] window being filled
    when it was taken (the log's line count against the [fps] lines' line numbers)."""
    rows = []
    if not path:
        return rows
    import bisect
    lines = [r["line"] for r in fps]
    try:
        with open(path, errors="replace") as f:
            for raw in f:
                kv = dict(p.split("=", 1) for p in raw.split() if "=" in p)
                if "t" not in kv or "n" not in kv:
                    continue
                try:
                    r = {"t": float(kv["t"]), "n": int(kv["n"] or 0), "cpu": int(kv.get("cpu", 0)) / 1000,
                         "gpu": int(kv.get("gpu", 0) or 0) / 1000, "skin": int(kv.get("skin", 0) or 0) / 1000, "pol": {}}
                    for k, v in kv.items():
                        if k.startswith("policy"):
                            cur, cap, hw = (int(x) for x in v.split("/"))
                            r["pol"][k] = (cur, cap, hw)
                except ValueError:
                    continue
                r["win"] = bisect.bisect_right(lines, r["n"]) + 1    # the window this sample falls in
                rows.append(r)
    except OSError:
        pass
    return rows


def parse(path):
    fps, slow, faces_all, notes, info = [], [], [], [], []
    state = {"goal": False, "stop": False, "kicked": False}
    faces_since = []                 # first-drawn players since the last [fps]/[slow] line
    window_tags = set()
    match_started = False
    n_fps = 0
    with open(path, errors="replace") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            m = FPS_RE.match(line)
            if m:
                n_fps += 1
                scene = int(m.group(2))
                rec = {"i": n_fps, "line": lineno, "fps": float(m.group(1)), "scene": scene, "frame": int(m.group(3)),
                       "worst": int(m.group(4)), "hitches": int(m.group(5)),
                       "ge": float(m.group(6)) if m.group(6) else None,
                       "wait": float(m.group(7)) if m.group(7) else None,
                       "tags": set(window_tags), "faces": len(faces_since)}
                if state["goal"]: rec["tags"].add("goal")
                if state["stop"]: rec["tags"].add("stop")
                if scene in MATCH_SCENES and not state["kicked"]: rec["tags"].add("pre")
                if faces_since: rec["tags"].add("faces")
                fps.append(rec)
                window_tags.clear()
                faces_since = []
                continue
            m = SLOW_RE.match(line)
            if m:
                v = [float(x) for x in m.group(1, 2, 3, 4, 5, 6)]
                parts = {"game": v[1], "ge": v[2], "present": v[3] + v[4], "other": v[5]}
                rec = {"ms": v[0], "parts": parts, "scene": int(m.group(7)), "frame": int(m.group(8)),
                       "after_fps": n_fps, "tags": set(), "faces": list(faces_since)}
                if state["goal"]: rec["tags"].add("goal")
                if state["stop"]: rec["tags"].add("stop")
                if rec["scene"] in MATCH_SCENES and not state["kicked"]: rec["tags"].add("pre")
                if faces_since: rec["tags"].add("faces")
                slow.append(rec)
                faces_since = []
                continue
            m = FACE_RE.match(line)
            if m:
                who = (m.group(3) or "").strip() or m.group(2)
                faces_since.append(who + (" (sin cara propia)" if m.group(4) else ""))
                faces_all.append((n_fps, who))
                window_tags.add("faces")
                continue
            if line.startswith("[commentary]"):
                if "restart after the goal" in line:
                    state["goal"] = False
                    continue
                if "whistle cause" in line:
                    if state["kicked"]:          # before the kick-off it is the kick-off's own whistle
                        state["stop"] = True
                        window_tags.add("stop")
                    continue
                if ": quiet" in line:
                    notes.append((n_fps, "pausa o final del partido" if "paused" in line or "match over" in line else line))
                    continue
                e = COMM_EVENT_RE.match(line)
                if not e:
                    continue
                ev = e.group(1)
                if ev in GOAL:
                    state["goal"] = True
                    state["stop"] = False
                    window_tags.add("goal")
                    notes.append((n_fps, "gol"))
                elif ev == "kickoff":
                    state["kicked"] = True
                    state["stop"] = False
                elif ev in PLAY:
                    state["kicked"] = True
                    state["stop"] = False
                elif ev in ("half_time", "full_time"):
                    notes.append((n_fps, "descanso" if ev == "half_time" else "final"))
                continue
            if line.startswith("[hint]") or line.startswith("[rules] match at 60") or "internal resolution" in line \
                    or line.startswith("gpu:") or line.startswith("[ge] ") and "replacement" in line \
                    or line.startswith("settings:") or line.startswith("env: "):
                info.append(line)
    if "env: PES6_COMMENTARY_LOG" not in info:       # no kick-off seen because none is logged
        for r in fps + slow: r["tags"].discard("pre")
    for r in slow:                                   # a negative time printed unsigned (2^64 - x)
        for k, v in r["parts"].items():
            if v > 1e9: r["parts"][k] = 0.0
    return fps, slow, faces_all, notes, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--thermal")
    ap.add_argument("--freq", help="freq.txt: CPU frequencies, their thermal ceiling and temperatures each second")
    ap.add_argument("--threshold-fps", type=float, help="a window under this is a dip (default 55 at 60 Hz, 27.5 at 30)")
    a = ap.parse_args()
    try:
        fps, slow, faces_all, notes, info = parse(a.log)
    except OSError as e:
        print(f"No pude leer {a.log}: {e}")
        return 1
    thermal = load_thermal(a.thermal)
    freq = load_freq(a.freq, fps)
    out = print

    out(f"Informe de {a.log}")
    envs = [l[5:] for l in info if l.startswith("env: ")]
    out("  env.txt: " + (", ".join(envs) if envs else "(vacío)"))
    for l in info:
        if not l.startswith("env: "):
            out("  " + l[:150])
    if "PES6_SLOW_LOG" not in envs:
        out("  AVISO: esta sesión no tenía PES6_SLOW_LOG=1: solo hay líneas [fps] (cada ~5 s), sin el detalle de cada cuadro lento.")
    if "PES6_COMMENTARY_LOG" not in envs:
        out("  AVISO: sin PES6_COMMENTARY_LOG=1 no se sabe cuándo hubo goles, paradas o repeticiones.")

    at60 = any(l.startswith("[rules] match at 60") for l in info)
    if a.threshold_fps is None:
        a.threshold_fps = 55.0 if at60 else 27.5
    worst_lim = 50 if at60 else 75                  # at 30 Hz a frame is 33 ms and 50+ is common
    out(f"  partido a {'60' if at60 else '30'} Hz: bajón = ventana bajo {a.threshold_fps:g} fps o un cuadro de más de {worst_lim} ms")
    mfps = [r for r in fps if r["scene"] in MATCH_SCENES]
    if not mfps:
        out("\nNo hay ninguna línea [fps] de partido (escena 8, 14 o 15): ¿se llegó a jugar? Nada que analizar.")
        return 0
    first_i = mfps[0]["i"]
    t_of = lambda i: (i - first_i) * WINDOW_S          # seconds since the first match window
    temp_of = {}
    for r in fps:
        cand = [t for t in thermal if t["n"] >= r["i"]]
        if cand:
            temp_of[r["i"]] = cand[0]
    vals = [r["fps"] for r in mfps]
    out(f"\nPartido: {len(mfps)} ventanas de ~5 s (≈ {mmss(len(mfps) * WINDOW_S)} de juego, contando pausas y repeticiones).")
    out(f"  fps: mediana {statistics.median(vals):.1f}, mínimo {min(vals):.1f}; "
        f"ventanas bajo {a.threshold_fps:g} fps: {sum(v < a.threshold_fps for v in vals)}; "
        f"cuadros de más de 50 ms: {sum(r['hitches'] for r in mfps)}; peor cuadro: {max(r['worst'] for r in mfps)} ms.")
    ge = [r["ge"] for r in mfps if r["ge"] is not None]
    if ge:
        out(f"  hilo de dibujo (GE): mediana {statistics.median(ge):.0f} % de un núcleo, máximo {max(ge):.0f} %.")
    if faces_all:
        in_match = [f for f in faces_all if f[0] >= first_i - 1]
        out(f"  caras leídas por primera vez: {len(faces_all)} en total, {len(in_match)} ya en el partido.")
    goals = [mmss(t_of(i)) for i, w in notes if w == "gol" and i >= first_i - 1]
    if goals:
        out("  goles hacia el minuto (reloj real desde el inicio): " + ", ".join(goals))

    # ---- the bad windows
    bad = [r for r in mfps if r["fps"] < a.threshold_fps or r["worst"] > worst_lim]
    out(f"\nVentanas de ~5 s con bajón: {len(bad)}")
    for r in sorted(bad, key=lambda r: (-r["worst"], r["fps"]))[:20]:
        tags = [TAG[t] for t in ("goal", "stop", "pre", "faces") if t in r["tags"]]
        star = "★ " if tags else "  "
        th = temp_of.get(r["i"])
        temp = f", {th['max']:.0f} °C máx." if th and th["max"] is not None else ""
        out(f"  {star}≈{mmss(t_of(r['i']))}  {r['fps']:.1f} fps, peor {r['worst']} ms, {r['hitches']} sobre 50 ms"
            f"{', GE %.0f %%' % r['ge'] if r['ge'] is not None else ''}{temp}"
            f"{'  — ' + '; '.join(tags) if tags else ''}"
            f"{' (%d)' % r['faces'] if r['faces'] else ''}")

    # ---- each slow frame
    mslow = [s for s in slow if s["scene"] in MATCH_SCENES]
    other_slow = len(slow) - len(mslow)
    out(f"\nCuadros lentos (más de 40 ms) en el partido: {len(mslow)}"
        + (f"  (y {other_slow} fuera del partido: menús y cargas, esperables)" if other_slow else ""))
    if not slow and "PES6_SLOW_LOG" in envs:
        out("  Ninguno: con PES6_SLOW_LOG=1 no hubo ni un cuadro de más de 40 ms.")
    by_part, by_tag = {}, {}
    for s in mslow:
        s["main"] = max(s["parts"], key=lambda k: s["parts"][k])
        by_part[s["main"]] = by_part.get(s["main"], 0) + 1
        key = "+".join(t for t in ("goal", "stop", "pre", "faces") if t in s["tags"]) or "none"
        by_tag[key] = by_tag.get(key, 0) + 1
    for s in sorted(mslow, key=lambda s: -s["ms"])[:25]:
        p = s["parts"]
        tags = [TAG[t] for t in ("goal", "stop", "pre") if t in s["tags"]]
        if s["faces"]:
            tags.append("justo después de leer %d cara(s): %s" % (len(s["faces"]), ", ".join(s["faces"][:4])
                                                                  + (" …" if len(s["faces"]) > 4 else "")))
        star = "★ " if tags else "  "
        th = temp_of.get(s["after_fps"] + 1)
        temp = f", {th['max']:.0f} °C" if th and th["max"] is not None else ""
        out(f"  {star}≈{mmss(t_of(s['after_fps'] + 0.5))} cuadro {s['frame']}: {s['ms']:.0f} ms = juego {p['game']:.0f}, "
            f"GE {p['ge']:.0f}, presentar {p['present']:.0f}, sistema {p['other']:.0f} → {PART[s['main']]}{temp}"
            + ("\n        " + "; ".join(tags) if tags else ""))
    if len(mslow) > 25:
        out(f"  … y {len(mslow) - 25} más (los 25 peores arriba).")

    # ---- temperature
    out("\nTemperatura:")
    tm = [t for t in thermal if t["max"] is not None]
    if not tm:
        out("  sin muestras (solo las toma el script mientras vigila el partido).")
    else:
        st = [t["status"] for t in thermal if t["status"] is not None]
        bat = [t["battery"] for t in thermal if t["battery"] is not None]
        out(f"  sensor más caliente: {tm[0]['max']:.0f} → {tm[-1]['max']:.0f} °C (máx. {max(t['max'] for t in tm):.0f})"
            + (f"; batería {bat[0]:.0f} → {bat[-1]:.0f} °C" if bat else "")
            + (f"; estado térmico de Android máx. {max(st):.0f} (0 = normal, 1 = leve, 2+ = limitando)" if st else ""))
        pairs = [(temp_of[r["i"]]["max"], r["fps"]) for r in mfps if r["i"] in temp_of and temp_of[r["i"]]["max"] is not None]
        if len(pairs) >= 6:
            pairs.sort()
            k = len(pairs) // 3
            cool = statistics.mean(f for _, f in pairs[:k])
            hot = statistics.mean(f for _, f in pairs[-k:])
            out(f"  fps medios en el tercio más frío {cool:.1f}, en el más caliente {hot:.1f}.")

    # ---- CPU frequency and its thermal ceiling, each second
    capped_bad = []
    if freq:
        out("\nCPU por segundo (freq.txt: frecuencia, techo térmico, temperaturas):")
        pols = sorted({k for r in freq for k in r["pol"]}, key=lambda k: -max(r["pol"][k][2] for r in freq if k in r["pol"]))
        big = pols[0]
        hw = max(r["pol"][big][2] for r in freq if big in r["pol"])
        caps = [r for r in freq if r["win"] >= first_i and any(c < h for _, c, h in r["pol"].values())]
        if caps:
            low = min(r["pol"][big][1] for r in caps if big in r["pol"])
            wins = sorted({r["win"] for r in caps})
            out(f"  el techo de frecuencia bajó en {len(caps)} muestra(s) del partido (núcleos grandes hasta {low / 1e6:.2f} de "
                f"{hw / 1e6:.2f} GHz), en ≈" + ", ".join(mmss(t_of(w)) for w in wins[:12]) + (" …" if len(wins) > 12 else ""))
        else:
            out(f"  el techo de frecuencia nunca bajó en el partido (núcleos grandes a {hw / 1e6:.2f} GHz como máximo): sin recorte por calor.")
        bywin = {}
        for r in freq:
            bywin.setdefault(r["win"], []).append(r)
        show = sorted({w for b in bad for w in (b["i"] - 1, b["i"], b["i"] + 1)} & set(bywin))
        if show:
            out("  alrededor de cada bajón (ventana: fps | núcleos grandes: media, techo mínimo | CPU, GPU, piel °C):")
        fps_by_i = {r["i"]: r for r in fps}
        for w in show:
            rs = bywin[w]
            cur = statistics.mean(r["pol"][big][0] for r in rs if big in r["pol"]) / 1e6
            cap = min(r["pol"][big][1] for r in rs if big in r["pol"])
            mark = "  ← techo bajado" if cap < hw else ""
            isbad = any(b["i"] == w for b in bad)
            if isbad and cap < hw:
                capped_bad.append(w)
            f = fps_by_i.get(w)
            out(f"   {'*' if isbad else ' '} ≈{mmss(t_of(w))}: {f['fps'] if f else 0:5.1f} fps | {cur:.2f} GHz, techo {cap / 1e6:.2f} | "
                f"{max(r['cpu'] for r in rs):.0f}, {max(r['gpu'] for r in rs):.0f}, {max(r['skin'] for r in rs):.0f}{mark}")

    # ---- verdict
    out("\nConclusión:")
    if not mslow and not bad:
        out("  El partido no tuvo bajones medibles.")
        return 0
    if not mslow:
        star = sum(1 for r in bad if r["tags"])
        out(f"  {len(bad)} ventana(s) con bajón, {star} con gol/repetición, parada, salida de los equipos o caras nuevas (★);"
            " sin líneas [slow] no se sabe en qué hilo se fue el tiempo.")
    if mslow:
        main_part = max(by_part, key=by_part.get)
        out(f"  De {len(mslow)} cuadros lentos, {by_part[main_part]} se fueron sobre todo en: {PART[main_part]} "
            f"({PART_WHY[main_part]}).")
        tagged = sum(n for k, n in by_tag.items() if k != "none")
        out(f"  {tagged} de {len(mslow)} coinciden con un gol/repetición, una parada, la salida de los equipos o caras nuevas (★).")
        for k, n in sorted(by_tag.items(), key=lambda kv: -kv[1]):
            out(f"    {n:3d}  " + (" + ".join(TAG[t] for t in k.split("+")) if k != "none" else "sin nada de eso"))
        faces_hits = sum(1 for s in mslow if s["faces"])
        if faces_hits:
            out(f"  {faces_hits} cuadro(s) lentos vinieron justo después de leer caras: apunta a la carga de caras.")
        early = [s for s in mslow if t_of(s["after_fps"] + 0.5) < 60]
        if early:
            out(f"  {len(early)} en el primer minuto del partido (arranque en frío: lo que se carga la primera vez).")
    if freq:
        if capped_bad:
            out(f"  En {len(capped_bad)} ventana(s) con bajón el control térmico había bajado el techo de la CPU: ahí fue el calor.")
        elif bad:
            out("  Ningún bajón coincide con un recorte de la frecuencia de la CPU: no fue el calor de la CPU "
                "(la GPU no se puede leer sin root).")
    if tm:
        st = [t["status"] for t in thermal if t["status"] is not None]
        if st and max(st) >= 2:
            out("  Android llegó a limitar por temperatura (estado ≥ 2): mirar si los bajones coinciden con eso.")
        else:
            out("  Android no llegó a limitar por temperatura durante el partido.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
