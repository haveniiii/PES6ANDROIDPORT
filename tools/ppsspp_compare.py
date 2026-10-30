#!/usr/bin/env python3
"""Measure the port's picture against PPSSPP's, frame for frame (docs/20 §5).

    python3 tools/ppsspp_compare.py [--points 4@300] [--menus] [--out build/gecompare]
    python3 tools/ppsspp_compare.py --menus --points 4@300,4@1200,4@2500,4@3000,4@3700

Both run the same game, whose own scene stack (pes6_addrs.h: PES6_SCENE_STACK)
counts frames per scene, so "S@F" names the same moment in both:
  1. the port, headless and deterministic, captures each point (PES6_CAPTURE_AT);
  2. PPSSPP is started on data/pes6.iso with its remote debugger; this script
     presses cross every half second until the first point's scene, polls the
     scene stack, pauses the CPU when a point's frame is reached and asks for a
     capture of its window (macOS screencapture; the debugger's own screenshot
     fails with the CPU paused on this Mac, docs/20 §9);
  3. PPSSPP's picture is box-filtered to 480x272 (it renders at its own internal
     resolution) and compared: PSNR, share of pixels off by more than 24, and a
     side-by-side PNG (port | PPSSPP | difference x4).
Points should be still screens: a paused CPU is a frame or two past the target,
which only matters for things that move.
"""
import argparse
import base64
import io
import json
import math
import os
import re
import subprocess
import sys
import time

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ppsspp_ws as ws  # noqa: E402

SCENE_STACK = 0x089ECF88
PPSSPP = '/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL'
BOOT_INPUT = '400:cross,r2600:cross:8,3@200:cross:10,3@330:cross:10'
# --menus: tools/ref_match.sh's presses in the menus (scene 4), replayed in both by scene frame:
# MATCH -> Exhibition -> player settings -> team select -> strip selection -> settings.
MENU_INPUT = ('4@631:cross:4,4@1031:cross:4,4@1431:cross:4,4@1731:right:4,4@1931:down:4,4@2131:cross:4,'
              '4@2431:cross:4,4@2631:down:4,4@2781:right:4,4@2931:cross:4,4@3131:right:4,4@3281:cross:4,'
              '4@3631:cross:4,4@4031:cross:4')


def scene_events(spec):
    """'S@F:button:hold,...' -> [(S, F, button, hold)] (only scene-relative events)."""
    out = []
    for e in spec.split(','):
        m = re.match(r'(\d+)@(\d+):(\w+):(\d+)$', e)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
    return out


