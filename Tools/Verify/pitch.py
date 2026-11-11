# ピッチ検出（pYIN）の確かめ。
#   python Tools/Verify/pitch.py [--voice 録音.wav] [--compare 別の vcabsh_render.exe]
# 1. 合成の声（正解の高さが分かる）の 5 つの場面: 速い抑揚、出だしの一つおき（周期 2T の谷が深い）、語尾のかすれ、
#    摩擦音を挟んだ母音、息まじり。見逃し・誤有声・1 半音超・オクターブ違い・跳び・入れ替わりの数を出す。
# 2. 出だしの一つおきから、正しい高さに戻るまでの時間（オクターブ跳びの確率ごと）。
# 3. --voice: 録音した声で、先読みの幅ごとに、本来の pYIN に近い形（先読み 63）とどれだけ一致するか。
# --compare: 別の版の vcabsh_render（--frames がなくてもよい。--track で比べる）も 1 の表に並べる。
import numpy as np

import common as C


def cases():
    out = {}
    b = C.Builder(1).gap(0.1)

    def glide(t):
        if t < 0.15: return 110
        if t < 0.27: return 110 * (260 / 110) ** ((t - 0.15) / 0.12)
        if t < 0.40: return 260
        if t < 0.48: return 260 * (120 / 260) ** ((t - 0.40) / 0.08)
        return 120
    out['速い抑揚 110→260→120Hz'] = b.voice(glide, 0.7).gap(0.1).done()
    b = C.Builder(2).gap(0.1)
    for v in 'aoi':
        b.voice(200, 0.35, v, amp=lambda i, t: 0.45 if (t < 0.04 and i % 2) else 1.0).gap(0.12)
    out['出だしの一つおき 200Hz'] = b.done()
    b = C.Builder(3).gap(0.1)
    for v in 'ao':
        b.voice(lambda t: 130 * (1 - 0.15 * t), 0.35, v)
        # かすれ: 周期 12.5ms のまわりで大きく揺れ（25%）、振幅も揺れて小さくなる（正解なし）
        b.voice(lambda tt: 1.0 / 0.0125, 0.25, v, jitter=0.25, amp=lambda i, tt: b.rng.uniform(0.3, 1.0) * (1 - tt / 0.3),
                ramp=0.001, level=0.15, truth=False)
        b.gap(0.12)
    out['語尾のかすれ'] = b.done()
    b = C.Builder(4).gap(0.1)
    for f0 in (120, 220):
        b.voice(f0, 0.25, 'a').noise(0.12, 0.05).gap(0.02).voice(f0 * 1.05, 0.25, 'i').gap(0.12)
    out['摩擦音を挟んだ母音'] = b.done()
    b = C.Builder(5).gap(0.1)
    for f0, v in ((150, 'a'), (300, 'i'), (95, 'o')):
        b.voice(lambda t, f=f0: f * (1 + 0.03 * np.sin(2 * np.pi * 4 * t)), 0.5, v, jitter=0.01).gap(0.12)
    x, tr, p = b.done()
    noise = b.rng.standard_normal(len(x)) * np.std(x[np.abs(x) > 0.01]) * 10 ** (-10 / 20) * (np.abs(x) > 0)
    out['息まじり（雑音 -10dB）'] = (x + noise, tr, p)
    return out


def metrics(t, f, truth):
    idx = np.clip((t * C.FS).astype(int), 0, len(truth) - 1)
    g = truth[idx]
    # 声の始まりと終わりから 10ms 以内は数えない
    edge = np.zeros(len(truth), bool)
    for c in np.where(np.diff((truth != 0).astype(int)) != 0)[0]:
        edge[max(0, c - 480):c + 480] = True
    e = edge[idx]
    tv, tu = (g > 0) & ~e, (g == 0) & ~e
    both = tv & (f > 0)
    st = np.abs(12 * np.log2(f[both] / g[both])) if both.any() else np.zeros(1)
    fv = f > 0
    jump = (fv[1:] & fv[:-1]) & (np.abs(12 * np.log2(np.maximum(f[1:], 1) / np.maximum(f[:-1], 1))) >= 7)
    cr = g < 0
    return {'見逃し': np.mean(f[tv] <= 0) * 100 if tv.any() else 0, '誤有声': np.mean(f[tu] > 0) * 100 if tu.any() else 0,
            '1半音超': np.mean(st > 1) * 100, 'オク違い': np.mean(st > 8) * 100, '跳び': int(jump.sum()),
            '入替': int(np.sum(fv[1:] != fv[:-1])), 'かすれ有声': np.mean(fv[cr]) * 100 if cr.any() else float('nan')}


