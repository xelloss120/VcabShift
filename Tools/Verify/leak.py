# 地声の貫通の確かめ（2026-11-11）。
# 帯域に分けた出力の包絡（強弱）のスペクトルで、入力の基本周波数 f0（地声の高さ）の所の山が、
# 周り（0.7〜0.85 倍と 1.15〜1.3 倍）より何 dB 高いかを、有声のフレームごとに求めて平均する。
# 地声の成分がなければ 0dB 以下（入れ替えなしの初期値で 4kHz より上は -2〜-3dB）。入力そのものは約 +13dB。
# 出力の高さ（+12 半音なら 2f0）の山も並べて出す（入れ替えなしで約 +12dB）。
# 高い帯域の入れ替えの最初の形（無声の流れを使っていた）は、8〜12kHz で +9.4dB だった。
# 使い方: python leak.py frames.csv out.wav [out2.wav ...]
#   frames.csv は vcabsh_render の --frames で書いたもの（入力の高さ）。out.wav は --align で書いたもの。
import csv
import sys

import numpy as np
import scipy.io.wavfile as wf
import scipy.signal as sg


def load(p):
    sr, x = wf.read(p)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(1)
    if np.abs(x).max() > 2:
        x /= 32768.0
    return sr, x


def peak(spec, freqs, f):
    at = spec[np.argmin(np.abs(freqs - f))]
    m = ((freqs > 0.7 * f) & (freqs < 0.85 * f)) | ((freqs > 1.15 * f) & (freqs < 1.3 * f))
    return 10 * np.log10(at / (np.median(spec[m]) + 1e-30) + 1e-30)


def main():
    fr = [(float(r['t']), float(r['f0'])) for r in csv.DictReader(open(sys.argv[1]))
          if r['voiced'] == '1' and float(r['f0']) > 0]
    for p in sys.argv[2:]:
        sr, x = load(p)
        print(p)
        for lo, hi in ((2000, 4000), (4000, 8000), (8000, 12000)):
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
            print(f'  {lo:>5}-{hi:<5}Hz  地声 f0 {np.mean(a):+5.1f}dB   2f0 {np.mean(b):+5.1f}dB  (フレーム {len(a)})')


if __name__ == '__main__':
    main()
