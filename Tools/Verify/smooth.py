# なめらかさの確かめ（2026-11-11）。処理で「足された」細かい揺れを、基準と比べて測る。
#   基準: 合成の声では理想の出力（同じパルス列の各周期を 2 つに割り、共振を +6 半音にした声）、
#         録音した声では入力そのもの（細かい揺れは、高さと音色を除けば入力と同じであるべき）。
# 測るもの（どれも小さいほど、なめらか。50ms より遅い動きは差し引く）:
#   lev  音量の揺れ（dB）: 10ms ごとの音量の、基準との差の、50ms より速い揺れの実効値（ブツブツ、揺れ）
#   spec 音色の揺れ（dB）: 1/3 オクターブの帯域ごとの強さ（20ms）の、基準との差の、50ms より速い揺れ（ザラつき、ちらつき）
#   hf   高い帯域の揺れ（dB）: spec のうち 4kHz より上の帯域だけ
#   pit  高さの揺れ（セント）: 高さ（YIN、30ms）の、基準との差の、50ms より速い揺れ（ふらつき）
#   step 切り替わりの段差（dB）: 有声と無声の切り替わりの前後 20ms で、5ms ごとの音量の一番大きい跳びの、基準との差
#   ulev 無声の音量の揺れ（dB）: lev と同じものを、無声で落ち着いている所で（雑音どうしなので、0 にはならない）
# 使い方: python smooth.py --exe 名前=vcabsh_render.exe[|キー=値...] [--exe ...] [--voice 録音.wav --frames 録音のフレーム.csv] [--reps 8]
#   最初の --exe を基準の版にして、ほかの版との p 値（ウィルコクソン）と良くなった数 / 悪くなった数を出す。
import argparse
import csv
import os

import numpy as np
import scipy.signal as ss
from scipy.ndimage import uniform_filter1d

import common as C
import quality as Q

FS = C.FS
K6 = 2 ** (6 / 12)
LH = FS // 400            # 音量: 2.5ms ごと
SH = FS // 200            # 音色・高さ: 5ms ごと
BANDS = [250 * 2 ** (k / 3) for k in range(17)]  # 250Hz〜約 12.7kHz の 1/3 オクターブの中心


def db(x):
    return 10 * np.log10(np.maximum(x, 1e-14))


def level(y, win=0.010, hop=LH):
    e = uniform_filter1d(y ** 2, int(win * FS))
    return db(e[::hop])


