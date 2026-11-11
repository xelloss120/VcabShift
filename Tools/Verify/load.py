# 負荷の確かめ（処理ツールで、実時間に対する割合）。
#   python Tools/Verify/load.py [--voice 録音.wav] [--compare 別の vcabsh_render.exe]
# 音を 10 回つないで長くし（3 分ほど）、設定ごとに負荷を出す。黙っている間を無音にした音（手前のゲートが閉じている形）でも出す。
# --voice がなければ、合成の声（母音と摩擦音と無音）を使う。
import re

import numpy as np
from scipy.ndimage import maximum_filter1d, uniform_filter1d

import common as C

SETTINGS = (('初期値', ()), ('探索 40〜1000Hz', ('pitch_min=40', 'pitch_max=1000')), ('有声に補正（F2 +3）', ('f2_delta=3',)),
            ('有声・無声に補正', ('f2_delta=3', 'uf2_delta=3')), ('素通し（一律・0）', ('warp_mode=0', 'formant=0', 'unvoiced_formant=0', 'pitch=0')))


def cpu(exe, inp, settings):
    msg = C.render(inp, C.path('_x.wav'), settings, exe=exe)
    return float(re.search(r'cpu ([0-9.]+)%', msg).group(1))


def main():
    ap = C.args_with_voice('負荷の確かめ')
    ap.add_argument('--compare', help='比べる別の版の vcabsh_render.exe（新しいキーを知らない版では、その設定は飛ばす）')
    a = ap.parse_args()
    if a.voice:
        fs, x = C.read(a.voice)
    else:
        b = C.Builder(3).gap(0.3)
        for f0 in (120, 180):
            b.voice(f0, 0.6, 'a').noise(0.12, 0.05).voice(f0, 0.5, 'o').gap(0.4)
        x, _, _ = b.done()
        fs = C.FS
    x = np.tile(x, 10)
    lvl = np.sqrt(uniform_filter1d(x ** 2, fs // 100))
    gate = maximum_filter1d((lvl > 10 ** (-45 / 20)).astype(int), fs // 10) > 0
    long_ = C.path('load_long.wav'); C.write(long_, x, fs)
    gated = C.path('load_gated.wav'); C.write(gated, x * gate, fs)
    print(f'長さ {len(x) / fs:.0f} 秒。ゲートが閉じている割合 {(1 - gate.mean()) * 100:.0f}%')
    exes = [('今の版', C.RENDER)] + ([('--compare の版', a.compare)] if a.compare else [])
    for label, s in SETTINGS:
        row = []
        for name, exe in exes:
            for which, inp in (('', long_), ('ゲート', gated)):
                try:
                    row.append(f'{name}{which} {cpu(exe, inp, s):.2f}%')
                except Exception:
                    row.append(f'{name}{which} -')
        print(f'   {label:20s} ' + '  '.join(row))


if __name__ == '__main__':
    main()