def run_port(points, out, menus=False):
    prefix = os.path.join(out, 'port')
    last = max(points, key=lambda p: (p[0] != 3, p[1]))       # stop after the last point
    env = dict(os.environ, PES6_INPUT=BOOT_INPUT + (',' + MENU_INPUT if menus else ''), PES6_VERSION='original', PES6_MENU='0',
               PES6_CAPTURE_AT=','.join(f'{s}@{f}' for s, f in points), PES6_CAPTURE_PREFIX=prefix,
               PES6_STOP_AT=f'{last[0]}@{last[1] + 10}', PES6_IO_ROOT=os.path.join(out, 'io'),
               PES6_MODS=os.path.join(out, 'no_mods'))   # the game's own pictures: no mod at all
    os.makedirs(env['PES6_IO_ROOT'], exist_ok=True)
    os.makedirs(env['PES6_MODS'], exist_ok=True)
    subprocess.run([os.path.join(ROOT, 'runtime/desktop/build/pes6'), 'build/EBOOT.elf', '--seconds', '400'],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {p: f'{prefix}_s{p[0]}_{p[1]:06d}.ppm' for p in points}


def ws_port(pid, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        out = subprocess.run(['lsof', '-nP', '-a', '-p', str(pid), '-iTCP', '-sTCP:LISTEN'],
                             capture_output=True, text=True).stdout
        for m in re.finditer(r':(\d+) \(LISTEN\)', out):
            try:
                s = ws.connect(int(m.group(1)))
                return s
            except Exception:
                pass
        time.sleep(1)
    raise RuntimeError('PPSSPP remote debugger not reachable (Settings > Tools > Developer: allow remote debugger)')


def call(s, event, **kw):
    ws.send_text(s, json.dumps(dict(event=event, **kw)))
    while True:
        msg = json.loads(ws.recv_text(s))
        if msg.get('event') in (event, 'error'):
            if msg.get('event') == 'error':
                raise RuntimeError(f'{event}: {msg.get("message")}')
            return msg


def scene(s):
    """(scene, frame) from the scene stack; (0, 0) while the game is not loaded."""
    try:
        msg = call(s, 'memory.read', address=SCENE_STACK, size=8)
    except RuntimeError:
        return 0, 0
    b = base64.b64decode(msg.get('base64', '')) if msg.get('base64') else b''
    if len(b) < 8:
        return 0, 0
    return int.from_bytes(b[0:4], 'little'), int.from_bytes(b[4:8], 'little')


WINID_SRC = os.path.join(ROOT, 'tools/regress/winid.swift')
WINID = os.path.join(ROOT, 'build/gecompare/winid')


def window_shot(path):
    """The PPSSPP window, captured by macOS (needs Screen Recording permission)."""
    if not os.path.exists(WINID):
        subprocess.run(['swiftc', '-O', WINID_SRC, '-o', WINID], check=True)
    rows = subprocess.run([WINID, 'PPSSPP'], capture_output=True, text=True).stdout.split('\n')
    wid = rows[0].split()[0] if rows and rows[0].strip() else None
    if not wid:
        raise RuntimeError('no PPSSPP window on screen')
    subprocess.run(['screencapture', '-x', '-o', '-l', wid, path], check=True)
    img = Image.open(path).convert('RGB')
    width_pt = int(rows[0].split()[3])
    return img, img.size[0] / max(1, width_pt)        # pixels per point (2 on a Retina screen)


def game_area(img, scale):
    """The game picture inside a window capture: under the 28-point title bar, the largest
    480:272 box centred in the rest (PPSSPP letterboxes the picture)."""
    w, h = img.size
    top = round(28 * scale)
    cw, ch = w, h - top
    gw, gh = (cw, round(cw * 272 / 480)) if cw * 272 <= ch * 480 else (round(ch * 480 / 272), ch)
    x0, y0 = (cw - gw) // 2, top + (ch - gh) // 2
    return img.crop((x0, y0, x0 + gw, y0 + gh))


def run_ppsspp(points, out, menus=False):
    iso = os.path.join(ROOT, 'data/pes6.iso')
    proc = subprocess.Popen([PPSSPP, iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shots = {}
    try:
        s = ws_port(proc.pid)
        todo = sorted(points, key=lambda p: (p[0] != 3, p[1]))  # title first, then the menus
        last_press = 0.0
        t0 = time.time()
        seen, seen_at = None, time.time()
        # The title's presses are the port's own (BOOT_INPUT: 3@200, 3@330); the menus' with --menus.
        events = scene_events('3@200:cross:10,3@330:cross:10' + (',' + MENU_INPUT if menus else ''))
        while todo and time.time() - t0 < 600:
            sc, fr = scene(s)
            if (sc, fr) != seen:
                seen, seen_at = (sc, fr), time.time()
            while events and sc > events[0][0] >= 3:
                events.pop(0)                      # its scene is over (the title can end on one press)
            if events and sc == events[0][0] and fr >= events[0][1]:
                es, ef, button, hold = events.pop(0)
                r = call(s, 'input.buttons.press', button=button, duration=hold)
                if os.environ.get('PPSSPP_COMPARE_LOG'):
                    print(f'  press {button} for {es}@{ef} at frame {fr}: {r}', flush=True)
            want_s, want_f = todo[0]
            if sc > want_s >= 3:                   # its scene ended before the frame: missed
                print(f'  PPSSPP {want_s}@{want_f}: missed (scene {want_s} ended at an earlier frame)')
                todo.pop(0)
                continue
            if sc == want_s and fr >= want_f:
                call(s, 'cpu.stepping')
                sc2, fr2 = scene(s)
                raw, k = window_shot(os.path.join(out, f'window_s{want_s}_{want_f:06d}.png'))
                img = game_area(raw, k)
                path = os.path.join(out, f'ppsspp_s{want_s}_{want_f:06d}.png')
                img.save(path)
                shots[todo[0]] = (path, fr2)
                print(f'  PPSSPP {want_s}@{want_f}: paused at frame {fr2}, {img.size[0]}x{img.size[1]}')
                todo.pop(0)
                call(s, 'cpu.resume')
                continue
            # Cross through the intro and language screens (scenes 1-2), and on any screen whose
            # frame stops counting (waiting for a button) before the menu; from the menu on, only
            # the scripted presses, so the menus stay where the port's own run leaves them.
            stalled = time.time() - seen_at > 1.5
            if time.time() - last_press > 0.5 and (sc in (1, 2) or (sc == 3 and stalled)):
                call(s, 'input.buttons.press', button='cross', duration=4)
                last_press = time.time()
                seen_at = time.time()
            # Every debugger read stalls the emulation a little: poll fast only near a target.
            near = [f for (sc_, f, *_r) in ([e[:2] for e in events[:1]] + todo[:1]) if sc_ == sc]
            time.sleep(0.003 if near and min(near) - fr < 20 else 0.08)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return shots


# Left out of the score: the scrolling help bar (its text moves with time) and PPSSPP's own
# "DevMenu" label (its UI, top left). In 480x272 pixels: (x0, y0, x1, y1).
MASK = [(0, 233, 480, 272), (0, 0, 64, 14)]


def _score(a, b):
    """PSNR and share of pixels off by more than 24, over the unmasked area."""
    b = b.copy()
    w, h = a.size
    masked = 0
    for box in MASK:                      # the masked areas count as equal
        b.paste(a.crop(box), box[:2])
        masked += (min(box[2], w) - box[0]) * (min(box[3], h) - box[1])
    n = w * h - masked
    d = ImageChops.difference(a, b)
    se = sum(v * v * c for band in d.split() for v, c in enumerate(band.histogram()))
    mse = se / (3 * n)
    r, g, bl = d.split()
    worst = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    off = sum(worst.histogram()[25:]) / n
    return (99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)), off


def compare(port_path, ppsspp_path, out_png):
    """Score the port's 480x272 frame against PPSSPP's (bigger) picture at the best alignment:
    PPSSPP's crop shifted by up to eight of its pixels up/down and four sideways (the title bar's
    height is an estimate), then box-filtered to 480x272."""
    a = Image.open(port_path).convert('RGB')
    big = Image.open(ppsspp_path).convert('RGB')
    W, H = big.size
    best = None
    for dy in range(-8, 9):
        for dx in range(-4, 5):
            b = big.crop((dx, dy, W + dx, H + dy)).resize(a.size, Image.BOX)
            psnr, off = _score(a, b)
            if not best or psnr > best[0]:
                best = (psnr, off, b, (dx, dy))
    psnr, off, b, shift = best
    d = ImageChops.difference(a, b)
    sheet = Image.new('RGB', (a.size[0] * 3, a.size[1]))
    sheet.paste(a, (0, 0)); sheet.paste(b, (a.size[0], 0)); sheet.paste(d.point(lambda v: min(255, v * 4)), (a.size[0] * 2, 0))
    sheet.save(out_png)
    return psnr, off, shift


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--points', default='3@150,4@300')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/gecompare'))
    ap.add_argument('--menus', action='store_true', help='replay tools/ref_match.sh in the menus (team select...)')
    a = ap.parse_args()
    points = [tuple(int(x) for x in p.split('@')) for p in a.points.split(',')]
    os.makedirs(a.out, exist_ok=True)
    print('port: capturing', a.points)
    port = run_port(points, a.out, a.menus)
    print('PPSSPP: capturing (its window will open)')
    pp = run_ppsspp(points, a.out, a.menus)
    print(f'{"point":>8}  {"PSNR":>6}  {"off>24":>7}  picture')
    for p in points:
        if p not in pp or not os.path.exists(port[p]):
            print(f'{p[0]}@{p[1]:<6}  missing ({"port" if not os.path.exists(port[p]) else "PPSSPP"})')
            continue
        png = os.path.join(a.out, f'compare_s{p[0]}_{p[1]:06d}.png')
        psnr, off, shift = compare(port[p], pp[p][0], png)
        print(f'{p[0]}@{p[1]:<6}  {psnr:6.1f}  {off * 100:6.2f}%  {png}  (PPSSPP frame {pp[p][1]}, shift {shift})')


if __name__ == '__main__':
    main()
