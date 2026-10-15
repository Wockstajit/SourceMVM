"""Generates the bundled osu! easter-egg warm-up beatmap: a synthesized 128 BPM track (kick-ish
click on every beat, a bass note on the bar) and a hand-patterned standard map over it -- circles,
all three slider curve types (linear, perfect circle, bezier), repeats and a spinner.

Nothing licensed: the audio is generated here with ffmpeg's aevalsrc. Output:
    Assets/easter-eggs/osu/SOURCE MVM - Warmup/audio.mp3
    Assets/easter-eggs/osu/SOURCE MVM - Warmup/SOURCE MVM - Warmup (mvm) [Normal].osu

Run from the repo root:  py -3 Automation/tools/make-osu-warmup-map.py
"""
import math
import os
import shutil
import subprocess
import sys

BPM = 128.0
BEAT = 60000.0 / BPM          # ms
LEN_S = 48.0
OFFSET = 1000.0               # first beat, ms
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(ROOT, 'Assets', 'easter-eggs', 'osu', 'SOURCE MVM - Warmup')


def audio(path):
    b = BEAT / 1000.0
    o = OFFSET / 1000.0
    # Beat clicks + a bass sine on each bar + a soft pad, all from time t.
    expr = (
        "0.55*sin(2*PI*90*(t-{o}))*exp(-18*mod(t-{o},{b}))*gte(t,{o})"
        "+0.25*sin(2*PI*1600*(t-{o}))*exp(-90*mod(t-{o},{b}))*gte(t,{o})"
        "+0.20*sin(2*PI*55*t)*exp(-3*mod(t-{o},{bar}))*gte(t,{o})"
        "+0.06*sin(2*PI*220*t)*(0.6+0.4*sin(2*PI*0.25*t))"
    ).format(o=o, b=b, bar=4 * b)
    ffmpeg = shutil.which('ffmpeg') or os.path.join(ROOT, 'build', 'source2-release', 'bin', 'ffmpeg', 'ffmpeg.exe')
    subprocess.run([ffmpeg, '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    # Quoted: the expression's own commas would otherwise split the filter options.
                    '-i', "aevalsrc=exprs='" + expr + "':s=44100:d=" + str(LEN_S),
                    '-af', 'alimiter=limit=0.9', '-c:a', 'libmp3lame', '-q:a', '3', path], check=True)


def beat(n):
    return int(round(OFFSET + n * BEAT))


def build_objects():
    objs = []
    nc = True

    def circle(n, x, y):
        nonlocal nc
        objs.append('%d,%d,%d,%d,0,0:0:0:0:' % (x, y, beat(n), 5 if nc else 1))
        nc = False

    def slider(n, x, y, curve, pts, slides, length):
        nonlocal nc
        body = curve + '|' + '|'.join('%d:%d' % p for p in pts)
        objs.append('%d,%d,%d,%d,0,%s,%d,%g' % (x, y, beat(n), 6 if nc else 2, body, slides, length))
        nc = False

    def spinner(n, end_n):
        nonlocal nc
        objs.append('256,192,%d,12,0,%d,0:0:0:0:' % (beat(n), beat(end_n)))
        nc = True

    def combo():
        nonlocal nc
        nc = True

    # Bars 1-2: a square, one circle a beat.
    for i, (x, y) in enumerate([(128, 96), (384, 96), (384, 288), (128, 288), (256, 96), (384, 192), (256, 288), (128, 192)]):
        circle(4 + i, x, y)
    combo()
    # Bar 3-4: linear sliders (1 beat at SV 1 = 1.4*100 px) and back.
    slider(12, 96, 192, 'L', [(236, 192)], 1, 140)
    circle(14, 300, 120)
    slider(15, 300, 260, 'L', [(440, 260)], 2, 140)  # with a repeat
    combo()
    # Bar 5-6: a circle of eighth-note-free quarters.
    for i in range(8):
        a = i / 8.0 * 2 * math.pi
        circle(20 + i, int(256 + 140 * math.cos(a)), int(192 + 110 * math.sin(a)))
    combo()
    # Bar 7-8: perfect-circle arcs and a bezier S.
    slider(28, 120, 300, 'P', [(200, 220), (280, 300)], 1, 180)
    slider(30, 400, 100, 'P', [(320, 40), (240, 100)], 1, 180)
    slider(32, 80, 120, 'B', [(160, 40), (240, 200), (320, 120)], 1, 280)
    combo()
    # Bar 9-10: back-and-forth jumps.
    for i in range(8):
        circle(36 + i, 96 if i % 2 == 0 else 416, 96 + (i // 2) * 64)
    # Bar 11: spinner for a bar.
    spinner(44, 48)
    combo()
    # Bar 13-16: a stream-lite of half beats then a finale slider.
    for i in range(12):
        n = 50 + i * 0.5
        x = 160 + int(192 * (i / 11.0))
        y = 192 + int(80 * math.sin(i / 2.0))
        objs.append('%d,%d,%d,%d,0,0:0:0:0:' % (x, y, int(round(OFFSET + n * BEAT)), 5 if i == 0 else 1))
    combo()
    slider(58, 80, 330, 'B', [(256, 380), (256, 20), (432, 60)], 1, 420)
    return objs


def main():
    os.makedirs(OUT, exist_ok=True)
    audio(os.path.join(OUT, 'audio.mp3'))
    head = """osu file format v14

[General]
AudioFilename: audio.mp3
AudioLeadIn: 0
PreviewTime: {preview}
Mode: 0

[Metadata]
Title:Warmup
Artist:SOURCE MVM
Creator:mvm
Version:Normal
Source:easter egg
Tags:mvm warmup

[Difficulty]
HPDrainRate:4
CircleSize:4
OverallDifficulty:5
ApproachRate:6
SliderMultiplier:1.4
SliderTickRate:1

[Events]

[TimingPoints]
{offset},{beat},4,2,0,70,1,0

[Colours]
Combo1 : 255,143,216
Combo2 : 159,230,255
Combo3 : 201,167,255
Combo4 : 255,211,107

[HitObjects]
""".format(preview=beat(20), offset=int(OFFSET), beat=repr(BEAT))
    with open(os.path.join(OUT, 'SOURCE MVM - Warmup (mvm) [Normal].osu'), 'w', newline='\n') as f:
        f.write(head + '\n'.join(build_objects()) + '\n')
    print('wrote', OUT)


if __name__ == '__main__':
    sys.exit(main())
