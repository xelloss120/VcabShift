# 印付けと出力の高さの確かめ。
#   python Tools/Verify/marks.py [--voice 録音.wav]
# 1. 合成の声で、印が声帯のパルスに対して周期のどこに置かれたか（きれい、周期の揺れ、息まじり）。
#    パルスから周期の 8% 以内の割合と、隣の印との位置の差（5% 超）の割合。
# 2. 高さがゆっくり上下する声で、出力の高さの動きが正解からどれだけ遅れるか（測り方そのものの遅れも出す）。
#    あわせて、出力の印と、使った元の印の時刻の差（先の音を使わないので、半周期が下限）。
# 3. --voice: 録音した声の、印の間隔の外れと相互相関の低い印、出力の高さが入力の +12 半音からずれる割合。
import numpy as np

import common as C


def phase_cases():
    out = {}
    for name, jit, snr in (('きれい', 0.0, None), ('周期の揺れ 3%', 0.03, None), ('息まじり 10dB', 0.01, 10), ('息まじり 5dB', 0.01, 5)):
        b = C.Builder(5).gap(0.1)
        for v in 'aio':
            for f0 in (110, 170, 240):
                b.voice(f0, 0.4, v, jitter=jit).gap(0.1)
        x, _, p = b.done()
        if snr:
            x = x + np.random.default_rng(9).standard_normal(len(x)) * np.std(x[np.abs(x) > 0.01]) * 10 ** (-snr / 20) * (np.abs(x) > 0)
        out[name] = (x, p)
    return out


def lag_ms(t, f, tt, tf, scale=1.0):
    """測った高さ (t, f) が、正解 (tt, tf) からどれだけ遅れているか（ms）と、残りのずれ（半音）"""
    m = (f > 0) & (t > 0.3) & (t < tt[-1] - 0.2)
    best = None
    for L in np.arange(-0.03, 0.03, 0.0005):
        e = np.mean((12 * np.log2(f[m] / (np.interp(t[m] - L, tt, tf) * scale))) ** 2)
        if best is None or e < best[0]:
            best = (e, L)
    return best[1] * 1000, np.sqrt(best[0])


def main():
    a = C.args_with_voice('印付けと出力の高さの確かめ').parse_args()

    print('== 1. 印の位置（パルスから周期の 8% 以内の割合、隣の印との位置の差が周期の 5% を超える割合）')
    for name, (x, p) in phase_cases().items():
        inp = C.path('marks_phase.wav'); C.write(inp, x)
        mp = C.path('marks.csv'); C.render(inp, C.path('_x.wav'), marks=mp)
        ph = C.mark_phase(C.load_csv(mp), p)
        ph = np.where(ph > 0.5, ph - 1, ph)
        d = np.abs((np.diff(ph) + 0.5) % 1 - 0.5)
        print(f'   {name:14s} 8% 以内 {np.mean(np.abs(ph) < 0.08) * 100:5.1f}%  位置の差 5% 超 {np.mean(d > 0.05) * 100:4.1f}%')

    print('== 2. 高さの動きの遅れ（120Hz を中心に ±3 半音、3Hz で上下）')
    b = C.Builder(7).gap(0.1).voice(lambda t: 120 * 2 ** (3 * np.sin(2 * np.pi * 3 * t) / 12), 2.0, 'a').gap(0.1)
    x, _, p = b.done()
    inp = C.path('marks_lag.wav'); C.write(inp, x)
    tt = (p[:-1] + p[1:]) / 2 / C.FS  # 正解は周期の真ん中に置く
    tf = C.FS / np.diff(p)
    t, f = C.frames_of(inp, extra=C.REFERENCE)
    base = lag_ms(t, f, tt, tf)
    print(f'   入力を測った高さ（測り方そのものの遅れ）: {base[0]:.1f}ms')
    t, f = C.frames_of(inp)
    print(f'   本番の検出: {lag_ms(t, f, tt, tf)[0]:.1f}ms')
    for label, s, sc in (('素通し', ('pitch=0', 'formant=0', 'unvoiced_formant=0'), 1), ('ピッチ +12', ('formant=0',), 2), ('初期値', (), 2)):
        out = C.path('marks_lag_out.wav'); gp = C.path('grains.csv')
        C.render(inp, out, s, grains=gp)
        t, f = C.frames_of(out, extra=C.REFERENCE)
        g = C.load_csv(gp)
        m = (g[:, 0] > 0.3 * C.FS) & (g[:, 0] < 1.9 * C.FS)
        dd = (g[m, 0] - g[m, 1]) / C.FS * 1000
        print(f'   出力（{label}）: 高さの遅れ {lag_ms(t, f, tt, tf, sc)[0] - base[0]:.1f}ms（測り方の分を引いた値）'
              f'  出力の印 − 元の印 平均 {dd.mean():.2f}ms（周期 {1000 / 120:.2f}ms）')

    if a.voice:
        print('== 3. 録音した声')
        mp = C.path('marks_voice.csv'); out = C.path('marks_voice_out.wav')
        C.render(a.voice, out, ('formant=0', 'unvoiced_formant=0'), marks=mp)
        m = C.load_csv(mp)
        v = m[m[:, 2] > 0]
        cont = v[:, 3] > -1.5
        r = np.r_[np.nan, np.diff(v[:, 0]) / v[1:, 1]]
        bad = cont & ((np.abs(r - 1) > 0.1) | (v[:, 3] < 0.7))
        print(f'   印: {cont.sum()} 個のうち、間隔が周期から 10% 以上外れるか相関 0.7 未満 {bad.sum()} 個')
        A_t, A_f = C.frames_of(a.voice, extra=C.REFERENCE)
        B_t, B_f = C.frames_of(out, extra=C.REFERENCE)
        n = min(len(A_f), len(B_f)) - 5
        best = None
        for L in range(0, 5):
            ok = (A_f[:n] > 0) & (B_f[L:L + n] > 0)
            dev = 12 * np.log2(B_f[L:L + n][ok] / A_f[:n][ok]) - 12
            if best is None or np.mean(np.abs(dev) > 0.5) < best[0]:
                best = (np.mean(np.abs(dev) > 0.5), L, dev)
        dev = best[2]
        print(f'   出力の高さ（時刻を {best[1] * 5}ms 合わせた後）: +12 半音から 0.5 半音超 {np.mean(np.abs(dev) > 0.5) * 100:.2f}%  1 半音超 {np.mean(np.abs(dev) > 1) * 100:.2f}%')


if __name__ == '__main__':
    main()
