# 出力の音の質の確かめ（2026-11-10 のチリパリの調べで使ったもの）。処理ツールの中の判定に頼らず、出力の音を測る。
#   python Tools/Verify/quality.py [--compare 別の vcabsh_render.exe] [--reps n] [--only harmonics,pulses,scenes,blocks]
#            [--voice 録音.wav]
# 1. harmonics: 周期が小数の合成の母音で、倍音の間の強さ − 倍音の強さ（帯域ごと）を、理想（合成し直した声）と並べる。
#    高い音を足した声を 32 ビット浮動小数で書く（16 ビットだと 8kHz より上は量子化の雑音しか残らない）。
# 2. pulses: 4kHz より上の鋭い山（1ms の山が 20ms の実効値の 4 倍超）の数を、理想と並べる（声帯のパルスも数えるので、理想との差を見る）。
# 3. scenes: 合成の声の 8 場面（部屋の雑音 -45dB を足す）で、プチッ・強さの飛び出し・有声無声の誤り・高さのずれ・出力の周期の乱れ。
#    --compare があれば、同じ入力で比べて p 値（ウィルコクソン）と良くなった数 / 悪くなった数を出す。
# 4. blocks: ブロックの大きさごとの、処理時間の最大と 99.9% 点（--voice の録音、なければ合成の声）。
# 5. pitch: 合成の声の場面で、出力の断片の間隔（--grains の周期）から出力の高さを正確に求め、正解と比べる。
#    使った元の印の時刻の正解と比べたもの（検出の誤り）と、出力の時刻の正解と比べたもの（使う印が古い分の遅れも含む）。
#    YIN（窓 40ms）で出力を測ると、速い抑揚では窓の遅れで誤りが大きく出る（2026-11-10。33% に対して、この測り方では 9%）。
import argparse
import os
import re
import subprocess

import numpy as np
import scipy.io.wavfile as wf
import scipy.signal as ss
from scipy.ndimage import binary_erosion, label, maximum_filter1d, uniform_filter1d
from scipy.stats import wilcoxon

import common as C

FS = C.FS
HOP = FS // 200  # 5ms
K6 = 2 ** (6 / 12)


# ---- 共通 ----

def write_float(p, x):
    wf.write(p, FS, x.astype(np.float32))


def run(exe, x, settings=(), name='q', extra=(), float_in=False):
    i = C.path(f'{name}_in.wav'); o = C.path(f'{name}_out.wav')
    (write_float if float_in else C.write)(i, x)
    msg = C.render(i, o, settings, exe=exe, extra=extra)
    return C.read(o)[1][:len(x)], msg


def shape_k(p, v, k=1.0):
    """common.shape と同じ声で、共振の周波数と幅を k 倍したもの（フォルマントを動かした理想の声）"""
    y = ss.lfilter([1], [1, -0.97], ss.lfilter([1], [1, -0.97], p))
    y = ss.lfilter([1, -1], [1], y)
    for F, B in zip(C.VOWELS[v], C.BANDWIDTHS):
        F, B = F * k, B * k
        if F < FS / 2 * 0.95:
            r = np.exp(-np.pi * B / FS); th = 2 * np.pi * F / FS
            y = ss.lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], y)
    return y


def env(n, a=0.015, b=0.015):
    t = np.arange(n) / FS
    return np.clip(np.minimum(t / a, (n / FS - t) / b), 0, 1)


def noise(n, rng, lo, hi=None):
    b = ss.butter(4, lo / (FS / 2), 'high') if hi is None else ss.butter(4, [lo / (FS / 2), hi / (FS / 2)], 'band')
    y = ss.lfilter(*b, rng.standard_normal(n))
    return y / np.std(y)


