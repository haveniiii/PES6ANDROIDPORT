#!/usr/bin/env python3
"""The three numbers of the rollback study from PES6_RB=dirty,replay logs (docs/27 §9, §12).

  tools/rb_report.py <pes6_30.log> <pes6_60.log>     either may be missing ("-")
  tools/rb_report.py --selftest

Per rate: cost of a simulation step (normal, and re-simulated with PES6_RB_SKIPDRAW=2), cost of saving
(dirty pages, measured) and restoring (the pages of 16 vblanks at the bandwidth of the measured 32 MB
restore), and the rollback frames that fit: (frame - normal - save - restore - 2 ms) / re-simulated step.
Messages in Spanish: the user reads them."""
import re
import sys

MARGIN_MS = 2.0


def parse(text):
    r = {"hz": None, "copy_us": None, "copy_p99": None, "union16_kb": None, "win": [], "restore_us": [],
         "identical": 0, "differs": 0, "refused": "restore refused" in text, "done": "replay: done" in text}
    m = re.search(r"\[rb\] dirty: \d+ vblanks .* sim at (\d+) Hz", text)
    if m:
        r["hz"] = int(m.group(1))
    m = re.search(r"\[rb\] copy dirty pages\s+mean\s+([\d.]+)\s+p50\s+[\d.]+\s+p90\s+[\d.]+\s+p99\s+([\d.]+)", text)
    if m:
        r["copy_us"], r["copy_p99"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"\[rb\] union 16 vblanks \(KB\)\s+mean\s+([\d.]+)", text)
    if m:
        r["union16_kb"] = float(m.group(1))
    for k, a, b in re.findall(r"\[rb\] K=(\d+): CPU per vblank (\d+) us normal, (\d+) us skipping the draw", text):
        r["win"].append((int(k), float(a), float(b)))
    # the RAM differs in the drawing state when the draw was skipped; what counts is the match itself
    for us, rest in re.findall(r"\[rb\] K=\d+: restore (\d+) us; (.*)", text):
        r["restore_us"].append(float(us))
        r["identical"] += rest.startswith("identical")
    for same in re.findall(r"\[rb\] K=\d+: match state [^:]*: (identical|DIFFERS)", text):
        r["identical" if same == "identical" else "differs"] += 1
    return r


