# 標本化周波数ごとの確かめ（44.1kHz、48kHz、96kHz）。
#   python Tools/Verify/samplerate.py [--voice 録音.wav]
# 合成の母音（120Hz、ビブラート 1%）を各標本化周波数に変換して、遅延、ピッチ +12 の高さ（狙い 240Hz）、
# 一律 +4 の写像の差（dB）、最初の印の位置（パルスの直後が 0）、ブロックの大きさによる出力の違いを出す。
# --voice: 録音した声でも、48kHz との有声・無声の違いと、高さの差を出す。
from fractions import Fraction

import numpy as np
import scipy.signal as ss

import common as C

RATES = (44100, 48000, 96000)
SEG = 0.7


def resample(x, fs_in, fs_out):
    f = Fraction(fs_out, fs_in)
    return ss.resample_poly(x, f.numerator, f.denominator)


def vowels():
    b = C.Builder(1)
    for v in C.VOWELS:
        b.voice(lambda t: 120 * (1 + 0.01 * np.sin(2 * np.pi * 5 * t)), SEG - 0.1, v, ramp=0.02).gap(0.1)
    return b.done()


def harmonics(x, fs, s, e, f0):
    f, P = ss.welch(x[int(s * fs):int(e * fs)], fs, nperseg=16384 * fs // 48000, window='blackmanharris')
    hs = np.arange(f0, 6000, f0)
    return hs, np.array([10 * np.log10(P[np.abs(f - h) < f0 * 0.3].max() + 1e-20) for h in hs])


def main():
    a = C.args_with_voice('標本化周波数ごとの確かめ').parse_args()
    x48, _, pulses48 = vowels()
    print('== 合成の母音')
    for fs in RATES:
        x = resample(x48, C.FS, fs)
        inp = C.path(f'sr_{fs}.wav'); C.write(inp, x, fs)
        out = C.path(f'sr_out_{fs}.wav'); msg = C.render(inp, out)
        _, y = C.read(out)
        f0s = []
        for i in range(5):
            seg = y[int((i * SEG + 0.1) * fs):int((i * SEG + 0.5) * fs)]; seg = seg - seg.mean()
            r = np.fft.irfft(np.abs(np.fft.rfft(seg, 2 * len(seg))) ** 2)[:len(seg)]
            lo, hi = fs // 1000, fs // 60
            k = lo + np.argmax(r[lo:hi]); p, q, s = r[k - 1], r[k], r[k + 1]
            f0s.append(fs / (k + 0.5 * (p - s) / (p - 2 * q + s)))
        out4 = C.path(f'sr_u4_{fs}.wav'); mp = C.path(f'sr_marks_{fs}.csv')
        C.render(inp, out4, ('pitch=0', 'formant=4', 'unvoiced_formant=4'), marks=mp)
        _, y4 = C.read(out4)
        errs = []
        for i, v in enumerate(C.VOWELS):
            s, e = i * SEG + 0.1, i * SEG + 0.5
            hs, ha = harmonics(x, fs, s, e, 120); _, hb = harmonics(y4, fs, s, e, 120)
            tgt = np.interp(np.log2(hs), np.log2(hs * 2 ** (4 / 12)), ha)
            m = (hs >= 200) & (hs <= 3800)
            d = (hb - tgt)[m]; d -= np.median(d); errs.append(np.sqrt(np.mean(d ** 2)))
        ph = C.mark_phase(C.load_csv(mp), pulses48 * fs / C.FS)
        outs = []
        for blk in (1, 7, 4096):
            o = C.path(f'sr_blk_{blk}.wav'); C.render(inp, o, ('f2_delta=3',), extra=('--block', str(blk))); outs.append(C.read(o)[1])
        diff = max(np.max(np.abs(outs[0] - z)) for z in outs[1:])
        print(f'   {fs}Hz: {msg}')
        print(f'      ピッチ +12: ' + ' '.join(f'{v:.1f}' for v in f0s) + f'  写像の差 {np.mean(errs):.2f}dB'
              f'  印の位置の中央値 {np.median(ph):.2f}  ブロックによる違い {diff:.1e}')

    if a.voice:
        print('== 録音した声: 48kHz との違い')
        fs0, v = C.read(a.voice)
        res = {}
        for fs in RATES:
            inp = C.path(f'sr_voice_{fs}.wav'); C.write(inp, resample(v, fs0, fs), fs)
            p = C.path('_frames.csv'); C.render(inp, C.path('_x.wav'), frames=p)
            res[fs] = C.load_csv(p)
        ref = res[48000]
        for fs in (44100, 96000):
            d = res[fs]
            t = d[:, 0]
            idx = np.clip(np.searchsorted(ref[:, 0], t), 1, len(ref) - 1)
            idx = np.where(np.abs(ref[idx - 1, 0] - t) < np.abs(ref[idx, 0] - t), idx - 1, idx)
            r = ref[idx]; m = (t > 0.1) & (t < ref[-1, 0] - 0.1)
            both = m & (d[:, 1] > 0) & (r[:, 1] > 0)
            st = np.abs(12 * np.log2(d[both, 2] / r[both, 2]))
            print(f'   {fs}Hz: 有声・無声が違う {np.mean((d[m, 1] > 0) != (r[m, 1] > 0)) * 100:.2f}%  高さの差が 0.5 半音超 {np.mean(st > 0.5) * 100:.2f}%')


if __name__ == '__main__':
    main()
