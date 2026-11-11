# 確かめのスクリプトの共通部分: WAV の処理ツールの呼び出し、合成の声、記録（CSV）の読み込み。
# 作業用のファイルは build/verify に置く（Git の対象外）。先に vcabsh_render をビルドしておく。
import argparse
import os
import subprocess

import numpy as np
import scipy.io.wavfile as wf
import scipy.signal as ss

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RENDER = os.path.join(ROOT, 'build', 'vcabsh_render_artefacts', 'Release', 'vcabsh_render.exe')
WORK = os.path.join(ROOT, 'build', 'verify')
FS = 48000
os.makedirs(WORK, exist_ok=True)

VOWELS = {'a': (750, 1200, 2600, 3500), 'i': (300, 2300, 3000, 3700), 'u': (350, 1400, 2300, 3500),
          'e': (480, 2000, 2700, 3600), 'o': (480, 850, 2600, 3500)}
BANDWIDTHS = (90, 110, 160, 250)


def args_with_voice(description):
    """--voice（録音した声の WAV。渡さなければ合成の声だけで確かめる）を受け取る"""
    p = argparse.ArgumentParser(description=description)
    p.add_argument('--voice', help='録音した声の WAV（モノラル。声のある所と黙っている所が混ざったもの）')
    return p


def path(name):
    return os.path.join(WORK, name)


# ---- WAV ----

def read(p):
    fs, x = wf.read(p)
    x = x.astype(np.float64) / (2 ** 31 if x.dtype == np.int32 else 32768)
    return fs, (x if x.ndim == 1 else x[:, 0])


def write(p, x, fs=FS):
    wf.write(p, fs, (np.clip(x, -1, 1) * 32767).astype(np.int16))


def render(inp, out, settings=(), frames=None, marks=None, grains=None, exe=RENDER, extra=()):
    """vcabsh_render で処理する（遅延の分を詰める）。settings はキー=値、extra はほかの引数。表示を返す"""
    cmd = [exe, inp, out, '--align'] + list(settings) + list(extra)
    if frames:
        cmd += ['--frames', frames]
    if marks:
        cmd += ['--marks', marks]
    if grains:
        cmd += ['--grains', grains]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()


def load_csv(p):
    return np.loadtxt(p, delimiter=',', skiprows=1, ndmin=2)


def frames_of(inp, settings=(), extra=()):
    """決まったフレームごとの (時刻, 高さ（無声は 0）)"""
    p = path('_frames.csv')
    render(inp, path('_x.wav'), settings, frames=p, extra=extra)
    d = load_csv(p)
    return d[:, 0], d[:, 2] * (d[:, 1] > 0)


REFERENCE = ('--lookahead', '63', '--octave-jump', '0', '--no-cues')  # 本来の pYIN に近い形（先読み 63 フレーム）


# ---- 合成の声 ----

def shape(pulse, vowel, fs=FS):
    """声帯のパルス列に -12dB/oct の傾きと +6dB/oct の口の放射をかけ、F1〜F4 の共振に通す"""
    y = ss.lfilter([1], [1, -0.97], ss.lfilter([1], [1, -0.97], pulse))
    y = ss.lfilter([1, -1], [1], y)
    for F, B in zip(VOWELS[vowel], BANDWIDTHS):
        r = np.exp(-np.pi * B / fs)
        th = 2 * np.pi * F / fs
        y = ss.lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], y)
    return y


def norm(y, level=0.3):
    return y / (np.max(np.abs(y)) + 1e-12) * level


class Builder:
    """声と無音と雑音をつないで、正解の高さ（標本ごと。無声は 0、正解なしは -1）とパルスの位置も持つ"""

    def __init__(self, seed=1, fs=FS):
        self.fs = fs
        self.rng = np.random.default_rng(seed)
        self.sig, self.truth, self.pulses = [], [], []
        self.pos = 0

    def gap(self, dur):
        n = int(dur * self.fs)
        self.sig.append(np.zeros(n)); self.truth.append(np.zeros(n)); self.pos += n
        return self

    def voice(self, f0, dur, vowel='a', jitter=0.0, amp=None, ramp=0.015, level=0.3, truth=True):
        """f0: 高さ（Hz）か、時刻（秒）→ 高さ の関数。amp: (番号, 時刻) → 振幅"""
        fs = self.fs
        n = int(dur * fs)
        f0fun = f0 if callable(f0) else (lambda t, v=f0: v)
        times, t = [], 0.0
        while t < dur:
            times.append(t)
            t += (1.0 / f0fun(t)) * (1 + jitter * self.rng.standard_normal())
        times = np.array(times)
        a = np.ones(len(times)) if amp is None else np.array([amp(i, tt) for i, tt in enumerate(times)])
        p = np.zeros(n)
        for tt, aa in zip(times, a):
            i = int(round(tt * fs))
            if i < n:
                p[i] += aa
        y = shape(p, vowel, fs)
        tt = np.arange(n) / fs
        env = np.minimum(1, np.minimum(tt / ramp, (dur - tt) / 0.015))
        tr = np.zeros(n)
        if truth:
            for k in range(len(times) - 1):
                tr[int(times[k] * fs):int(times[k + 1] * fs)] = 1.0 / (times[k + 1] - times[k])
        else:
            tr[:] = -1
        self.sig.append(norm(y * env, level)); self.truth.append(tr)
        self.pulses += list(self.pos + np.round(times * fs))
        self.pos += n
        return self

    def noise(self, dur, level, highpass=3000):
        fs = self.fs
        n = int(dur * fs)
        y = ss.lfilter(*ss.butter(4, highpass / (fs / 2), 'high'), self.rng.standard_normal(n))
        tt = np.arange(n) / fs
        env = np.minimum(1, np.minimum(tt / 0.01, (dur - tt) / 0.01))
        self.sig.append(y / np.std(y) * level * env); self.truth.append(np.zeros(n)); self.pos += n
        return self

    def done(self):
        return np.concatenate(self.sig), np.concatenate(self.truth), np.array(self.pulses)


def mark_phase(marks, pulses):
    """有声の印ごとに、周期の中の位置（直前のパルスで 0、次のパルスで 1）"""
    m = marks[marks[:, 2] > 0, 0]
    k = np.searchsorted(pulses, m)
    ok = (k > 0) & (k < len(pulses))
    return (m[ok] - pulses[k[ok] - 1]) / (pulses[k[ok]] - pulses[k[ok] - 1])