def numbers(r, hz):
    """None, or the figures for one rate; vps = vblanks per simulation step."""
    if not r["win"] or r["copy_us"] is None or not r["restore_us"] or r["union16_kb"] is None:
        return None
    vps = 60 // hz
    normal = sum(w[1] for w in r["win"]) / len(r["win"]) * vps / 1000
    resim = sum(w[2] for w in r["win"]) / len(r["win"]) * vps / 1000
    resim_worst = max(w[2] for w in r["win"]) * vps / 1000
    full = sorted(r["restore_us"])[len(r["restore_us"]) // 2] / 1000      # 32 MB, ms
    gb_s = 32 * 1024 * 1024 / (full / 1000) / 1e9
    save = r["copy_us"] * vps / 1000
    restore = r["union16_kb"] * 1024 / (gb_s * 1e9) * 1000
    budget = 1000 / hz - normal - save - restore - MARGIN_MS
    fit = max(0, int(budget // resim)) if resim > 0 else 0
    fit_worst = max(0, int(budget // resim_worst)) if resim_worst > 0 else 0
    return dict(normal=normal, resim=resim, resim_worst=resim_worst, ratio=resim / normal if normal else 0,
                save=save, save_p99=r["copy_p99"] * vps / 1000, restore=restore, full=full, gb_s=gb_s,
                budget=budget, fit=fit, fit_worst=fit_worst, ms=fit * 1000 / hz)


def report(logs):
    out, res = [], {}
    for hz, path in logs:
        if path in (None, "-"):
            out.append(f"{hz} Hz: sin log")
            continue
        try:
            r = parse(open(path, errors="replace").read())
        except OSError as e:
            out.append(f"{hz} Hz: no puedo leer {path}: {e}")
            continue
        if r["hz"] is not None and r["hz"] != hz:
            out.append(f"{hz} Hz: OJO, el log dice que el partido iba a {r['hz']} Hz")
        n = numbers(r, hz)
        if r["refused"]:
            out.append(f"{hz} Hz: la sonda no pudo reponer el estado (cambiaron los hilos)")
        if n is None:
            out.append(f"{hz} Hz: el log no tiene las mediciones (¿llegó al partido? ¿terminó la sonda?)")
            continue
        res[hz] = n
        out.append(f"{hz} Hz: re-simulación {'idéntica' if not r['differs'] else 'DISTINTA'} en "
                   f"{r['identical']} de {r['identical'] + r['differs']} ventanas")
    if not res:
        return "\n".join(out), res
    out.append("")
    out.append("                                    " + "".join(f"{h:>12} Hz" for h in sorted(res)))
    rows = [("paso normal (hilo del juego)", "normal", "ms"), ("paso re-simulado sin dibujar", "resim", "ms"),
            ("  la peor ventana", "resim_worst", "ms"), ("  re-simulado / normal", "ratio", "x"),
            ("guardar lo cambiado (media)", "save", "ms"), ("  p99", "save_p99", "ms"),
            ("reponer 16 cuadros de cambios", "restore", "ms"), ("  (reponer los 32 MB, medido)", "full", "ms"),
            ("  copia de memoria", "gb_s", "GB/s"), ("cabe para re-simular", "budget", "ms")]
    for label, key, unit in rows:
        out.append(f"{label:<36}" + "".join(f"{res[h][key]:>12.2f} {unit:<2}" for h in sorted(res)))
    out.append("")
    out.append("=" * 68)
    out.append("RESULTADO (el entregable)")
    h0 = sorted(res)
    out.append("1. Un paso de simulación cuesta: " + ", ".join(
        f"{res[h]['normal']:.1f} ms normal y {res[h]['resim']:.1f} ms re-simulado a {h} Hz" for h in h0))
    out.append("2. Guardar y reponer el estado: " + ", ".join(
        f"{res[h]['save']:.2f} + {res[h]['restore']:.2f} ms a {h} Hz" for h in h0))
    out.append("3. Rollback que aguanta: " + ", ".join(
        f"{res[h]['fit']} {'cuadros' if h == 60 else 'pasos'} (~{res[h]['ms']:.0f} ms; "
        f"{res[h]['fit_worst']} con la peor ventana) a {h} Hz" for h in h0))
    out.append("=" * 68)
    out.append(f"Fórmula (docs/27 §9): (cuadro − normal − guardar − reponer − {MARGIN_MS:.0f} ms) / paso re-simulado.")
    return "\n".join(out), res


def selftest():
    log = """[rb] dirty: 600 vblanks of match from 8@1200, sim at 60 Hz
[rb] copy dirty pages            mean    100.0  p50     90.0  p90    150.0  p99    300.0  max    400.0 us
[rb] union 16 vblanks (KB)       mean    512.0  p50    500.0  p90    600.0  p99    700.0  max    800.0 KB
[rb] K=8: CPU per vblank 7000 us normal, 4000 us skipping the draw
[rb] K=8: restore 10000 us; identical every vblank
[rb] K=32: CPU per vblank 7000 us normal, 3000 us skipping the draw
[rb] K=32: restore 10000 us; first difference after 1 vblank(s); end state:
[rb] K=32: match state after the second pass: identical
[rb] replay: done
"""
    r = parse(log)
    assert r["hz"] == 60 and r["identical"] == 2 and not r["differs"] and r["done"]
    n = numbers(r, 60)
    assert abs(n["normal"] - 7.0) < 1e-9 and abs(n["resim"] - 3.5) < 1e-9 and abs(n["resim_worst"] - 4.0) < 1e-9
    assert abs(n["gb_s"] - 3.3554432) < 1e-6
    # 16.667 - 7 - 0.1 - 0.156 - 2 = 7.41 ms -> 2 steps of 3.5, 1 of 4
    assert n["fit"] == 2 and n["fit_worst"] == 1, n
    n30 = numbers(r, 30)                      # same per-vblank figures, 2 vblanks per step
    assert abs(n30["normal"] - 14.0) < 1e-9 and n30["fit"] == 2, n30
    assert numbers(parse("nothing"), 60) is None
    text, res = report([(30, "-"), (60, "-")])
    assert not res and "sin log" in text
    print("rb_report selftest ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
        sys.exit(0)
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text, res = report([(30, sys.argv[1]), (60, sys.argv[2])])
    print(text)
    sys.exit(0 if res else 1)