def spikes(y, m):
    """4kHz より上の、1ms の山が 20ms の実効値の 4 倍を超える回数（1 秒あたり）"""
    h = ss.lfilter(*ss.butter(4, 4000 / (FS / 2), 'high'), y)
    pk = maximum_filter1d(np.abs(h), FS // 1000)
    rms = np.sqrt(np.maximum(uniform_filter1d(h ** 2, FS // 50), 0) + 1e-20)
    hit = ((pk > 4.0 * rms) & m & (rms > 1e-5))[:: FS // 1000]
    return np.sum(hit[1:] & ~hit[:-1]) / (m.sum() / FS + 1e-9)


def jumps(y, m):
    """2kHz より上の 2.5ms ごとの強さが、前後の平均から 6dB 以上飛び出す回数（1 秒あたり）"""
    h = ss.lfilter(*ss.butter(4, 2000 / (FS / 2), 'high'), y)
    st = FS // 400
    e = 10 * np.log10(np.maximum(uniform_filter1d(h ** 2, st), 0)[::st] + 1e-14)
    mm = m[::st][:len(e)]
    hit = (np.abs(e - uniform_filter1d(e, 9)) > 6) & mm & (e > -80)
    return np.sum(hit[1:] & ~hit[:-1]) / (mm.sum() * st / FS + 1e-9)


def loud_mask(x, db=-50):
    return np.sqrt(np.maximum(uniform_filter1d(x ** 2, FS // 100), 0)) > 10 ** (db / 20)


def yin(x, fmin, fmax, win=0.04):
    """オフラインの YIN（5ms ごと、約 16kHz で）。(高さ, 谷の深さ)"""
    d = ss.resample_poly(x, 1, 3); fs = FS / 3; hop = HOP // 3
    n = int(win * fs); tmin = int(fs / fmax); tmax = int(fs / fmin) + 1
    nf = len(x) // HOP
    f0 = np.zeros(nf); dm = np.ones(nf)
    pad = np.concatenate([np.zeros(n + tmax), d, np.zeros(n + tmax)])
    for k in range(nf):
        c = k * hop + n + tmax
        seg = pad[c - (n + tmax) // 2: c + (n + tmax) // 2 + 1]
        a = seg[:n]
        if np.sqrt(np.mean(a ** 2)) < 10 ** (-55 / 20):
            continue
        L = len(seg)
        e = np.cumsum(np.concatenate([[0], seg ** 2]))
        cc = np.fft.irfft(np.fft.rfft(seg, 2 * L) * np.conj(np.fft.rfft(a, 2 * L)), 2 * L)[:tmax + 1]
        tau = np.arange(1, tmax + 1)
        df = np.concatenate([[0], e[n] - e[0] + e[tau + n] - e[tau] - 2 * cc[1:]])
        cm = np.concatenate([[1], df[1:] * tau / (np.cumsum(df[1:]) + 1e-20)])
        rng = cm[tmin:tmax]
        below = np.where(rng < 0.15)[0]
        if len(below):
            i = below[0]
            while i + 1 < len(rng) and rng[i + 1] < rng[i]:
                i += 1
        else:
            i = int(np.argmin(rng))
        t = i + tmin
        if 1 <= t < tmax:
            y0, y1, y2 = cm[t - 1], cm[t], cm[t + 1]
            den = y0 - 2 * y1 + y2
            t = t + (0.5 * (y0 - y2) / den if abs(den) > 1e-12 else 0)
        f0[k] = fs / t; dm[k] = cm[i + tmin]
    return f0, dm


def paired(a, b):
    """1 つ目 → 2 つ目の p 値と、小さくなった数 / 大きくなった数"""
    a, b = np.asarray(a, float), np.asarray(b, float)
    ok = ~np.isnan(a) & ~np.isnan(b)
    a, b = a[ok], b[ok]
    try:
        p = wilcoxon(a, b).pvalue if np.any(a != b) else 1.0
    except ValueError:
        p = 1.0
    return p, int(np.sum(b < a)), int(np.sum(b > a))


# ---- 1. 倍音の間の雑音 ----

BANDS = ((500, 2000), (2000, 4000), (4000, 8000), (8000, 12000), (12000, 16000))


def harmonic_voice(period, mul, k, v):
    """周期 period 標本（小数）の声。パルスは帯域を制限して小数の位置に置き、F4 より上の高い音の枝も足す"""
    n = int(0.8 * FS); T = period / mul
    p = np.zeros(n); t = 0.0
    while t < n - 1:
        i0 = int(np.floor(t)); fr = t - i0
        j = np.arange(-24, 25); idx = i0 + j; ok = (idx >= 0) & (idx < n)
        p[idx[ok]] += (np.sinc(j - fr) * (0.5 + 0.5 * np.cos(np.pi * (j - fr) / 25)))[ok]
        t += T
    y = shape_k(p, v, k)
    hf = ss.lfilter(*ss.butter(2, 3000 / (FS / 2), 'high'), ss.lfilter([1], [1, -0.9], np.diff(p, prepend=0)))
    y = y / np.max(np.abs(y)) + hf / np.max(np.abs(hf)) * 0.004
    y = y / np.max(np.abs(y)) * 0.25 * env(n)
    return np.concatenate([np.zeros(FS // 5), y, np.zeros(FS // 5)])


def inharmonic(y, f0):
    a, b = FS // 5 + FS // 5, FS // 5 + int(0.7 * FS)
    P = np.abs(np.fft.rfft(y[a:b] * np.hanning(b - a), 1 << 20)) ** 2
    f = np.arange(len(P)) * FS / (1 << 20)
    d = np.abs(f / f0 - np.round(f / f0))
    return np.array([10 * np.log10(P[(f >= lo) & (f < hi) & (d > 0.25)].mean() / P[(f >= lo) & (f < hi) & (d < 0.08)].mean())
                     for lo, hi in BANDS])


def harmonics(exes):
    print('\n### 1. 倍音の間の強さ − 倍音の強さ（dB。周期が小数の母音 8 通りの平均。小さいほど澄んでいる）')
    print(f'  {"":24s}{"":14s}' + ''.join(f'{f"{lo / 1000:g}-{hi / 1000:g}k":>9s}' for lo, hi in BANDS))
    periods = [400.37, 360.71, 320.23, 300.55, 281.4, 341.9, 377.3, 303.3]
    for name, s, mul, k in (('素通し', ('pitch=0', 'formant=0', 'unvoiced_formant=0'), 1, 1.0),
                            ('ピッチ +12 だけ', ('pitch=12', 'formant=0', 'unvoiced_formant=0'), 2, 1.0),
                            ('初期値（+12 / +6）', (), 2, K6)):
        rows = {lab: [] for lab in ['理想'] + list(exes)}
        for r, per in enumerate(periods):
            v = 'aiueo'[r % 5]
            x = harmonic_voice(per, 1, 1.0, v)
            f0 = FS / per * mul
            rows['理想'].append(inharmonic(harmonic_voice(per, mul, k, v), f0))
            for lab, exe in exes.items():
                rows[lab].append(inharmonic(run(exe, x, s, 'h', float_in=True)[0], f0))
        for lab, v in rows.items():
            print(f'  {name if lab == "理想" else "":24s}{lab:14s}' + ''.join(f'{m:9.1f}' for m in np.mean(v, 0)))


# ---- 2. 鋭い山の数（理想との比べ） ----

def pulses(exes, reps):
    print(f'\n### 2. 4kHz より上の鋭い山の数（1 秒あたり、{reps} 通りの平均。声帯のパルスも数えるので、理想との差を見る）')
    for mod, name in ((False, '一定の高さ'), (True, '速い抑揚')):
        R = {lab: [] for lab in ['理想'] + list(exes)}
        for r in range(reps):
            rng = np.random.default_rng(42 + r)
            f0 = rng.uniform(100, 170); v = 'aiueo'[r % 5]
            sig = []
            for mul, k in ((1, 1.0), (2, K6)):
                segs = [np.zeros(int(0.15 * FS))]
                for _ in range(2):
                    fun = (lambda t, m=mul: f0 * m * 2 ** (np.sin(2 * np.pi * t / 0.5) * 0.35)) if mod else (lambda t, m=mul: f0 * m)
                    n = int(0.5 * FS); p = np.zeros(n); t = 0.0
                    while t < 0.5:
                        i = int(round(t * FS))
                        if i < n:
                            p[i] += 1
                        t += 1 / fun(t)
                    segs += [C.norm(shape_k(p, v, k), 1) * 0.25 * env(n), np.zeros(int(0.08 * FS))]
                sig.append(np.concatenate(segs + [np.zeros(int(0.15 * FS))]))
            nz = noise(len(sig[0]), np.random.default_rng(4001 + r), 50) * 10 ** (-45 / 20) * 0.5
            x, ideal = sig[0] + nz, sig[1] + nz
            m = loud_mask(x)
            R['理想'].append(spikes(ideal, m))
            for lab, exe in exes.items():
                R[lab].append(spikes(run(exe, x, (), 'p')[0], m))
        print(f'  {name:10s} ' + '  '.join(f'{lab} {np.mean(v):5.1f}' for lab, v in R.items()))


# ---- 3. 8 つの場面 ----

SCENES = ['さ行（摩擦→母音、つなぎ目が重なる）', 'は行（息の音→母音）', '母音→摩擦→母音（つなぎ目に無音なし）', 'ざ行（有声の摩擦音）',
          '息まじりの母音', '語尾のかすれ（弱く、揺れが増す）', 'ガラガラ声（不規則な長短）', '速い抑揚']


def glottal(f0fun, dur, rng, jitter=0.005, shimmer=0.05, fry=False):
    n = int(dur * FS); p = np.zeros(n); tr = np.zeros(n); t = 0.0; k = 0
    while t < dur:
        T = 1 / f0fun(t) * (1 + jitter * rng.standard_normal())
        if fry:
            T *= 1.35 if k % 2 else 0.75
        a = (1 + shimmer * rng.standard_normal()) * (0.5 if fry and k % 2 else 1.0)
        i = int(round(t * FS))
        if i < n:
            p[i] += a
            tr[i:min(n, int(round((t + T) * FS)))] = 1 / T
        t += T; k += 1
    return p, tr


def scene(name, rng):
    """(音, 区画（V 有声・U 無声・M 混ざり・S 無音）, 正解の高さ)"""
    f0 = rng.uniform(100, 170)
    v1, v2 = rng.choice(list('aiueo')), rng.choice(list('aiueo'))
    parts = []

    def add(y, kind, tr=None):
        parts.append((y, np.full(len(y), kind), np.zeros(len(y)) if tr is None else tr))

    def vowel(d, f=None, v=None, level=0.25, **kw):
        p, tr = glottal(f or (lambda t: f0), d, rng, **kw)
        return C.norm(C.shape(p, v or v1), 1) * level, tr

    def V(d, f=None, v=None):
        y, tr = vowel(d, f, v); add(y * env(len(y)), 'V', tr)

    def U(d, lo, lvl, hi=None):
        y = noise(int(d * FS), rng, lo, hi) * lvl; add(y * env(len(y), 0.01, 0.01), 'U')

    def gap(d):
        add(np.zeros(int(d * FS)), 'S')

    gap(0.15)
    if name == SCENES[0]:
        for _ in range(3):
            lo = rng.choice([2500, 3500, 4500]); ov = rng.uniform(0.01, 0.04)
            n1, no, n2 = int(0.12 * FS), int(ov * FS), int(0.18 * FS)
            u = noise(n1 + no, rng, lo) * 0.04 * np.concatenate([env(n1, 0.02, 1e-3), np.linspace(1, 0, no)])
            y, tr = vowel(0.18)
            sig = np.concatenate([u, np.zeros(n2 - no)]) + np.concatenate([np.zeros(n1), y * env(n2)])
            kind = np.array(['U'] * n1 + ['M'] * no + ['V'] * (n2 - no))
            trr = np.concatenate([np.zeros(n1), tr]); trr[n1:n1 + no] = 0
            parts.append((sig, kind, trr)); gap(0.08)
    elif name == SCENES[1]:
        for _ in range(3):
            U(0.08, 800, 0.03, 6000); V(0.2); gap(0.08)
    elif name == SCENES[2]:
        for _ in range(3):
            V(0.18); U(0.1, rng.choice([2500, 3500]), 0.04); V(0.18, v=v2); gap(0.06)
    elif name == SCENES[3]:
        for _ in range(3):
            y, _ = vowel(0.12, v='i', level=0.08)
            add((y + noise(len(y), rng, 3000) * 0.03) * env(len(y)), 'M'); V(0.18); gap(0.08)
    elif name == SCENES[4]:
        for _ in range(2):
            y, tr = vowel(0.4)
            add((y + noise(len(y), rng, 1000) * 0.25 * np.std(y)) * env(len(y)), 'V', tr); gap(0.1)
    elif name == SCENES[5]:
        for _ in range(2):
            n = int(0.45 * FS)
            p, tr = glottal(lambda t: f0 * (1 - 0.25 * t), 0.45, rng, jitter=0.02, shimmer=0.2)
            fade = np.linspace(1, 0.05, n) ** 2
            y = C.norm(C.shape(p, v1), 1) * 0.25 * fade + noise(n, rng, 1000) * 0.004 * np.linspace(0.3, 1, n)
            parts.append((y * env(n), np.where(fade > 0.15, 'V', 'M'), tr * (fade > 0.15))); gap(0.1)
    elif name == SCENES[6]:
        for _ in range(2):
            p, _ = glottal(lambda t: f0 * 0.6, 0.35, rng, jitter=0.03, shimmer=0.2, fry=True)
            y = C.norm(C.shape(p, v1), 1) * 0.2
            add(y * env(len(y)), 'M'); V(0.2); gap(0.1)
    else:
        for _ in range(2):
            V(0.5, f=lambda t: f0 * 2 ** (np.sin(2 * np.pi * t / 0.5) * 0.35)); gap(0.08)
    gap(0.15)
    return tuple(np.concatenate([p[i] for p in parts]) for i in range(3))


def frames(exe, x, name):
    """--frames で決まったフレーム（新しい版）、なければ --track（古い版。ブロック 240 で 5ms ごと）"""
    i = C.path(f'{name}_in.wav'); o = C.path(f'{name}_out.wav'); f = C.path(f'{name}_f.csv')
    C.write(i, x)
    r = subprocess.run([exe, i, o, '--align', '--frames', f], capture_output=True, text=True)
    if 'unknown argument' in r.stdout:
        subprocess.run([exe, i, o, '--align', '--track', f, '--block', '240'], check=True, capture_output=True)
        d = C.load_csv(f); return d[:, 0], d[:, 1] > 0, C.read(o)[1][:len(x)]
    d = C.load_csv(f)
    return d[:, 0], d[:, 1] > 0, C.read(o)[1][:len(x)]


def evaluate(x, kind, tr, y, vt, vv):
    n = len(x); loud = loud_mask(x)
    mU = binary_erosion((kind == 'U') & loud, np.ones(FS // 200))
    tt = (vt * FS).astype(int); ok = (tt >= 0) & (tt < n); vv = vv[ok]; tt = tt[ok]; ll = loud[tt]
    isV, isU = (kind == 'V')[tt] & ll, (kind == 'U')[tt] & ll
    fo, do = yin(y, 120, 1000)
    fr = tr[::HOP][:len(fo)]
    kv = (kind == 'V')[::HOP][:len(fo)] & (fr > 0) & loud[::HOP][:len(fo)]
    err = 1200 * np.log2(np.maximum(fo, 1) / (2 * np.maximum(fr, 1)))
    return dict(spk=spikes(y, loud) - spikes(x, loud), spkU=spikes(y, mU) - spikes(x, mU) if mU.any() else np.nan,
                jmp=jumps(y, loud) - jumps(x, loud),
                miss=np.mean(~vv[isV]) * 100 if isV.any() else np.nan, false=np.mean(vv[isU]) * 100 if isU.any() else np.nan,
                pbad=np.mean(((np.abs(err) > 50) | (do > 0.3))[kv]) * 100 if kv.any() else np.nan,
                aper=np.mean(do[kv]) if kv.any() else np.nan)


LABELS = (('spk', '4kHz超のプチッ（入力との差）/秒'), ('spkU', '同・無声の所だけ /秒'), ('jmp', '2kHz超の強さの飛び出し（入力との差）/秒'),
          ('miss', '有声を無声と判定 %'), ('false', '無声を有声と判定 %'), ('pbad', '高さが50セント超ずれた %'),
          ('aper', '出力の周期の乱れ（YIN の谷）'))


def scenes(exes, reps):
    print(f'\n### 3. 合成の声の 8 場面（{reps} 通り、部屋の雑音 -45dB）')
    labs = list(exes)
    for si, sc in enumerate(SCENES):
        R = {lab: [] for lab in labs}
        for r in range(reps):
            rng = np.random.default_rng(1000 + r * 7 + si * 1000)
            x, kind, tr = scene(sc, rng)
            x = x + noise(len(x), rng, 50) * 10 ** (-45 / 20) * 0.5
            for lab, exe in exes.items():
                vt, vv, y = frames(exe, x, 's')
                R[lab].append(evaluate(x, kind, tr, y, vt, vv))
        print(f'  == {sc}')
        for key, lab in LABELS:
            cols = [[d[key] for d in R[l]] for l in labs]
            if np.all(np.isnan(cols[0])):
                continue
            line = f'     {lab:38s}' + ''.join(f'{np.nanmean(c):9.2f}' for c in cols)
            if len(labs) > 1:
                p, good, bad = paired(cols[1], cols[0])
                line += f'   p={p:.3f}（良 {good} / 悪 {bad}）'
            print(line)
    if len(labs) > 1:
        print(f'  （列: {" / ".join(labs)}。p と良・悪は {labs[1]} → {labs[0]}。どの値も小さいほど良い）')


# ---- 4. ブロックごとの処理時間 ----

def blocks(exes, voice):
    print('\n### 4. ブロックごとの処理時間（最大 / 99.9% 点。ブロックの長さに対する %。3 回のうち 99.9% 点が一番小さい回）')
    if voice:
        x = C.read(voice)[1]
    else:
        b = C.Builder(3).gap(0.3)
        for f0 in (120, 180):
            b.voice(f0, 0.6, 'a').noise(0.12, 0.05).voice(f0, 0.5, 'o').gap(0.4)
        x = np.tile(b.done()[0], 5)
    i = C.path('b_in.wav'); C.write(i, x)
    for blk in (32, 128, 512):
        line = f'  ブロック {blk:4d}: '
        for lab, exe in exes.items():
            best = None
            for _ in range(3):
                msg = subprocess.run([exe, i, C.path('b_out.wav'), '--block', str(blk)], capture_output=True, text=True).stdout
                m = re.search(r'block max [0-9.]+ us \(([0-9.]+)%.*p99.9 [0-9.]+ us \(([0-9.]+)%', msg)
                if m and (best is None or float(m.group(2)) < best[1]):
                    best = (float(m.group(1)), float(m.group(2)))
            line += f'{lab} {best[0]:5.1f}% / {best[1]:5.1f}%   ' if best else f'{lab} （記録なし）   '
        print(line)


def pitch(exes, reps):
    print(f'\n### 5. 出力の高さの正確な誤り（断片の間隔から。{reps} 通り。50 セント超の割合 / 中央値）')
    for si in (7, 5, 0, 4):
        line = f'  {SCENES[si][:14]:16s}'
        for lab, exe in exes.items():
            det, lag = [], []
            for r in range(reps):
                rng = np.random.default_rng(1000 + r * 7 + si * 1000)
                x, kind, tr = scene(SCENES[si], rng)
                x = x + noise(len(x), rng, 50) * 10 ** (-45 / 20) * 0.5
                i = C.path('pe_in.wav'); g = C.path('pe_g.csv'); C.write(i, x)
                subprocess.run([exe, i, C.path('pe_out.wav'), '--align', '--grains', g], check=True, capture_output=True)
                for to, ts, P in C.load_csv(g):
                    to, ts = int(to), int(ts)
                    if P <= 0 or not (0 <= ts < len(x) and 0 <= to < len(x)) or kind[ts] != 'V' or tr[ts] <= 0:
                        continue
                    det.append(abs(1200 * np.log2(FS / P / tr[ts])))
                    if kind[to] == 'V' and tr[to] > 0:
                        lag.append(abs(1200 * np.log2(FS / P / tr[to])))
            det, lag = np.array(det), np.array(lag)
            line += f'  {lab}: 検出 {np.mean(det > 50) * 100:5.2f}% / {np.median(det):4.1f}  出力の時刻 {np.mean(lag > 50) * 100:5.2f}% / {np.median(lag):4.1f}'
        print(line)


def main():
    ap = C.args_with_voice('出力の音の質の確かめ')
    ap.add_argument('--compare', help='比べる別の版の vcabsh_render.exe')
    ap.add_argument('--reps', type=int, default=8, help='場面ごとの通りの数（初期値 8）')
    ap.add_argument('--only', default='harmonics,pulses,scenes,blocks,pitch')
    a = ap.parse_args()
    exes = {'今の版': C.RENDER}
    if a.compare:
        exes['比べる版'] = a.compare
    only = a.only.split(',')
    if 'harmonics' in only:
        harmonics(exes)
    if 'pulses' in only:
        pulses(exes, a.reps)
    if 'scenes' in only:
        scenes(exes, a.reps)
    if 'blocks' in only:
        blocks(exes, a.voice)
    if 'pitch' in only:
        pitch(exes, a.reps)


if __name__ == '__main__':
    main()
