# 回帰テスト: 合成の母音を、いくつかの設定で vcabsh_render に通し、基準の出力と比べる。
#   python regress.py make   … 今の版の出力を基準として保存する
#   python regress.py check  … 基準と比べる（一致しない設定を出す）
#   python regress.py eval   … 数値の確かめ（声の高さ、写像どおりに動いたか）を出す
# 基準と作業用のファイルは build/regress に置く（Git の対象外）。
# 必要なもの: Python 3、numpy、scipy。先に vcabsh_render をビルドしておく。
import os
import subprocess
import sys

import numpy as np
import scipy.io.wavfile as wf
import scipy.signal as ss

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
RENDER = os.path.join(ROOT, 'build', 'vcabsh_render_artefacts', 'Release', 'vcabsh_render.exe')
WORK = os.path.join(ROOT, 'build', 'regress')
FS = 48000

VOWELS = {'a': (750, 1200, 2600, 3500), 'i': (300, 2300, 3000, 3700), 'u': (350, 1400, 2300, 3500),
          'e': (480, 2000, 2700, 3600), 'o': (480, 850, 2600, 3500)}
BANDWIDTHS = (90, 110, 160, 250)
SEG = 0.7  # 1 母音の長さ（0.6 秒の声と 0.1 秒の無音）

CASES = {
    'through_yin': 'pitch=0 formant=0',
    'default': '',
    'bands': 'warp_mode=2 f1_delta=-3 f2_delta=4 f4_delta=2 above_f4=2',
    'formants': 'f2_delta=4',
    'formants_above_uniform': 'f1_delta=-2 f3_delta=3 above_f4=1',
    'formant_down': 'pitch=-12 formant=-12',
    'extreme_up': 'pitch=24 formant=24',
    'fmin200': 'pitch_min=200 pitch_max=150',
    'fmin40': 'pitch_min=40 pitch_max=1000',
}


def vowel(v, f0, dur):
    """声帯のパルス列（-12dB/oct、ビブラート 1%）に口の放射（+6dB/oct）をかけ、F1〜F4 の共振に通す"""
    n = int(FS * dur)
    t = np.arange(n) / FS
    f = f0 * (1 + 0.01 * np.sin(2 * np.pi * 5 * t))
    ph = np.cumsum(f) / FS
    pulse = np.zeros(n)
    pulse[np.where(np.diff(np.floor(ph)) > 0)[0]] = 1
    y = ss.lfilter([1], [1, -0.97], ss.lfilter([1], [1, -0.97], pulse))
    y = ss.lfilter([1, -1], [1], y)
    for F, B in zip(VOWELS[v], BANDWIDTHS):
        r = np.exp(-np.pi * B / FS)
        th = 2 * np.pi * F / FS
        y = ss.lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], y)
    y = y / np.max(np.abs(y)) * 0.3
    return y * np.minimum(1, np.minimum(t / 0.02, (dur - t) / 0.02))


def make_input(path, f0):
    sig = np.concatenate([np.concatenate([vowel(v, f0, SEG - 0.1), np.zeros(int(0.1 * FS))]) for v in VOWELS])
    wf.write(path, FS, (sig * 32767).astype(np.int16))


def read(path):
    fs, x = wf.read(path)
    return x.astype(np.float64) / (2 ** 31 if x.dtype == np.int32 else 32768)


def render(inp, out, args):
    subprocess.run([RENDER, inp, out, '--align'] + args.split(), check=True, stdout=subprocess.DEVNULL)


def run_all(suffix):
    os.makedirs(WORK, exist_ok=True)
    inp = os.path.join(WORK, 'in120.wav')
    make_input(inp, 120)
    for name, args in CASES.items():
        render(inp, os.path.join(WORK, f'{name}{suffix}.wav'), args)
    return inp


