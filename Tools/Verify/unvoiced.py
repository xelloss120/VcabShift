# 無声音の処理の確かめ。
#   python Tools/Verify/unvoiced.py [--voice 録音.wav]
# 無声の区間（つなぎ目の 10ms を除き、-50dBFS より大きい所）の、帯域ごとの大きさ（dBFS）、
# 4kHz より上の「プチッ」という短い音の数（1 秒あたり）、5ms ごとの強弱の揺れ（200Hz、dB）を、設定ごとに出す。
# 有声の区間の帯域も出す（無声の設定で変わらないことの確かめ）。
# 合成の声（母音と、さ行・は行の摩擦音）と、--voice の録音した声で行う。
import numpy as np
import scipy.signal as ss
from scipy.ndimage import binary_erosion, maximum_filter1d, uniform_filter1d

import common as C

BANDS = ((2000, 4000), (4000, 8000), (8000, 12000), (12000, 16000), (16000, 20000))
SETTINGS = (('入力', None), ('無声フォルマント 0', ('unvoiced_formant=0',)), ('無声フォルマント +3（初期値）', ()),
            ('無声フォルマント +6', ('unvoiced_formant=6',)), ('無声F4以降 地声合わせ', ('unvoiced_above_f4=2',)))


def masks(inp, x, fs):
    t, f = C.frames_of(inp)
    voiced = np.zeros(len(x), bool)
    for tt, ff in zip(t, f):
        i = int(tt * fs)
        voiced[max(0, i - fs // 400):i + fs // 400] = ff > 0
    lvl = np.sqrt(uniform_filter1d(x ** 2, fs // 100))
    loud = lvl > 10 ** (-50 / 20)
    return binary_erosion(~voiced & loud, np.ones(fs // 100)), binary_erosion(voiced & loud, np.ones(fs // 100))


def band_db(y, m, lo, hi, fs):
    b = ss.butter(4, [lo / (fs / 2), min(hi, fs / 2 * 0.98) / (fs / 2)], 'band')
    return 10 * np.log10(np.mean(ss.lfilter(*b, y)[m] ** 2) + 1e-20)


def clicks(y, m, fs):
    h = ss.lfilter(*ss.butter(4, 4000 / (fs / 2), 'high'), y)
    pk = maximum_filter1d(np.abs(h), fs // 1000)
    rms = np.sqrt(uniform_filter1d(h ** 2, fs // 50) + 1e-20)
    hit = ((pk > 4.0 * rms) & m & (rms > 1e-5))[:: fs // 1000]
    return np.sum(hit[1:] & ~hit[:-1]) / (m.sum() / fs + 1e-9)


def wobble(y, m, fs):
    h = ss.lfilter(*ss.butter(4, 2000 / (fs / 2), 'high'), y) * m
    env = np.abs(ss.hilbert(h))
    f, P = ss.welch(env - env[m].mean() * m, fs, nperseg=8192)
    return 10 * np.log10(P[(f > 190) & (f < 210)].max() / np.median(P[(f > 100) & (f < 400)]))


def report(name, inp):
    fs, x = C.read(inp)
    uv, vo = masks(inp, x, fs)
    print(f'== {name}（無声で音のある所 {uv.sum() / fs:.2f} 秒）')
    print('   ' + ' ' * 28 + 'プチッ/s  揺れ dB  無声の帯域 dBFS: ' + ' '.join(f'{lo // 1000}-{hi // 1000}k' for lo, hi in BANDS)
          + '  | 有声 ' + ' '.join(f'{lo // 1000}-{hi // 1000}k' for lo, hi in BANDS[1:4]))
    for label, s in SETTINGS:
        if s is None:
            y = x
        else:
            out = C.path('unvoiced_out.wav'); C.render(inp, out, s)
            y = C.read(out)[1][:len(x)]
        print(f'   {label:28s} {clicks(y, uv, fs):6.1f} {wobble(y, uv, fs):7.1f}   '
              + ' '.join(f'{band_db(y, uv, lo, hi, fs):6.1f}' for lo, hi in BANDS)
              + '  | ' + ' '.join(f'{band_db(y, vo, lo, hi, fs):6.1f}' for lo, hi in BANDS[1:4]))


def main():
    a = C.args_with_voice('無声音の処理の確かめ').parse_args()
    b = C.Builder(21).gap(0.1)
    for f0 in (120, 220):
        for hp, lvl in ((3500, 0.06), (1200, 0.03)):
            b.voice(f0, 0.2, 'a').noise(0.15, lvl, hp).voice(f0, 0.2, 'i').gap(0.1)
    x, _, _ = b.done()
    inp = C.path('unvoiced_syn.wav'); C.write(inp, x)
    report('合成（母音＋摩擦音＋母音）', inp)
    if a.voice:
        report('録音した声', a.voice)


if __name__ == '__main__':
    main()
