#!/usr/bin/env python3
"""Put a song of your own in place of one of PES6's music tracks (docs/11 §8).

    python3 tools/music_replace.py <track> <song> [--add | --chance PERCENT] [--iso data/pes6.iso] [--out mods/music]
    python3 tools/music_replace.py --remove <track> [--out mods/music]
    python3 tools/music_replace.py --list [--out mods/music]

<track> is the entry number in PSP_GAME/USRDIR/0_sound.afs: 14 is the title
and main menu theme; the idle demo plays many others (3, 4, 5, 6, ...) about a
minute each. --chance 20 makes the song play in one app start out of five
(rolled once per run; the original plays otherwise). The song (anything ffmpeg reads) becomes
16-bit stereo PCM at 44.1 kHz -- what the game's own ATRAC3plus tracks decode
to -- at the loudness of the track it replaces (EBU R128), with a limiter so
nothing clips. The file is named track<NNN>_<file size>_<samples>.pcm after
the original's RIFF header (track<NNN>_c<PERCENT>_... with --chance), which is how runtime/common/atrac.c recognises
the stream. --add keeps the track's other songs (track<NNN>_n<K>_...): with several, each time
the track starts one of them plays, all equally likely, and never the original. mods/ is not versioned: the song stays on this machine (and in the
APKs built here with `mods` or `bundle`).
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afs_overlays import iter_iso_files  # noqa: E402


def sound_track(iso_path, track):
    """The RIFF/WAVE bytes of 0_sound.afs entry `track`."""
    with open(iso_path, 'rb') as iso:
        afs = next((f for f in iter_iso_files(iso) if f[0].lower().endswith('usrdir/0_sound.afs')), None)
        if not afs:
            sys.exit('0_sound.afs not found in the ISO')
        base = afs[1] * 2048
        iso.seek(base)
        first = struct.unpack('<I', iso.read(4))[0]          # the first entry's sector = table size
        iso.seek(base)
        table = iso.read(first * 2048)
        count = min(len(table) // 8, 4096)
        entries = []
        for i in range(count):                               # pairs up to the first empty one
            e = struct.unpack_from('<II', table, i * 8)
            if not e[1]:
                break
            entries.append(e)
        if not 0 <= track < len(entries):
            sys.exit(f'track {track}: 0_sound.afs has {len(entries)} entries')
        start, sectors = entries[track]
        iso.seek(base + start * 2048)
        data = iso.read(sectors * 2048)
    if data[:4] != b'RIFF':
        sys.exit(f'track {track} is not RIFF/WAVE')
    return data


def riff_key(data):
    """(file size, length in samples) as atrac.c computes them."""
    file_size = struct.unpack_from('<I', data, 4)[0] + 8
    pos, samples, data_off, data_size, block = 12, None, None, 0, 0
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if cid == b'data':
            data_off, data_size = pos + 8, size
            break
        if cid == b'fmt ':
            block = struct.unpack_from('<H', data, pos + 8 + 12)[0]
        if cid == b'fact':
            samples = struct.unpack_from('<I', data, pos + 8)[0]
        pos += 8 + size + (size & 1)
    if data_off is not None:
        file_size = max(file_size, data_off + data_size)
    if samples is None:
        samples = data_size // block * 2048
    return file_size, samples


def loudness(path):
    """Integrated loudness (LUFS) of anything ffmpeg reads."""
    out = subprocess.run(['ffmpeg', '-nostats', '-i', path, '-af', 'ebur128', '-f', 'null', '-'],
                         capture_output=True, text=True).stderr
    found = re.findall(r'I:\s+(-?[\d.]+) LUFS', out)
    if not found:
        sys.exit(f'could not measure the loudness of {path}')
    return float(found[-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('track', nargs='?', type=int)
    ap.add_argument('song', nargs='?')
    ap.add_argument('--iso', default='data/pes6.iso')
    ap.add_argument('--out', default='mods/music')
    ap.add_argument('--chance', type=int, default=100, metavar='PERCENT',
                    help='share of app starts in which the song plays (default 100)')
    ap.add_argument('--add', action='store_true',
                    help="keep the track's other songs: one of them plays each time, all equally likely")
    ap.add_argument('--remove', type=int, metavar='TRACK')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    mine = sorted(f for f in os.listdir(a.out) if re.match(r'track\d{3}_([cn]\d+_)?[0-9a-f]{8}_[0-9a-f]{8}\.pcm$', f))
    if a.list:
        for f in mine:
            print(f'{f}  {os.path.getsize(os.path.join(a.out, f)) / 4 / 44100:.1f} s')
        return
    if a.remove is not None:
        for f in mine:
            if f.startswith(f'track{a.remove:03d}_'):
                os.remove(os.path.join(a.out, f))
                print('removed', f)
        return
    if a.track is None or not a.song:
        ap.error('give a track and a song (or --list / --remove)')

    data = sound_track(a.iso, a.track)
    size, samples = riff_key(data)
    original = os.path.join(a.out, f'.track{a.track:03d}.at3')
    with open(original, 'wb') as f:
        f.write(data)
    target = loudness(original)
    os.remove(original)
    gain = target - loudness(a.song)

    ours = [f for f in mine if f.startswith(f'track{a.track:03d}_')]
    if a.add:                                        # one more song: the next free number
        used = {int(m.group(1)) for f in ours for m in [re.match(r'track\d{3}_n(\d+)_', f)] if m}
        chance = f'n{min(set(range(1, len(used) + 2)) - used):02d}_'
    else:                                            # the only song of this track
        for f in ours:
            os.remove(os.path.join(a.out, f))
        chance = f'c{a.chance}_' if 0 < a.chance < 100 else ''
    dst = os.path.join(a.out, f'track{a.track:03d}_{chance}{size:08x}_{samples:08x}.pcm')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', a.song, '-vn',
                    '-af', f'aresample=44100:filter_size=64:phase_shift=10:cutoff=0.97,volume={gain:.2f}dB,'
                           'alimiter=limit=0.95:level=false',
                    '-ac', '2', '-ar', '44100', '-f', 's16le', dst], check=True)
    secs = os.path.getsize(dst) / 4 / 44100
    print(json.dumps({'track': a.track, 'file': dst, 'chance_percent': a.chance, 'seconds': round(secs, 1),
                      'original_seconds': round(samples / 44100, 1), 'gain_db': round(gain, 2)}))


if __name__ == '__main__':
    main()