def bands(y):
    f, t, Z = ss.stft(y, FS, nperseg=FS // 50, noverlap=FS // 50 - SH, boundary=None, padded=False)
    P = np.abs(Z) ** 2
    out = []
    for c in BANDS:
        m = (f >= c / 2 ** (1 / 6)) & (f < c * 2 ** (1 / 6))
        out.append(db(P[m].sum(0)))
    # stft のフレーム k の中心は k*SH + 窓の半分。5ms ごとの格子にそろえる
    B = np.array(out)
    off = (FS // 50) // 2 // SH
    return np.pad(B, ((0, 0), (off, 0)), mode='edge')


def fast(d, hop):
    """50ms より遅い動きを差し引いた残り"""
    return d - uniform_filter1d(d, max(1, int(0.05 * FS / hop)), axis=-1, mode='nearest')


def rms(v):
    v = v[np.isfinite(v)]
    return float(np.sqrt(np.mean(v ** 2))) if len(v) else np.nan


def measure(y, ref, voiced, bounds, ref_f0=None, unvoiced=None):
    """voiced / unvoiced: 標本ごとの、基準が有声 / 無声で落ち着いている所（bool）。bounds: 有声・無声の切り替わりの標本の位置"""
    n = min(len(y), len(ref))
    y, ref, voiced = y[:n], ref[:n], voiced[:n]
    r = {}
    Ly, Lr = level(y), level(ref)
    mv = voiced[::LH][:len(Ly)] & (Lr > Lr.max() - 40)
    r['lev'] = rms(fast(Ly - Lr, LH)[mv])
    r['ulev'] = np.nan
    if unvoiced is not None:
        mu = unvoiced[:n][::LH][:len(Ly)] & (Lr > Lr.max() - 50)
        if mu.sum() > 8:
            r['ulev'] = rms(fast(Ly - Lr, LH)[mu])

    By, Br = bands(y), bands(ref)
    k = min(By.shape[1], Br.shape[1])
    By, Br = By[:, :k], Br[:, :k]
    ms = voiced[::SH][:k]
    live = Br > Br.max() - 50
    d = fast(By - Br, SH)
    sel = live & ms[None, :]
    r['spec'] = rms(d[sel])
    hi = np.array([c >= 4000 for c in BANDS])
    r['hf'] = rms(d[hi][sel[hi]])

    fy, dy = Q.yin(y, 150, 900, win=0.03)
    if ref_f0 is None:
        fr, dr = Q.yin(ref, 150, 900, win=0.03)
    else:
        fr, dr = ref_f0
    k = min(len(fy), len(fr))
    cy = 1200 * np.log2(np.maximum(fy[:k], 1) / 440)
    cr = 1200 * np.log2(np.maximum(fr[:k], 1) / 440)
    ok = (fy[:k] > 0) & (fr[:k] > 0) & (dy[:k] < 0.3) & (dr[:k] < 0.3) & voiced[::SH][:k] & (np.abs(cy - cr) < 100)
    dd = np.where(ok, cy - cr, np.nan)
    # 抜けがある所はつなげずに、続いている区間ごとに差し引く
    res = []
    idx = np.flatnonzero(np.diff(np.r_[0, ok.astype(int), 0]))
    for s, e in zip(idx[::2], idx[1::2]):
        if e - s >= 6:
            res.append(fast(dd[s:e], SH))
    r['pit'] = rms(np.concatenate(res)) if res else np.nan

    Sy, Sr = level(y, 0.005, FS // 1000), level(ref, 0.005, FS // 1000)
    st = []
    for b in bounds:
        a, e = (b - int(0.02 * FS)) // (FS // 1000), (b + int(0.02 * FS)) // (FS // 1000)
        if a < 5 or e + 5 >= min(len(Sy), len(Sr)):
            continue
        jy = np.max(np.abs(Sy[a + 5:e] - Sy[a:e - 5]))
        jr = np.max(np.abs(Sr[a + 5:e] - Sr[a:e - 5]))
        st.append(jy - jr)
    r['step'] = float(np.mean(st)) if st else np.nan
    return r


# ---- 合成の声と理想の出力 ----

def pulses_to_signal(pos, amp, n):
    """小数の位置にパルスを置く（窓をかけた sinc、16 点）"""
    p = np.zeros(n)
    for t, a in zip(pos, amp):
        i0 = int(np.floor(t))
        k = np.arange(i0 - 7, i0 + 9)
        ok = (k >= 0) & (k < n)
        x = k - t
        p[k[ok]] += a * (np.sinc(x) * (0.5 + 0.5 * np.cos(np.pi * x / 8)))[ok]
    return p


def glottal(f0fun, dur, rng, jitter=0.005, shimmer=0.05):
    pos, amp = [], []
    t = 0.0
    while t < dur:
        T = 1 / f0fun(t) * (1 + jitter * rng.standard_normal())
        pos.append(t * FS)
        amp.append(1 + shimmer * rng.standard_normal())
        t += T
    return np.array(pos), np.array(amp)


def ideal_pulses(pos, amp):
    """各周期を 2 つに割る（+12 半音の理想）"""
    mid = (pos[:-1] + pos[1:]) / 2
    p2 = np.sort(np.r_[pos, mid])
    a2 = np.r_[amp, amp[:-1]][np.argsort(np.r_[pos, mid], kind='stable')]
    return p2, a2


def voiced_seg(d, rng, f0fun, v1, v2=None, jitter=0.005, shimmer=0.05, fade=None):
    n = int(d * FS)
    pos, amp = glottal(f0fun, d, rng, jitter, shimmer)
    if fade is not None:
        amp = amp * fade(pos / FS)
    p, p2 = pulses_to_signal(pos, amp, n), pulses_to_signal(*ideal_pulses(pos, amp), n)
    if v2 is None:
        x, y = C.shape(p, v1), Q.shape_k(p2, v1, K6)
    else:
        w = np.clip((np.arange(n) / FS - d / 2) / 0.12 + 0.5, 0, 1)  # 真ん中の 120ms で移る
        x = C.shape(p, v1) * (1 - w) + C.shape(p, v2) * w
        y = Q.shape_k(p2, v1, K6) * (1 - w) + Q.shape_k(p2, v2, K6) * w
    e = Q.env(n)
    return x * e, y * e


SCENES = ['伸ばした母音', '母音の移り変わり', '速い抑揚', '息まじり', '母音→摩擦→母音', '語尾のかすれ']


def scene(name, rng):
    """(入力, 理想, 有声で落ち着いている所, 切り替わりの位置, 無声で落ち着いている所)"""
    f0 = rng.uniform(100, 170)
    v1, v2 = rng.choice(list('aiueo'), 2, replace=False)
    xs, ys, vs, bs = [], [], [], []
    pos = [0]

    def add(x, y, v, u=None):
        xs.append(x); ys.append(y); vs.append(v); pos[0] += len(x)
        us.append(np.zeros(len(x), bool) if u is None else u)

    def gap(d):
        n = int(d * FS); add(np.zeros(n), np.zeros(n), np.zeros(n, bool))

    us = []

    def steady(n):
        v = np.zeros(n, bool); g = int(0.03 * FS); v[g:n - g] = True
        return v

    gap(0.1)
    for _ in range(2):
        if name == SCENES[0]:
            x, y = voiced_seg(0.6, rng, lambda t: f0, v1); add(x, y, steady(len(x)))
        elif name == SCENES[1]:
            x, y = voiced_seg(0.6, rng, lambda t: f0, v1, v2); add(x, y, steady(len(x)))
        elif name == SCENES[2]:
            x, y = voiced_seg(0.6, rng, lambda t: f0 * 2 ** (np.sin(2 * np.pi * t / 0.5) * 0.35), v1)
            add(x, y, steady(len(x)))
        elif name == SCENES[3]:
            x, y = voiced_seg(0.6, rng, lambda t: f0, v1)
            nz = Q.noise(len(x), rng, 1000) * 0.25 * np.std(x[len(x) // 4: -len(x) // 4]) * Q.env(len(x))
            add(x + nz, y / np.std(y[len(y) // 4: -len(y) // 4]) * np.std(x[len(x) // 4: -len(x) // 4]) + nz,
                steady(len(x)))
        elif name == SCENES[4]:
            x, y = voiced_seg(0.25, rng, lambda t: f0, v1); add(x, y, steady(len(x))); bs.append(pos[0])
            u = Q.noise(int(0.1 * FS), rng, rng.choice([2500, 3500])) * 0.03 * Q.env(int(0.1 * FS), 0.01, 0.01)
            uu = np.zeros(len(u), bool); uu[int(0.02 * FS):-int(0.02 * FS)] = True
            add(u, u, np.zeros(len(u), bool), uu); bs.append(pos[0])
            x, y = voiced_seg(0.25, rng, lambda t: f0, v2); add(x, y, steady(len(x)))
        else:
            x, y = voiced_seg(0.5, rng, lambda t: f0 * (1 - 0.25 * t), v1, jitter=0.02, shimmer=0.2,
                              fade=lambda t: np.clip(1 - t / 0.5, 0.05, 1) ** 2)
            add(x, y, steady(len(x)))
        gap(0.1)
    x, y, v, u = (np.concatenate(a) for a in (xs, ys, vs, us))
    s = 0.25 / (np.max(np.abs(x)) + 1e-12)
    return x * s, y * s, v, np.array(bs, int), u


# ---- 録音した声 ----

def voice_mask(frames_csv, n):
    """入力の有声のフレーム（前後 30ms を除く）と、有声・無声の切り替わりの位置"""
    rows = list(csv.DictReader(open(frames_csv)))
    t = np.array([float(r['t']) for r in rows])
    v = np.array([r['voiced'] == '1' for r in rows])
    m = np.zeros(n, bool)
    um = np.zeros(n, bool)
    for k in range(len(v)):
        a = int(t[k] * FS) - SH // 2
        if v[k] and all(v[max(0, k - 6):k + 7]):
            m[max(0, a):max(0, a + SH)] = True
        if not v[k] and not any(v[max(0, k - 4):k + 5]):
            um[max(0, a):max(0, a + SH)] = True
    ch = np.flatnonzero(np.diff(v.astype(int)))
    return m, (t[ch + 1] * FS).astype(int), um


def report(title, keys, R, labs):
    print(f'\n### {title}')
    print('  ' + ' ' * 22 + ''.join(f'{l:>14s}' for l in labs))
    names = {'lev': '音量の揺れ dB', 'spec': '音色の揺れ dB', 'hf': '高い帯域の揺れ dB', 'pit': '高さの揺れ セント', 'step': '切り替わりの段差 dB', 'ulev': '無声の音量の揺れ dB'}
    for key in keys:
        cols = [[d[key] for d in R[l]] for l in labs]
        if np.all(np.isnan(cols[0])):
            continue
        line = f'  {names[key]:20s}' + ''.join(f'{np.nanmean(c):14.2f}' for c in cols)
        marks = []
        for c in cols[1:]:
            p, good, bad = Q.paired(cols[0], c)
            marks.append(f'{"良" if good > bad else "悪" if bad > good else "="}{good}/{bad} p={p:.2f}')
        print(line + '   ' + '  '.join(marks))


def main():
    ap = argparse.ArgumentParser(description='なめらかさの確かめ')
    ap.add_argument('--exe', action='append', required=True, help='名前=vcabsh_render.exe[|キー=値...][|@環境変数=値...]（最初が基準）')
    ap.add_argument('--voice')
    ap.add_argument('--frames', help='録音した声の、決まったフレームの CSV（--frames で書いたもの）')
    ap.add_argument('--reps', type=int, default=8)
    a = ap.parse_args()
    # 名前=exe|キー=値|キー=値（設定は省略できる）
    exes, sets = {}, {}
    for e in a.exe:
        name, rest = e.split('=', 1)
        parts = rest.split('|')
        exes[name], sets[name] = parts[0], tuple(parts[1:])

    def render(l, x, tag):
        # 「@名前=値」は環境変数として渡す（開発用の切り替え）
        env = [kv[1:].split('=', 1) for kv in sets[l] if kv.startswith('@')]
        keep = {k: os.environ.get(k) for k, _ in env}
        os.environ.update(dict(env))
        try:
            return Q.run(exes[l], x, tuple(kv for kv in sets[l] if not kv.startswith('@')), tag, float_in=True)[0]
        finally:
            for k, v in keep.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    labs = list(exes)
    keys = ['lev', 'spec', 'hf', 'pit', 'step']
    print('（値は基準より足された揺れ。小さいほどなめらか。右の印は最初の版と比べて 良/悪 の数と p 値）')

    allR = {l: [] for l in labs}
    for si, sc in enumerate(SCENES):
        R = {l: [] for l in labs}
        for r in range(a.reps):
            rng = np.random.default_rng(500 + 31 * r + 1000 * si)
            x, ideal, v, b, u = scene(sc, rng)
            room = Q.noise(len(x), rng, 50) * 10 ** (-45 / 20) * 0.5
            x, ideal = x + room, ideal + room
            for l, exe in exes.items():
                y = render(l, x, 'sm')
                m = measure(y, ideal, v, b, unvoiced=u)
                R[l].append(m)
                allR[l].append(m)
        report(f'合成の声: {sc}（{a.reps} 通り）', keys + ['ulev'] if sc == SCENES[4] else keys[:4], R, labs)
    report(f'合成の声: 全部の場面（{a.reps * len(SCENES)} 通り）', keys[:4], allR, labs)

    if a.voice and a.frames:
        fs, x = C.read(a.voice)
        assert fs == FS
        m, b, um = voice_mask(a.frames, len(x))
        fr, dr = Q.yin(x, 75, 450, win=0.03)
        ref_f0 = (fr * 2, dr)
        outs = {l: render(l, x, 'smv') for l in labs}
        R = {l: [] for l in labs}
        sec = FS
        for s in range(0, len(x) - sec, sec):
            e = s + sec
            if m[s:e].sum() < 0.2 * FS:
                continue
            bb = b[(b >= s) & (b < e)] - s
            f0c = (ref_f0[0][s // SH:e // SH], ref_f0[1][s // SH:e // SH])
            for l in labs:
                R[l].append(measure(outs[l][s:e], x[s:e], m[s:e], bb, f0c, um[s:e]))
        report(f'録音した声（1 秒ごと、{len(R[labs[0]])} 区間。基準は入力）', keys + ['ulev'], R, labs)


if __name__ == '__main__':
    main()