def track_of_other(exe, inp, truth):
    """別の版（--frames がない）: --track の高さを使い、時刻のずれは一番合うように探す"""
    p = C.path('_track.csv')
    C.render(inp, C.path('_x.wav'), exe=exe, extra=('--track', p, '--block', '240'))
    d = C.load_csv(p)
    best = None
    for sh in np.arange(0.0, 0.03, 0.0005):
        m = metrics(d[:, 0] - sh, d[:, 1], truth)
        score = m['1半音超'] + m['見逃し'] + m['誤有声']
        if best is None or score < best[0]:
            best = (score, sh)
    return d[:, 0] - best[1], d[:, 1]


def main():
    ap = C.args_with_voice('ピッチ検出の確かめ')
    ap.add_argument('--compare', help='比べる別の版の vcabsh_render.exe')
    a = ap.parse_args()

    print('== 1. 合成の声（見逃し・誤有声・1 半音超・オクターブ違いは %。跳びは 7 半音以上、入替は有声・無声の入れ替わりの数）')
    keys = ['見逃し', '誤有声', '1半音超', 'オク違い', '跳び', '入替', 'かすれ有声']
    for name, (x, tr, _) in cases().items():
        inp = C.path('pitch_case.wav'); C.write(inp, x)
        print(f'-- {name}')
        rows = [('今の版', C.frames_of(inp)), ('オクターブ跳びなし', C.frames_of(inp, extra=('--octave-jump', '0'))),
                ('手がかりなし', C.frames_of(inp, extra=('--no-cues',)))]
        if a.compare:
            rows.append(('--compare の版', track_of_other(a.compare, inp, tr)))
        for label, (t, f) in rows:
            m = metrics(t, f, tr)
            print(f'   {label:18s} ' + '  '.join(f'{k} {m[k]:.1f}' if isinstance(m[k], float) else f'{k} {m[k]}' for k in keys))

    print('== 2. 出だしの一つおきから、正しい高さ（±1 半音）に戻るまでの時間 ms（母音 3 つの平均）')
    for f0 in (200, 150):
        for ratio, dur in ((0.45, 0.04), (0.7, 0.04), (0.7, 0.1)):
            b = C.Builder(6).gap(0.1)
            starts = []
            for v in 'aoi':
                starts.append(b.pos / C.FS)
                b.voice(f0, 0.35, v, amp=lambda i, t: ratio if (t < dur and i % 2) else 1.0).gap(0.12)
            x, _, _ = b.done()
            inp = C.path('pitch_onset.wav'); C.write(inp, x)
            row = []
            for oj in ('0', '0.001', '0.01'):
                t, f = C.frames_of(inp, extra=('--octave-jump', oj))
                rec = []
                for s in starts:
                    m = (t > s + dur) & (t < s + 0.3)
                    bad = np.where(np.abs(12 * np.log2(np.maximum(f[m], 1) / f0)) >= 1)[0]
                    rec.append((t[m][bad[-1]] - (s + dur)) * 1000 + 5 if len(bad) else 0)
                row.append(f'跳び確率 {oj}: {np.mean(rec):5.1f}')
            print(f'   {f0}Hz 強さ {ratio} 長さ {int(dur * 1000)}ms | ' + ' | '.join(row))

    if a.voice:
        print('== 3. 録音した声: 先読み 63・つなぎなし・手がかりなし（本来の pYIN に近い形）との食い違い')
        tr_, fr = C.frames_of(a.voice, extra=C.REFERENCE)
        for la in (0, 1, 2, 4, 8):
            t, f = C.frames_of(a.voice, extra=('--lookahead', str(la), '--no-cues'))
            n = min(len(t), len(tr_))
            g, f = fr[:n], f[:n]
            ok = (g > 0) & (f > 0)
            st = np.abs(12 * np.log2(f[ok] / g[ok]))
            print(f'   先読み {la}: 有声・無声 {np.mean((g > 0) != (f > 0)) * 100:.2f}%  1 半音超 {np.mean(st > 1) * 100:.2f}%')


if __name__ == '__main__':
    main()
