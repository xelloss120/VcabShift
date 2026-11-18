# 有声と無声のつなぎ（10ms）の間の音量の確かめ（2026-11-18）。
# 処理ツールの --streams（有声の流れ、無声の流れ、つなぎの角度）から、つなぎごとに次を出す（入力は遅延の分ずらして比べる）。
#   - へこみ: つなぎの間（10ms）の出力の強さを、「2 つの流れがそれぞれ入力に付いて行った場合」の強さと比べた dB。
#     有声の流れの入力に対する強さ g_v と無声の g_u を、つなぎの前と後の 10ms（有声の側と無声の側）で求め、
#     Σ (sin²θ g_v + cos²θ g_u) x² を狙いとする。0dB なら、つなぎの間も両方の流れが入力どおり鳴っている。
#   - 消えていく側の流れ（声の終わりは有声、出だしは無声）の不足: Σ sin²θ v² と Σ sin²θ g_v x² の比（出だしは cos²θ と u）。
# 声の終わり（有声 → 無声）と出だし（無声 → 有声）に分け、前後 10ms がそれぞれの側にとどまるつなぎだけを見る。
# 短い窓（2.5ms）の音量の比は、声の 1 周期の中の強弱で ±5dB 揺れて、へこみが測れなかったので、つなぎ全体の強さで比べる。
import subprocess
import sys

import numpy as np
import scipy.io.wavfile as wf

from common import FS, RENDER, Builder, args_with_voice, path, read, write

sys.stdout.reconfigure(encoding='utf-8')

FADE = int(0.010 * FS)
HALF_PI = np.pi / 2


def rms_env(x, win):
    k = np.ones(win) / win
    return np.sqrt(np.convolve(x * x, k, 'same') + 1e-20)


def streams(inp, settings, exe=RENDER):
    out = path('fade_out.wav')
    st = path('fade_streams.wav')
    r = subprocess.run([exe, inp, out, '--streams', st] + list(settings), check=True, capture_output=True, text=True).stdout
    latency = int(r.split('latency ')[1].split(' ')[0])
    _, s = wf.read(st)
    return s.astype(np.float64), latency


def analyse(inp, settings, exe=RENDER, label=''):
    _, x = read(inp)
    s, lat = streams(inp, settings, exe)
    v, u, th = s[:, 0], s[:, 1], s[:, 2]
    y = np.where(th >= HALF_PI - 1e-6, v, np.where(th <= 1e-6, u, np.sin(th) * v + np.cos(th) * u))
    xin = np.zeros(len(y))
    xin[lat:lat + len(x)] = x[:len(y) - lat]
    loud = rms_env(xin, int(0.010 * FS)) > np.max(np.abs(x)) * 10 ** (-40 / 20)  # 入力が小さすぎる所は比を見ない
    res = {'end': [], 'onset': []}
    full = th >= HALF_PI - 1e-6
    zero = th <= 1e-6
    W = int(0.010 * FS)

    def energy(a, sl, w=None):
        return np.sum((a[sl] ** 2) * (1.0 if w is None else w)) + 1e-20

    # つなぎの始まり: 角度が端を離れた最初の標本
    for kind, a, b in (('end', full, zero), ('onset', zero, full)):
        starts = np.where(a[:-1] & ~a[1:])[0] + 1
        for s0 in starts:
            if s0 + FADE + W + 1 >= len(y) or s0 < W + 1:
                continue
            # 途中で引き返さず、反対の端まで行き、前後 10ms はそれぞれの端にとどまるつなぎだけ
            if not (a[s0 - W:s0].all() and b[s0 + FADE:s0 + FADE + W].all()):
                continue
            pre, mid, post = slice(s0 - W, s0), slice(s0, s0 + FADE), slice(s0 + FADE, s0 + FADE + W)
            if not (loud[pre].all() and loud[mid].all() and loud[post].all()):
                continue
            vs, us = (pre, post) if kind == 'end' else (post, pre)
            gv = energy(v, vs) / energy(xin, vs)
            gu = energy(u, us) / energy(xin, us)
            sv, cu = np.sin(th[mid]) ** 2, np.cos(th[mid]) ** 2
            want = gv * energy(xin, mid, sv) + gu * energy(xin, mid, cu)
            dip = 10 * np.log10(energy(y, mid) / want)
            if kind == 'end':
                lack = 10 * np.log10(energy(v, mid, sv) / (gv * energy(xin, mid, sv)))
            else:
                lack = 10 * np.log10(energy(u, mid, cu) / (gu * energy(xin, mid, cu)))
            res[kind].append((dip, lack, s0))
    print(f'  {label}')
    for kind, name in (('end', '声の終わり（有声 → 無声）'), ('onset', '声の出だし（無声 → 有声）')):
        r = np.array(res[kind])
        if len(r) == 0:
            print(f'    {name}: なし')
            continue
        d = r[:, 0]
        print(f'    {name}: {len(r)} 回。へこみ 中央値 {np.median(d):+.1f}dB、10% 点 {np.percentile(d, 10):+.1f}dB、'
              f'一番深い {d.min():+.1f}dB、3dB 以上へこむ {np.sum(d <= -3)} 回。'
              f'消えていく{"有声" if kind == "end" else "無声"}の流れの不足 中央値 {np.median(r[:, 1]):+.1f}dB、一番大きい {r[:, 1].min():+.1f}dB')
    return res


def synth():
    """母音 → 摩擦音 → 母音 と、母音 → 無音 を、高さと母音を変えて並べた合成の声"""
    b = Builder(seed=11)
    for f0, v in ((110, 'a'), (130, 'i'), (150, 'u'), (120, 'e'), (140, 'o'), (100, 'a'), (170, 'e'), (125, 'o')):
        b.gap(0.15)
        b.voice(f0, 0.3, v)
        b.noise(0.12, 0.03, highpass=2500)
        b.voice(f0 * 1.05, 0.3, v)
        b.noise(0.1, 0.01, highpass=1000)
    b.gap(0.15)
    x, _, _ = b.done()
    p = path('fade_synth.wav')
    write(p, x)
    return p


def main():
    p = args_with_voice('有声と無声のつなぎの間の音量')
    p.add_argument('--exe', default=RENDER)
    a = p.parse_args()
    sets = {'初期値': [], 'ピッチ 0・フォルマント 0・無声フォルマント 0': ['pitch=0', 'formant=0', 'unvoiced_formant=0']}
    inputs = [('合成の声', synth())] + ([('録音した声', a.voice)] if a.voice else [])
    for name, inp in inputs:
        print(name)
        for label, st in sets.items():
            analyse(inp, st, a.exe, label)


if __name__ == '__main__':
    main()
