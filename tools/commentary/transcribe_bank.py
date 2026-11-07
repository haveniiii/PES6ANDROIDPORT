#!/usr/bin/env python3
"""Transcribe a voice bank clip by clip from its own audio (docs/24).

    python3 tools/commentary/transcribe_bank.py BANK.afs OUT.tsv [--last 7999] [--workers 2]
        [--lang es|en] [--clips a-b,c,...]

The catalogue must come from the bank it plays: PECH's recording has the same blocks as the older
Palma bank, but not the same clip in each place. OUT.tsv (clip, seconds, event, score, text) is
what build_catalogue.py reads; event and score stay empty. Already written clips are skipped, so
a stopped run goes on where it was. Every clip is padded with silence: whisper drops short ones.
"""
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile

MODEL = 'vendor/whisper/ggml-small.bin'
LANG = 'es'
PAD = 0.8


def quiet(*cmd):
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def transcribe(job):
    clip, data, tmp = job
    base = os.path.join(tmp, 'c%d' % clip)
    with open(base + '.adx', 'wb') as f:
        f.write(data)
    quiet('ffmpeg', '-nostdin', '-y', '-f', 'adx', '-i', base + '.adx', '-ar', '16000', '-ac', '1', base + '.wav')
    if not os.path.exists(base + '.wav'):
        return clip, 0.0, ''
    seconds = (os.path.getsize(base + '.wav') - 78) / 32000.0
    quiet('ffmpeg', '-nostdin', '-y', '-i', base + '.wav', '-af',
          'adelay=%d,apad=pad_dur=%g' % (PAD * 1000, PAD), base + '_p.wav')
    quiet('whisper-cli', '-m', MODEL, '-l', LANG, '-nt', '-t', '3', '-otxt', '-of', base, '-f', base + '_p.wav')
    text = ''
    if os.path.exists(base + '.txt'):
        text = ' '.join(open(base + '.txt', errors='replace').read().split())
    for ext in ('.adx', '.wav', '_p.wav', '.txt'):
        if os.path.exists(base + ext):
            os.remove(base + ext)
    return clip, seconds, text


def main():
    global LANG
    opts, args, argv = {}, [], sys.argv[1:]
    while argv:
        a = argv.pop(0)
        if a.startswith('--'):
            opts[a[2:]] = argv.pop(0)
        else:
            args.append(a)
    last = int(opts.get('last', 7999))
    workers = int(opts.get('workers', 2))
    LANG = opts.get('lang', 'es')
    only = None
    if 'clips' in opts:
        only = set()
        for part in opts['clips'].split(','):
            a, _, b = part.partition('-')
            only.update(range(int(a), int(b or a) + 1))
    bank, out = args
    f = open(bank, 'rb')
    if f.read(4) != b'AFS\0':
        sys.exit('not an AFS file')
    n = struct.unpack('<I', f.read(4))[0]
    table = [struct.unpack('<II', f.read(8)) for _ in range(n)]
    done = set()
    if os.path.exists(out):
        done = {int(l.split('\t')[0]) for l in open(out) if l[0].isdigit()}
    else:
        open(out, 'w').write('clip\tseconds\tevent\tscore\ttext\n')
    todo = [i for i in (sorted(only) if only is not None else range(min(n, last + 1)))
            if i < n and table[i][1] and i not in done]
    print('%d clips to do, %d done' % (len(todo), len(done)), flush=True)
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(workers) as pool:
        def jobs():
            for i in todo:
                f.seek(table[i][0])
                yield i, f.read(table[i][1]), tmp
        with open(out, 'a') as o:
            for k, (clip, seconds, text) in enumerate(pool.map(transcribe, jobs())):
                o.write('%d\t%.2f\t-\t\t%s\n' % (clip, seconds, text.replace('\t', ' ')))
                o.flush()
                if k % 100 == 0:
                    print(k, flush=True)


if __name__ == '__main__':
    main()
