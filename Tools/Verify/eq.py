# 声に追従する EQ の確かめ。
#   1. 合成の母音（120Hz、5 母音）で、EQ をかけた出力とかけない出力の倍音ごとの比が、狙いの線（ベル形の足し合わせ）と合うか
#      （中心は、基音が出力の高さ、F1〜F4 が母音の F1〜F4 を有声フォルマントの分だけ動かした所。追いかけの誤差の分はずれる）
#   2. 雑音（摩擦音のつもり）で、無声の EQ が無声の F1〜F4 の所に効き、有声の EQ が効かないか
#   3. ブロックの大きさ（1・64・4096）で出力が変わらないか（EQ をかけたまま）
#   4. --voice: 録音した声で、出力の山と、黙っている間に音が残らないか
import sys

import numpy as np

from common import FS, REFERENCE, VOWELS, Builder, args_with_voice, frames_of, path, read, render, write

sys.stdout.reconfigure(encoding='utf-8')

PITCH, FORMANT, UFORMANT = 12.0, 6.0, 3.0
FIXED = (500.0, 1400.0, 2700.0, 3500.0)
# 合成の音の確かめ（1〜3）は、初期値が変わっても同じ所を測るよう、つまみをはっきり渡す
# （無声フォルマントの初期値は 2026-11-17 に +3 から +6 になった。4 の録音した声は初期値のまま）
BASE = [f'pitch={PITCH}', f'formant={FORMANT}', f'unvoiced_formant={UFORMANT}']


def bell_db(f, fc, g, bw):
    """FollowEq の eqBellDb と同じ式"""
    if g == 0:
        return np.zeros_like(f)
    w = np.tan(np.pi * np.minimum(f, 0.4999 * FS) / FS) / np.tan(np.pi * fc / FS)
    a = 10 ** (g / 40)
    q = 1 / (2 * np.sinh(np.log(2) / 2 * bw))
    re = 1 - w * w
    return 10 * np.log10((re * re + (a / q * w) ** 2) / (re * re + (w / (a * q)) ** 2))


def spectrum(y, lo, hi):
    seg = y[lo:hi] * np.hanning(hi - lo)
    n = 1 << int(np.ceil(np.log2(len(seg)) + 2))
    return np.abs(np.fft.rfft(seg, n)) ** 2, n


def harmonic_gain(base, eq, lo, hi, f0, kmax):
    """倍音ごとの、EQ あり / なし の比（dB）。倍音の周りの ±f0/4 のパワーで比べる"""
    pb, n = spectrum(base, lo, hi)
    pe, _ = spectrum(eq, lo, hi)
    out = []
    for k in range(1, kmax + 1):
        c = k * f0 * n / FS
        w = f0 / 4 * n / FS
        s = slice(int(c - w), int(c + w) + 1)
        out.append(10 * np.log10(pe[s].sum() / (pb[s].sum() + 1e-30) + 1e-30))
    return np.array(out)


def vowels():
    b = Builder(seed=3)
    spans = []
    for v in 'aiueo':
        b.gap(0.1)
        start = b.pos
        b.voice(120.0, 0.6, v)
        spans.append((v, start + int(0.2 * FS), start + int(0.5 * FS)))
    b.gap(0.1)
    x, _, _ = b.done()
    return x, spans


