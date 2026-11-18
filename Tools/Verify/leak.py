# 地声の貫通の確かめ（2026-11-11）。
# 帯域に分けた出力の包絡（強弱）のスペクトルで、入力の基本周波数 f0（地声の高さ）の所の山が、
# 周り（0.7〜0.85 倍と 1.15〜1.3 倍）より何 dB 高いかを、有声のフレームごとに求めて平均する。
# 地声の成分がなければ 0dB 以下（入れ替えなしの初期値で 4kHz より上は -2〜-3dB）。入力そのものは約 +13dB。
# 出力の高さ（+12 半音なら 2f0）の山も並べて出す（入れ替えなしで約 +12dB）。
# 高い帯域の入れ替えの最初の形（無声の流れを使っていた）は、8〜12kHz で +9.4dB だった。
# 2026-11-18: 帯域の下限を 2kHz から 100Hz に広げた（100〜250、250〜500、500Hz〜1kHz、1〜2kHz を足した）。
#   低い帯域は出力の倍音が帯域に数本しかなく、包絡の山は測りにくいので、もう 1 つの見方として、出力のスペクトルで
#   入力の f0 の奇数倍（f0、3f0、5f0 ...。+12 半音の出力の倍音 2f0 の倍数にはない所）の強さを、偶数倍（出力の倍音）と比べた値も出す
#   （「奇数倍」。入力そのものは約 0dB、地声の成分がなければ大きく負）。80ms の窓で、窓の中の高さの変わりが 2% 以内のフレームだけ。
# 使い方: python leak.py frames.csv out.wav [out2.wav ...]
#   frames.csv は vcabsh_render の --frames で書いたもの（入力の高さ）。out.wav は --align で書いたもの。
import csv
import sys

import numpy as np
import scipy.io.wavfile as wf
import scipy.signal as sg


def load(p):
    sr, x = wf.read(p)
    # 整数の WAV は ±1 にそろえる（24 ビットは int32 で読まれる。前は 32768 で割っていて、処理ツールの出力が 65536 倍になり、
    # 下の「小さすぎるフレームを除く」しきい値が効いていなかった）
    scale = {'int16': 32768.0, 'int32': 2.0 ** 31}.get(x.dtype.name, 1.0)
    x = x.astype(np.float64) / scale
    if x.ndim > 1:
        x = x.mean(1)
    return sr, x


def peak(spec, freqs, f):
    at = spec[np.argmin(np.abs(freqs - f))]
    m = ((freqs > 0.7 * f) & (freqs < 0.85 * f)) | ((freqs > 1.15 * f) & (freqs < 1.3 * f))
    return 10 * np.log10(at / (np.median(spec[m]) + 1e-30) + 1e-30)


BANDS = ((60, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 12000))


def harmonic_power(spec, freqs, f, half):
    m = (freqs > f - half) & (freqs < f + half)
    return spec[m].sum()


def odd_even(x, sr, fr, lo, hi):
    """帯域 lo〜hi の中の、f0 の奇数倍と偶数倍の強さの比（dB）。高さが安定したフレームだけ"""
    n = int(0.08 * sr)
    w = np.hanning(n)
    nfft = 1 << 17
    freqs = np.fft.rfftfreq(nfft, 1 / sr)
    ts = np.array([t for t, _ in fr])
    fs_ = np.array([f for _, f in fr])
    out = []
    for t, f0 in fr[::2]:
        c = int(t * sr)
        if c - n // 2 < 0 or c + n // 2 >= len(x):
            continue
        near = fs_[np.abs(ts - t) <= 0.04]
        if len(near) < 5 or near.max() / near.min() > 1.02:
            continue
        s = np.abs(np.fft.rfft(x[c - n // 2:c - n // 2 + n] * w, nfft)) ** 2
        ks = [k for k in range(1, 200) if lo <= k * f0 < hi]
        odd = [harmonic_power(s, freqs, k * f0, f0 / 6) for k in ks if k % 2 == 1]
        even = [harmonic_power(s, freqs, k * f0, f0 / 6) for k in ks if k % 2 == 0]
        if not odd or not even or np.mean(even) <= 0:
            continue
        out.append(10 * np.log10(np.mean(odd) / np.mean(even) + 1e-30))
    return out


def main():
    fr = [(float(r['t']), float(r['f0'])) for r in csv.DictReader(open(sys.argv[1]))
          if r['voiced'] == '1' and float(r['f0']) > 0]
    for p in sys.argv[2:]:
        sr, x = load(p)
        print(p)
        for lo, hi in BANDS:
            y = sg.sosfiltfilt(sg.butter(6, [lo, hi], 'bandpass', fs=sr, output='sos'), x)
            env = np.abs(sg.hilbert(y))
            n = int(0.08 * sr)
            w = np.hanning(n)
            nfft = 1 << 16
            freqs = np.fft.rfftfreq(nfft, 1 / sr)
            a, b = [], []
            for t, f0 in fr[::2]:
                c = int(t * sr)
                if c - n // 2 < 0 or c + n // 2 >= len(env):
                    continue
                e = env[c - n // 2:c - n // 2 + n]
                if e.mean() < 1e-6:
                    continue
                s = np.abs(np.fft.rfft((e - e.mean()) * w, nfft)) ** 2
                a.append(peak(s, freqs, f0))
                b.append(peak(s, freqs, 2 * f0))
            oe = odd_even(x, sr, fr, lo, hi)
            oe_txt = f'   奇数倍 中央値 {np.median(oe):+6.1f}dB 90% 点 {np.percentile(oe, 90):+6.1f}dB (フレーム {len(oe)})' if oe else ''
            print(f'  {lo:>5}-{hi:<5}Hz  地声 f0 {np.mean(a):+5.1f}dB   2f0 {np.mean(b):+5.1f}dB  (フレーム {len(a)}){oe_txt}')


if __name__ == '__main__':
    main()
