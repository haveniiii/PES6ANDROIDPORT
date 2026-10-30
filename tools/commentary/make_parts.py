#!/usr/bin/env python3
"""Cut clips out of a PS2 ?_sound.afs and batch them for whisper (docs/24).

    python3 tools/commentary/make_parts.py BANK.afs FIRST LAST WAVDIR PARTDIR [--minutes 10]
        [--gap 1.2] [--separator WORD.wav --word WORD]

Each clip (ADX) becomes WAVDIR/<clip>.wav (16 kHz mono, by ffmpeg); empty slots are skipped.
Whisper pads every input to 30 s, so the clips are joined with 1.2 s of silence into
PARTDIR/part<N>.wav of about ten minutes (--minutes, --gap seconds), with PARTDIR/part<N>.json = [[clip, start, end], ...].
Lists of short clips (names) come back from whisper as one long segment with loose times: then
a spoken separator (say -v Eddy -o w.aiff siguiente) goes between the clips, PARTDIR/separator.txt
names the word, and split_parts.py cuts the text at it instead of by time.
Then: whisper-cli -m vendor/whisper/ggml-small.bin -l es -ojf -f PARTDIR/part<N>.wav for each,
and tools/commentary/split_parts.py PARTDIR WAVDIR.
"""
import json
import os
import struct
import subprocess
import sys
import wave

RATE, GAP = 16000, 1.2


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = {'--minutes': 10.0, '--gap': GAP, '--separator': None, '--word': None}
    for k in opt:
        if k in sys.argv:
            v = sys.argv[sys.argv.index(k) + 1]
            opt[k] = v if k in ('--separator', '--word') else float(v)
            args.remove(v)
    minutes, gap = opt['--minutes'], opt['--gap']
    bank, first, last, wavdir, partdir = args[0], int(args[1]), int(args[2]), args[3], args[4]
    os.makedirs(wavdir, exist_ok=True)
    os.makedirs(partdir, exist_ok=True)
    with open(bank, 'rb') as fh:
        head = fh.read(8)
        assert head[:4] == b'AFS\0'
        count = struct.unpack('<I', head[4:])[0]
        tab = struct.unpack('<%dI' % (2 * count), fh.read(8 * count))
        for c in range(first, min(last, count - 1) + 1):
            out = os.path.join(wavdir, '%05d.wav' % c)
            off, size = tab[2 * c], tab[2 * c + 1]
            if size < 200 or os.path.exists(out):
                continue
            fh.seek(off)
            data = fh.read(size)
            if data[:2] != b'\x80\x00':              # padding left in a patched bank, not ADX
                continue
            r = subprocess.run(['ffmpeg', '-loglevel', 'quiet', '-y', '-f', 'adx', '-i', '-', '-ar', str(RATE),
                                '-ac', '1', out], input=data)
            if r.returncode != 0 and os.path.exists(out):
                os.remove(out)
    silence = b'\0\0' * int(RATE * gap)
    if opt['--separator']:
        with wave.open(opt['--separator']) as w:
            assert w.getframerate() == RATE and w.getnchannels() == 1
            silence += w.readframes(w.getnframes()) + silence
        with open(os.path.join(partdir, 'separator.txt'), 'w') as fh:
            fh.write(opt['--word'] + '\n')
    part, pos, index, frames = 0, 0.0, [], []

    def flush():
        nonlocal part, pos, index, frames
        if not index:
            return
        name = os.path.join(partdir, 'part%03d' % part)
        with wave.open(name + '.wav', 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(b''.join(frames))
        json.dump(index, open(name + '.json', 'w'))
        part, pos, index, frames = part + 1, 0.0, [], []

    for c in range(first, last + 1):
        path = os.path.join(wavdir, '%05d.wav' % c)
        if not os.path.exists(path):
            continue
        with wave.open(path) as w:
            pcm = w.readframes(w.getnframes())
        secs = len(pcm) / 2 / RATE
        index.append([c, pos, pos + secs])
        frames += [pcm, silence]
        pos += secs + len(silence) / 2 / RATE
        if pos > minutes * 60:
            flush()
    flush()
    print('%d parts in %s' % (part, partdir))


if __name__ == '__main__':
    main()