def check():
    run_all('_new')
    bad = 0
    for name in CASES:
        ref = os.path.join(WORK, f'{name}_ref.wav')
        if not os.path.exists(ref):
            print(f'{name}: 基準がない（make を先に）')
            bad += 1
            continue
        a, b = read(ref), read(os.path.join(WORK, f'{name}_new.wav'))
        diff = np.max(np.abs(a - b)) if len(a) == len(b) else float('inf')
        status = '一致' if diff == 0 else f'違う（最大の差 {diff:.2e}）'
        if diff != 0:
            bad += 1
        print(f'{name:24s} {status}')
    print('すべて一致' if bad == 0 else f'{bad} 件が違う')
    return bad


def harmonics(x, s, e, f0):
    f, P = ss.welch(x[int(s * FS):int(e * FS)], FS, nperseg=16384, window='blackmanharris')
    hs = np.arange(f0, 6000, f0)
    return hs, np.array([10 * np.log10(P[np.abs(f - h) < f0 * 0.3].max() + 1e-20) for h in hs])


def shift_curve(x, points):
    if x <= points[0][0]:
        return points[0][1]
    for (x0, d0), (x1, d1) in zip(points, points[1:]):
        if x <= x1:
            t = (x - x0) / (x1 - x0)
            return d0 + (d1 - d0) * 0.5 * (1 - np.cos(np.pi * t))
    return points[-1][1]


def evaluate():
    inp = run_all('_eval')
    x = read(inp)
    # 声の高さ: ピッチ +12 で 240Hz になるか（自己相関の山）
    y = read(os.path.join(WORK, 'default_eval.wav'))
    f0s = []
    for i in range(5):
        seg = y[int((i * SEG + 0.1) * FS):int((i * SEG + 0.5) * FS)]
        seg = seg - seg.mean()
        r = np.fft.irfft(np.abs(np.fft.rfft(seg, 2 * len(seg))) ** 2)[:len(seg)]
        lo, hi = FS // 1000, FS // 60
        f0s.append(FS / (lo + np.argmax(r[lo:hi])))
    print('ピッチ +12 の声の高さ（狙い 240Hz）:', ' '.join(f'{f:.1f}' for f in f0s))

    # 写像どおりに動いたか: 入力の倍音の大きさを、写像で動かした「狙い」との差（200Hz〜3.8kHz、全体の大きさはそろえる）
    def warp_error(name, pts_for):
        out = read(os.path.join(WORK, f'{name}_eval.wav'))
        errs = []
        for i, v in enumerate(VOWELS):
            s, e = i * SEG + 0.1, i * SEG + 0.5
            hs, a = harmonics(x, s, e, 120)
            _, b = harmonics(out, s, e, 120)
            pts = pts_for(v)
            fo = np.array([h * 2 ** (shift_curve(np.log2(h), pts) / 12) for h in hs])
            target = np.interp(np.log2(hs), np.log2(fo), a)
            m = (hs >= 200) & (hs <= 3800)
            d = (b - target)[m]
            d -= np.median(d)
            errs.append(np.sqrt(np.mean(d ** 2)))
        print(f'{name:10s} 狙いの包絡との差 dB:', ' '.join(f'{v} {e:.1f}' for v, e in zip(VOWELS, errs)), f' 平均 {np.mean(errs):.1f}')

    render(inp, os.path.join(WORK, 'uniform4_eval.wav'), 'pitch=0 formant=4')
    render(inp, os.path.join(WORK, 'formants4_eval.wav'), 'pitch=0 formant=0 f2_delta=4')
    warp_error('uniform4', lambda v: [(0.0, 4.0)])

    def formant_pts(v):
        p = [(np.log2(F), d) for F, d in zip(VOWELS[v], (0, 4, 0, 0))]
        return p + [(p[-1][0] + 1 / 3, 0.0)]
    warp_error('formants4', formant_pts)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'check'
    if cmd == 'make':
        run_all('_ref')
        print('基準を保存した:', WORK)
    elif cmd == 'check':
        sys.exit(1 if check() else 0)
    elif cmd == 'eval':
        evaluate()
    else:
        print(__doc__)
