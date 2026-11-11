# 有声・無声の判定の確かめ。
#   python Tools/Verify/voicing.py [--voice 録音.wav]
# 1. 雑音だけの音（低い雑音、ファンの音、息、摩擦音、ピンク雑音）を、有声と判定した割合（手がかりの有無）。
# 2. きれいに立ち上がる合成の母音で、声の始まりから有声と判定するまでの時間（今の版と、先読み 63）。
# 3. --voice: 録音した声で、先読み 63・手がかりなしの判定と比べた、有声の区間の始まりと終わりのずれ（フレーム）。
import numpy as np
import scipy.signal as ss

import common as C


def segments(v):
    out, i = [], 0
    while i < len(v):
        if v[i]:
            j = i
            while j < len(v) and v[j]:
                j += 1
            out.append((i, j)); i = j
        else:
            i += 1
    return out


def main():
    a = C.args_with_voice('有声・無声の判定の確かめ').parse_args()
    rng = np.random.default_rng(11)
    n = 2 * C.FS
    t = np.arange(n) / C.FS

    def at_db(y, db):
        return y / np.sqrt(np.mean(y ** 2)) * 10 ** (db / 20)
    pink = ss.lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], rng.standard_normal(n))
    noises = {
        '低い雑音（ブラウン）-30dB': at_db(ss.lfilter([1], [1, -0.999], rng.standard_normal(n)), -30),
        'ファンの音（56Hz と半分）-45dB': at_db(np.sin(2 * np.pi * 55.7 * t) + 0.5 * np.sin(2 * np.pi * 27.8 * t)
                                          + 0.3 * ss.lfilter(*ss.butter(2, 300 / 24000), rng.standard_normal(n)), -45),
        '息（300Hz〜6kHz）-30dB': at_db(ss.lfilter(*ss.butter(2, [300 / 24000, 6000 / 24000], 'band'), rng.standard_normal(n)), -30),
        '摩擦音（3kHz 以上）-25dB': at_db(ss.lfilter(*ss.butter(4, 3000 / 24000, 'high'), rng.standard_normal(n)), -25),
        'ピンク雑音 -30dB': at_db(pink, -30),
    }
    print('== 1. 雑音だけの音を有声と判定した割合')
    for name, y in noises.items():
        inp = C.path('voicing_noise.wav'); C.write(inp, np.r_[np.zeros(C.FS // 5), y])
        _, f1 = C.frames_of(inp, extra=('--no-cues',))
        _, f2 = C.frames_of(inp)
        print(f'   {name:24s} 手がかりなし {np.mean(f1 > 0) * 100:5.1f}%  あり {np.mean(f2 > 0) * 100:5.1f}%')

    print('== 2. 声の始まりから、有声と判定した最初のフレームまで（ms、母音 3 つの平均）')
    for ramp in (0.005, 0.015, 0.04):
        for f0 in (110, 170, 240):
            b = C.Builder(8).gap(0.15)
            starts = []
            for v in 'aio':
                starts.append(b.pos / C.FS)
                b.voice(f0, 0.3, v, ramp=ramp).gap(0.15)
            x, _, _ = b.done()
            inp = C.path('voicing_onset.wav'); C.write(inp, x)
            res = []
            for extra in ((), C.REFERENCE):
                tt, f = C.frames_of(inp, extra=extra)
                dl = []
                for s in starts:
                    m = (tt > s - 0.05) & (tt < s + 0.15) & (f > 0)
                    dl.append((tt[m].min() - s) * 1000 if m.any() else np.nan)
                res.append(np.mean(dl))
            print(f'   立ち上がり {ramp * 1000:3.0f}ms {f0}Hz: 今 {res[0]:5.1f}  先読み 63 {res[1]:5.1f}')

    if a.voice:
        print('== 3. 録音した声: 先読み 63・手がかりなしと比べた、有声の区間の始まりと終わりのずれ（フレーム）')
        _, fr = C.frames_of(a.voice, extra=C.REFERENCE)
        for label, extra in (('手がかりなし', ('--no-cues',)), ('手がかりあり（今の版）', ())):
            _, f = C.frames_of(a.voice, extra=extra)
            k = min(len(f), len(fr))
            rv, cv = fr[:k] > 0, f[:k] > 0
            ds, de = [], []
            for s, e in segments(rv):
                ov = [(c, d) for c, d in segments(cv) if d > s and c < e]
                if ov:
                    ds.append(ov[0][0] - s); de.append(ov[-1][1] - e)
            print(f'   {label}: 区間 {len(segments(cv))}（基準 {len(segments(rv))}）  食い違い {np.sum(rv != cv)}'
                  f'（今だけ有声 {np.sum(cv & ~rv)}、基準だけ有声 {np.sum(rv & ~cv)}）')
            print(f'      始まり（+ は遅い）{sorted(ds)}')
            print(f'      終わり（+ は長い）{sorted(de)}')


if __name__ == '__main__':
    main()