def check_voiced():
    x, spans = vowels()
    inp = path('eq_vowels.wav')
    write(inp, x)
    base = path('eq_base.wav')
    render(inp, base, BASE)
    _, yb = read(base)
    cases = {
        '基音 +6': ['eq_f0=6'],
        'F1 +6': ['eq_f1=6'],
        'F2 +6': ['eq_f2=6'],
        'F3 -6': ['eq_f3=-6'],
        'F4 +6': ['eq_f4=6'],
        '全部 +6（無声 F1〜F4 も +6）': ['eq_f0=6', 'eq_f1=6', 'eq_f2=6', 'eq_f3=6', 'eq_f4=6',
                                      'eq_uf1=6', 'eq_uf2=6', 'eq_uf3=6', 'eq_uf4=6'],
    }
    f0 = 120.0 * 2 ** (PITCH / 12)
    print(f'1. 合成の母音（入力 120Hz → 出力 {f0:.0f}Hz）。倍音ごとの比（測った値 − 狙いの線、dB）。200Hz〜5kHz の倍音')
    for name, st in cases.items():
        out = path('eq_x.wav')
        render(inp, out, BASE + st)
        _, ye = read(out)
        gains = {k.split('=')[0]: float(k.split('=')[1]) for k in st}
        errs, f0gain, peak = [], [], []
        for v, lo, hi in spans:
            kmax = int(5000 // f0)
            g = harmonic_gain(yb, ye, lo, hi, f0, kmax)
            fk = f0 * np.arange(1, kmax + 1)
            want = bell_db(fk, f0, gains.get('eq_f0', 0), 2 / 3)
            for k in range(4):
                fc = VOWELS[v][k] * 2 ** (FORMANT / 12)
                want = want + bell_db(fk, fc, gains.get(f'eq_f{k + 1}', 0), 1.0)
            errs.append(g - want)
            f0gain.append(g[0])
            peak.append(np.max(np.abs(g)))
        e = np.concatenate(errs)
        print(f'   {name:24s} 差の中央値 {np.median(np.abs(e)):.2f}dB、90% 点 {np.percentile(np.abs(e), 90):.2f}dB'
              f'、基音の倍音の比 {np.mean(f0gain):+.2f}dB（母音ごと {" ".join(f"{a:+.1f}" for a in f0gain)}）')
    print(f'   EQ なしの出力と 0dB の EQ: 一致は regress.py check で確かめる')


def check_unvoiced():
    b = Builder(seed=5)
    b.gap(0.2)
    lo = b.pos + int(0.15 * FS)
    b.noise(0.6, 0.05, highpass=1000)
    hi = b.pos - int(0.15 * FS)
    b.gap(0.2)
    x, _, _ = b.done()
    inp = path('eq_noise.wav')
    write(inp, x)
    base = path('eq_nbase.wav')
    render(inp, base, BASE)
    _, yb = read(base)
    print('2. 雑音（1kHz より上）。帯域ごとの比（EQ あり / なし、dB）。無声の F1〜F4 は追いかけられないので決まった位置 × 無声フォルマント')
    centres = [f * 2 ** (UFORMANT / 12) for f in FIXED]
    for name, st in {'無声 F3 +6': ['eq_uf3=6'], '無声 F4 -6': ['eq_uf4=-6'], '有声 全部 +6': ['eq_f0=6', 'eq_f1=6', 'eq_f2=6', 'eq_f3=6', 'eq_f4=6']}.items():
        out = path('eq_nx.wav')
        render(inp, out, BASE + st)
        _, ye = read(out)
        pb, n = spectrum(yb, lo, hi)
        pe, _ = spectrum(ye, lo, hi)
        row = []
        for fc in [centres[1], centres[2], centres[3], 8000.0]:
            s = slice(int(fc / 2 ** (1 / 12) * n / FS), int(fc * 2 ** (1 / 12) * n / FS))
            row.append(f'{fc:.0f}Hz {10 * np.log10(pe[s].sum() / pb[s].sum()):+.1f}')
        print(f'   {name:12s} ' + '、'.join(row))


def check_blocks():
    x, _ = vowels()
    inp = path('eq_vowels.wav')
    st = BASE + ['eq_f0=6', 'eq_f2=-4', 'eq_uf3=5', 'warp_mode=1']
    outs = []
    for blk in (1, 64, 4096):
        out = path(f'eq_b{blk}.wav')
        render(inp, out, st, extra=['--block', str(blk)])
        outs.append(read(out)[1])
    d = max(np.max(np.abs(outs[0] - o)) for o in outs[1:])
    print(f'3. ブロック 1・64・4096 の出力の最大の差: {d:.2e}（16 ビットの 1 段は 3.1e-05）')


def check_voice(voice):
    _, x = read(voice)
    print('4. 録音した声: 出力の山と実効値（dBFS。処理ツールの出力は 0dBFS で切れる）')
    for name, st in {'EQ なし': [], '基音 +12': ['eq_f0=12'], '全部 +12': [f'eq_f{k}=12' for k in range(5)] + [f'eq_uf{k}=12' for k in range(1, 5)],
                     '全部 -12': [f'eq_f{k}=-12' for k in range(5)] + [f'eq_uf{k}=-12' for k in range(1, 5)]}.items():
        out = path('eq_v.wav')
        render(voice, out, st)
        _, y = read(out)
        print(f'   {name:8s} 山 {20 * np.log10(np.max(np.abs(y)) + 1e-12):+.1f}、実効値 {10 * np.log10(np.mean(y ** 2) + 1e-20):+.1f}')


def windows(n, win, hop):
    return range(win // 2, n - win // 2, hop)


def ratio_at(pb, pe, n, f, half):
    s = slice(max(0, int((f - half) * n / FS)), int((f + half) * n / FS) + 1)
    return 10 * np.log10(pe[s].sum() / (pb[s].sum() + 1e-30) + 1e-30)


def check_follow(voice):
    """録音した声で、EQ が実際の基音と F1〜F4 の山に付いて行けているか。
    基音: 本来の pYIN に近い形（先読み 63）の高さ × 2 を正解の出力の基音として、そこの倍音が +6dB 上がっているか。
    F1〜F4: EQ あり / なし の比が一番大きい倍音（EQ の山）が、出力の包絡（ケプストラム）の山からどれだけ離れているか"""
    _, x = read(voice)
    base = path('eqv_base.wav')
    render(voice, base)
    _, yb = read(base)
    t_ref, f_ref = frames_of(voice, extra=REFERENCE)
    t_eng, f_eng = frames_of(voice)
    win, hop = int(0.030 * FS), int(0.010 * FS)
    ratio = 2 ** (PITCH / 12)

    def f0_at(t, tt, ff):
        i = np.clip(np.searchsorted(tt, t), 1, len(tt) - 1)
        return ff[i - 1] if abs(tt[i - 1] - t) < abs(tt[i] - t) else ff[i]

    # 窓の中ずっと有声（正解）で、高さの変わりが 3% 以内の窓。声の出だし（有声になってから 30ms 以内）は別に数える
    voiced_t = t_ref[f_ref > 0]
    sel, onset = [], []
    for c in windows(min(len(yb), len(x)), win, hop):
        ts = [(c + d) / FS for d in (-win // 2, 0, win // 2)]
        fs_ = [f0_at(t, t_ref, f_ref) for t in ts]
        if min(fs_) <= 0 or max(fs_) / min(fs_) > 1.03:
            continue
        # 有声の続きの始まり
        k = np.searchsorted(t_ref, ts[0])
        j = k
        while j > 0 and f_ref[j - 1] > 0:
            j -= 1
        since = ts[0] - t_ref[j]
        sel.append((c, fs_[1], f0_at(ts[1], t_eng, f_eng), since < 0.030))
    print(f'5. 録音した声（{len(x) / FS:.1f} 秒）。30ms の窓、10ms ごと。窓の中ずっと有声で高さが安定している窓 {len(sel)} 個')

    out = path('eqv_x.wav')
    render(voice, out, ['eq_f0=6'])
    _, ye = read(out)
    g1, g2, on, miss = [], [], [], 0
    for c, fr, fe, is_on in sel:
        pb, n = spectrum(yb, c - win // 2, c + win // 2)
        pe, _ = spectrum(ye, c - win // 2, c + win // 2)
        h1 = fr * ratio
        a = ratio_at(pb, pe, n, h1, h1 / 4)
        b = ratio_at(pb, pe, n, 2 * h1, h1 / 4)
        if fe <= 0 or abs(12 * np.log2(fe / fr)) > 1:
            miss += 1
        (on if is_on else g1).append(a)
        if not is_on:
            g2.append(b)
    g1, g2, on = np.array(g1), np.array(g2), np.array(on)
    want2 = bell_db(np.array([2.0]), 1.0, 6, 2 / 3)[0]
    print(f'   基音 +6: 正解の基音の倍音の比 中央値 {np.median(g1):+.2f}dB、±1dB 以内 {np.mean(np.abs(g1 - 6) <= 1) * 100:.1f}%、'
          f'±2dB 以内 {np.mean(np.abs(g1 - 6) <= 2) * 100:.1f}%、10% 点 {np.percentile(g1, 10):+.2f}dB')
    print(f'            2 倍音の比 中央値 {np.median(g2):+.2f}dB（狙いの線では {want2:+.2f}dB）')
    if len(on):
        print(f'            声の出だし 30ms 以内の窓 {len(on)} 個: 中央値 {np.median(on):+.2f}dB、±1dB 以内 {np.mean(np.abs(on - 6) <= 1) * 100:.1f}%')
    print(f'            検出の高さが正解と 1 半音超違う窓 {miss} / {len(sel)}')

    for k in range(1, 5):
        render(voice, out, [f'eq_f{k}=6'])
        _, ye = read(out)
        dist, gains = [], []
        for c, fr, _, is_on in sel:
            if is_on:
                continue
            lo, hi = c - win // 2, c + win // 2
            pb, n = spectrum(yb, lo, hi)
            pe, _ = spectrum(ye, lo, hi)
            h1 = fr * ratio
            ks = np.arange(1, int(min(6000, 0.45 * FS) // h1) + 1)
            gr = np.array([ratio_at(pb, pe, n, kk * h1, h1 / 4) for kk in ks])
            peak = ks[np.argmax(gr)] * h1
            gains.append(gr.max())
            # 出力の包絡（ケプストラム。2ms より先を落とす）の山のうち、EQ の山に一番近いもの
            lp = np.log(pb + 1e-20)
            cep = np.fft.irfft(lp)
            q = int(0.002 * FS)  # ケプストラムの番号は、標本化周波数の標本の数（FFT の大きさによらない）
            cep[q:len(cep) - q] = 0
            env = np.fft.rfft(cep).real
            fr_ax = np.arange(len(env)) * FS / n
            m = (fr_ax > 150) & (fr_ax < 6000)
            idx = np.where(m[1:-1] & (env[1:-1] > env[:-2]) & (env[1:-1] > env[2:]))[0] + 1
            if len(idx) == 0:
                continue
            dist.append(np.min(np.abs(12 * np.log2(fr_ax[idx] / peak))))
        dist = np.array(dist)
        print(f'   F{k} +6: EQ の山の高さ 中央値 {np.median(gains):+.2f}dB。EQ の山から一番近い包絡の山まで 中央値 {np.median(dist):.1f} 半音、'
              f'2 半音以内 {np.mean(dist <= 2) * 100:.0f}%、4 半音以内 {np.mean(dist <= 4) * 100:.0f}%')


def check_tracking(voice):
    """EQ の F1〜F4 の中心は「追いかけた F1〜F4 × 動く量」なので、追いかけた位置が入力の包絡の山に乗っているかを、
    倍音の細かい入力の側で見る（出力は基音が約 2 倍で、F1 の周りに倍音が数本しかなく、山の位置を細かく測れない）。
    注意: F1 は当てにならない。比べる側の LPC が、基音のあたり（120〜290Hz）の山を拾い、F1 の山は幅が広くて外れることが多い（2026-11-17）"""
    import numpy.linalg as la
    _, x = read(voice)
    trk = path('eqv_track.csv')
    render(voice, path('eqv_t.wav'), extra=['--track', trk])
    d = np.loadtxt(trk, delimiter=',', skiprows=1, ndmin=2)
    t_ref, f_ref = frames_of(voice, extra=REFERENCE)
    win = int(0.030 * FS)
    # 包絡: 入力を 16kHz に落とし、窓をかけて 20 次の LPC（自己相関法）。山は根から（幅 500Hz 未満）
    import scipy.signal as ss
    x16 = ss.resample_poly(x, 1, 3)
    fs16 = FS / 3
    dist = [[] for _ in range(4)]
    for row in d[::2]:
        t = row[0]
        if row[1] <= 0:
            continue
        i = np.clip(np.searchsorted(t_ref, t), 0, len(t_ref) - 1)
        if f_ref[i] <= 0:
            continue
        c = int(t * fs16)
        w = int(win / 3)
        if c - w // 2 < 0 or c + w // 2 > len(x16):
            continue
        s = x16[c - w // 2:c + w // 2]
        s = np.append(s[0], s[1:] - 0.7 * s[:-1]) * np.hamming(len(s))
        r = np.correlate(s, s, 'full')[len(s) - 1:len(s) + 21]
        if r[0] <= 0:
            continue
        a = la.solve(np.array([[r[abs(i - j)] for j in range(20)] for i in range(20)]) + np.eye(20) * r[0] * 1e-6, -r[1:21])
        roots = np.roots(np.concatenate([[1], a]))
        roots = roots[np.imag(roots) > 0]
        fr = np.angle(roots) * fs16 / (2 * np.pi)
        bw = -np.log(np.abs(roots)) * fs16 / np.pi
        peaks = np.sort(fr[(bw < 500) & (fr > 150) & (fr < 5500)])
        if len(peaks) == 0:
            continue
        for k in range(4):
            if row[6 + k] < 0.5:  # 追いかけられていない
                continue
            dist[k].append(np.min(np.abs(12 * np.log2(peaks / row[2 + k]))))
    print('7. 追いかけた F1〜F4 と、入力の包絡（LPC 20 次、別に求めたもの）の一番近い山までの距離（有声で追いかけている所）')
    for k in range(4):
        dk = np.array(dist[k])
        print(f'   F{k + 1}: {len(dk)} フレーム、中央値 {np.median(dk):.2f} 半音、1 半音以内 {np.mean(dk <= 1) * 100:.0f}%、'
              f'2 半音以内 {np.mean(dk <= 2) * 100:.0f}%、6 半音（EQ の幅の半分）超 {np.mean(dk > 6) * 100:.1f}%')


def make_listening(voice, outdir):
    import os
    import shutil
    os.makedirs(outdir, exist_ok=True)
    shutil.copy(voice, os.path.join(outdir, '0_入力.wav'))
    sets = {'1_EQなし': [], '2_おすすめ（基音+3 F3-2 F4-2）': ['eq_f0=3', 'eq_f3=-2', 'eq_f4=-2'],
            '3_基音+3だけ': ['eq_f0=3'], '4_F3-2 F4-2だけ': ['eq_f3=-2', 'eq_f4=-2'], '5_基音+6だけ': ['eq_f0=6']}
    print('6. 聞き比べ用の WAV（初期値＋EQ。遅延を詰めたもの）と、声のある所の実効値')
    _, x = read(voice)
    for name, st in sets.items():
        p = os.path.join(outdir, name + '.wav')
        render(voice, p, st)
        _, y = read(p)
        m = min(len(x), len(y))
        env = np.convolve(x[:m] ** 2, np.ones(1440) / 1440, 'same')
        loud = env > np.max(env) * 1e-3
        print(f'   {name:28s} 実効値 {10 * np.log10(np.mean(y[:m][loud] ** 2)):+.1f}dBFS、山 {20 * np.log10(np.max(np.abs(y))):+.1f}dBFS')


def main():
    p = args_with_voice('声に追従する EQ の確かめ')
    p.add_argument('--listen', help='聞き比べ用の WAV を書くフォルダ（--voice と使う）')
    p.add_argument('--follow-only', action='store_true', help='録音した声の確かめだけ')
    a = p.parse_args()
    if not a.follow_only:
        check_voiced()
        check_unvoiced()
        check_blocks()
    if a.voice:
        if not a.follow_only:
            check_voice(a.voice)
        check_follow(a.voice)
        check_tracking(a.voice)
        if a.listen:
            make_listening(a.voice, a.listen)


if __name__ == '__main__':
    main()
