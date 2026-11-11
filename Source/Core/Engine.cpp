#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kHopSeconds = 0.005;
constexpr double kUnvoicedSeconds = 0.005;
constexpr double kFadeSeconds = 0.010;      // 有声と無声のつなぎ、チャンネルの切り替え
constexpr double kLifterSeconds = 0.0025;   // 有声の補正の包絡（周期の半分と短いほう）
constexpr double kLifterUnvoicedSeconds = 0.0012;
constexpr double kLowestPitch = 40.0, kHighestPitch = 1000.0;
constexpr double kTargetLowRate = 16000.0;  // F4 まで追いかけるため、約 16kHz に落とす
constexpr float kSilentDb = -60.0f, kBelowPeakDb = 40.0f, kPeakDecayDbPerSec = 20.0f;
// 有声・無声の手がかり（周期性以外）。log10 の倍率で、どれも ±1（10 倍）まで
constexpr float kCueLowLo = 80.0f, kCueLowHi = 1000.0f, kCueHighLo = 3000.0f; // 低域と高域（高域の上は、落とした音の 0.4 倍）
constexpr float kCueTiltCentre = -7.5f, kCueTiltSlope = 0.08f; // 高域/低域が -20dB で +1、+5dB で -1
constexpr float kCueQuietDb = 40.0f, kCueQuietSlope = 0.1f;    // 最近の一番大きい音より 40dB 下から下げ始め、50dB 下で -1
constexpr float kCueRiseNone = 1.0f, kCueRiseFull = 5.0f;      // 有声に寄せる分は、音量が 2 フレーム前から上がっているときだけ（1dB で 0、5dB で全部）
constexpr double kChannelSwitchSeconds = 2.0;
constexpr float kChannelSwitchRatio = 3.98f; // 6dB（パワー）
constexpr float kSilentGrainPeak = 1.0e-6f; // これより小さい断片は無音として作らない（-120dBFS）
constexpr int kFrameRing = 1024;
constexpr int kMarkRing = 4096;

int64_t ringSize (double samples)
{
    int64_t n = 1;
    while (n < static_cast<int64_t> (samples))
        n <<= 1;
    return n;
}
} // namespace

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    fs = sampleRate;

    const int tMaxAbs = static_cast<int> (std::ceil (fs / kLowestPitch));
    const int64_t r = ringSize (fs * 1.0 + maxBlockSize);
    xRing.assign (static_cast<size_t> (r), 0.0f);
    xMask = r - 1;
    accVoiced.assign (static_cast<size_t> (r), 0.0f);
    accUnvoiced.assign (static_cast<size_t> (r), 0.0f);
    accMask = r - 1;

    decim = std::max (1, static_cast<int> (std::lround (fs / kTargetLowRate)));
    fsd = fs / decim;
    // 低域通過（窓をかけた sinc、約 0.42 × 落とした後の標本化周波数で切る）
    const int taps = 16 * decim + 1;
    lpDelay = (taps - 1) / 2;
    lowpass.resize (static_cast<size_t> (taps));
    const double fc = 0.42 * fsd / fs;
    double sum = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        const double m = i - lpDelay;
        const double s = m == 0 ? 2.0 * fc : std::sin (2.0 * kPi * fc * m) / (kPi * m);
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 1) / (taps + 1));
        lowpass[static_cast<size_t> (i)] = static_cast<float> (s * w);
        sum += s * w;
    }
    for (auto& c : lowpass)
        c = static_cast<float> (c / sum);
    const int64_t dr = ringSize (fsd * 1.0);
    dRing.assign (static_cast<size_t> (dr), 0.0f);
    dMask = dr - 1;

    lag = std::clamp (tuning.pitchLookahead, 0, kPendingRing - 1);
    detector.prepare (static_cast<float> (fsd), static_cast<float> (kLowestPitch), static_cast<float> (kHighestPitch), lag, tuning.octaveJump);
    hop = static_cast<int> (std::lround (kHopSeconds * fs));
    tracker.prepare (static_cast<float> (fsd), static_cast<float> (hop / fs));
    scratch.assign (static_cast<size_t> (std::max (2 * (static_cast<int> (std::ceil (fsd / kLowestPitch)) + 4) + 2,
                                                   tracker.windowLength()) + 8), 0.0f);
    {
        int n = 1;
        while (n < static_cast<int> (scratch.size()))
            n <<= 1;
        cueFft.prepare (n);
        cueBuf.assign (static_cast<size_t> (n), {});
        cueWindow.assign (scratch.size(), 0.0f);
    }
    frames.assign (kFrameRing, Frame {});
    marks.assign (kMarkRing, Mark {});

    lifterMax = static_cast<int> (std::lround (kLifterSeconds * fs));
    lifterUnvoiced = static_cast<int> (std::lround (kLifterUnvoicedSeconds * fs));
    const int maxHalfIn = static_cast<int> (std::ceil (1.6 * tMaxAbs)) + 2;
    const int maxHalfOut = tMaxAbs + 2;
    grain.prepare (fs, maxHalfIn, maxHalfOut, lifterMax);
    seg.assign (static_cast<size_t> (2 * maxHalfIn + 3), 0.0f);
    grainOut.assign (static_cast<size_t> (2 * maxHalfOut + 1), 0.0f);
    corr.assign (static_cast<size_t> (4 * tMaxAbs + 8), 0.0f);
    mono.assign (static_cast<size_t> (std::max (1, maxBlockSize)), 0.0f);

    configured = false;
    configure();
}

void Engine::setSettings (const Settings& s)
{
    const bool structural = ! configured
                            || s.pitchMin != settings.pitchMin
                            || s.pitchMax != settings.pitchMax;
    settings = s;
    if (structural)
        configure();
}

void Engine::configure()
{
    tMax = static_cast<int> (std::ceil (fs / settings.pitchMin));
    latency = 2 * tMax + lag * hop;
    // 無声の断片: 伸縮後 ±U、元は ±U × 倍率（最大 4）。U (1 + 4) が遅延 2 × tMax に収まるようにする
    unvoicedStep = std::max (8, std::min (static_cast<int> (std::lround (kUnvoicedSeconds * fs)), static_cast<int> (0.4 * tMax)));
    fadeSamples = std::max (1, static_cast<int> (std::ceil (kFadeSeconds * fs)));

    detector.configure (settings.pitchMin, settings.pitchMax);
    {
        const int wp = detector.windowLength();
        cueSize = 1;
        while (cueSize < wp)
            cueSize <<= 1;
        for (int i = 0; i < wp; ++i)
            cueWindow[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / wp));
    }
    tracker.reset (WarpMap::separateAnchors (settings.anchor));

    std::fill (xRing.begin(), xRing.end(), 0.0f);
    std::fill (dRing.begin(), dRing.end(), 0.0f);
    std::fill (accVoiced.begin(), accVoiced.end(), 0.0f);
    std::fill (accUnvoiced.begin(), accUnvoiced.end(), 0.0f);
    inPos = 0;
    dCount = 0;
    frameCount = 0;
    rawCount = 0;
    nextFrameAt = hop;
    levelPeak = -120.0f;
    rawPeak = -120.0f;
    markCount = 0;
    markCursor = 0;
    outPos = -latency;
    // 有声の流れは、印ができるまで進まない。始まりをブロックの区切りによらない時刻（0）にしておく
    // （最初の印は入力が hop 標本届いたときにでき、そのとき出力はまだ 0 より前なので、0 から始めて取りこぼさない）
    nextVoiced = 0.0;
    nextUnvoiced = static_cast<double> (outPos);
    theta = 0.0f;
    for (auto& w : warps)
        w.valid = false;
    grain.forgetEnvelope();
    configured = true;
}

void Engine::selectInput (const float* const* in, int numIn, int n, float* out)
{
    if (numIn <= 0)
    {
        std::fill (out, out + n, 0.0f);
        return;
    }
    if (numIn < 2)
    {
        std::copy (in[0], in[0] + n, out);
        stat.channel.store (0);
        return;
    }

    switch (settings.inputChannel)
    {
        case InputChannel::ch1: std::copy (in[0], in[0] + n, out); stat.channel.store (0); return;
        case InputChannel::ch2: std::copy (in[1], in[1] + n, out); stat.channel.store (1); return;
        case InputChannel::average:
            for (int i = 0; i < n; ++i)
                out[i] = 0.5f * (in[0][i] + in[1][i]);
            stat.channel.store (2);
            return;
        case InputChannel::automatic:
            break;
    }

    // 自動: 大きいほうを使う。もう片方が 2 秒続けて 6dB 以上大きいと切り替える
    const float a = static_cast<float> (std::exp (-n / (0.3 * fs)));
    for (int c = 0; c < 2; ++c)
    {
        double e = 0.0;
        for (int i = 0; i < n; ++i)
            e += static_cast<double> (in[c][i]) * in[c][i];
        chPower[c] = a * chPower[c] + (1.0f - a) * static_cast<float> (e / n);
    }
    const int other = 1 - channel;
    if (chPower[other] > chPower[channel] * kChannelSwitchRatio && chPower[other] > 1.0e-10f)
    {
        otherLouderFor += n;
        if (otherLouderFor >= kChannelSwitchSeconds * fs)
        {
            chPrev = channel;
            channel = other;
            chFade = 0.0f;
            otherLouderFor = 0;
        }
    }
    else
        otherLouderFor = 0;

    const float step = static_cast<float> (1.0 / (kFadeSeconds * fs));
    for (int i = 0; i < n; ++i)
    {
        if (chFade < 1.0f)
        {
            out[i] = chFade * in[channel][i] + (1.0f - chFade) * in[chPrev][i];
            chFade = std::min (1.0f, chFade + step);
        }
        else
            out[i] = in[channel][i];
    }
    stat.channel.store (channel);
}

void Engine::pushInput (float v)
{
    xRing[static_cast<size_t> (inPos & xMask)] = v;
    ++inPos;

    if (inPos % decim == 0)
    {
        float acc = 0.0f;
        const int taps = static_cast<int> (lowpass.size());
        for (int j = 0; j < taps; ++j)
            acc += lowpass[static_cast<size_t> (j)] * x (inPos - 1 - j);
        dRing[static_cast<size_t> (dCount & dMask)] = acc;
        ++dCount;
    }

    if (inPos >= nextFrameAt)
    {
        analyzeFrame();
        nextFrameAt += hop;
    }
}

void Engine::analyzeFrame()
{
    // いちばん新しい窓をピッチ検出に渡す。高さが決まるのは、先読みの分（lag フレーム）だけ前のフレーム
    const int wp = detector.windowLength();
    double e = 0.0;
    for (int i = 0; i < wp; ++i)
    {
        // 始まった直後で、まだ届いていない所は 0（輪は 0 で埋めてある）
        const int64_t k = dCount - wp + i;
        const float v = k < 0 ? 0.0f : dRing[static_cast<size_t> (k & dMask)];
        scratch[static_cast<size_t> (i)] = v;
        e += static_cast<double> (v) * v;
    }
    const float levelDb = static_cast<float> (10.0 * std::log10 (e / wp + 1.0e-20));
    pendingLevel[static_cast<size_t> (rawCount % kPendingRing)] = levelDb;
    ++rawCount;
    rawPeak = std::max (levelDb, rawPeak - kPeakDecayDbPerSec * static_cast<float> (hop / fs));
    float up = 0.0f, down = 0.0f;
    if (tuning.voicingCues)
        voicingCue (wp, levelDb, up, down);
    PitchResult pitch;
    if (detector.analyze (scratch.data(), up, down, pitch))
        finalizeFrame (pitch);
}

// 周期性以外の手がかりから、有声の観測に掛ける倍率（log10）を求める（scratch にピッチ検出の窓がある）。
// 声（母音）は高域が低域よりずっと弱く、さ行・は行などの摩擦音や息は高域が強い。最近の一番大きい音よりずっと小さい所は、息や背景の音とみる。
// 有声に寄せる分（up）は、声の出だし（音量が上がっていく途中）でだけ効かせる。黙っている間の低い背景の音も低域が強いため
void Engine::voicingCue (int wp, float levelDb, float& up, float& down)
{
    for (int i = 0; i < cueSize; ++i)
        cueBuf[static_cast<size_t> (i)] = i < wp ? Fft::Complex (scratch[static_cast<size_t> (i)] * cueWindow[static_cast<size_t> (i)], 0.0f) : Fft::Complex {};
    cueFft.transform (cueBuf.data(), cueSize, false);
    const double binHz = fsd / cueSize;
    const int lo0 = std::max (1, static_cast<int> (std::ceil (kCueLowLo / binHz))), lo1 = static_cast<int> (std::floor (kCueLowHi / binHz));
    const int hi0 = static_cast<int> (std::ceil (kCueHighLo / binHz)), hi1 = std::min (cueSize / 2, static_cast<int> (std::floor (0.4 * fsd / binHz)));
    double low = 0.0, high = 0.0;
    for (int k = lo0; k <= lo1; ++k)
        low += std::norm (cueBuf[static_cast<size_t> (k)]);
    for (int k = hi0; k <= hi1; ++k)
        high += std::norm (cueBuf[static_cast<size_t> (k)]);
    // 帯域の幅の違いはそのまま（声では高域がもともと弱いので、その差も手がかりの内）
    const float tilt = static_cast<float> (10.0 * std::log10 ((high + 1.0e-20) / (low + 1.0e-20)));
    const float t = std::clamp (-(tilt - kCueTiltCentre) * kCueTiltSlope, -1.0f, 1.0f);
    const float before = rawCount >= 3 ? pendingLevel[static_cast<size_t> ((rawCount - 3) % kPendingRing)] : -120.0f;
    const float rise = std::clamp ((levelDb - before - kCueRiseNone) / (kCueRiseFull - kCueRiseNone), 0.0f, 1.0f);
    up = std::max (0.0f, t) * rise;
    down = std::min (0.0f, t) + std::clamp ((levelDb - rawPeak + kCueQuietDb) * kCueQuietSlope, -1.0f, 0.0f);
}

void Engine::finalizeFrame (const PitchResult& pitch)
{
    // 落とした音の i 番目の標本の時刻（元の標本の番号）
    const int64_t newest = dCount - 1;
    Frame& f = frames[static_cast<size_t> (frameCount % kFrameRing)];
    f = Frame {};
    f.centre = frameCentre (frameCount);
    const float levelDb = pendingLevel[static_cast<size_t> (frameCount % kPendingRing)];
    levelPeak = std::max (levelDb, levelPeak - kPeakDecayDbPerSec * static_cast<float> (hop / fs));

    // F1〜F4: 窓の中心をフレームの中心にそろえる（まだ届いていなければ、いちばん新しい所で終わる窓）
    const int wt = tracker.windowLength();
    const bool usable = pitch.voiced && levelDb > kSilentDb && levelDb > levelPeak - kBelowPeakDb;
    const auto anchors = WarpMap::separateAnchors (settings.anchor);
    if (dCount >= wt)
    {
        int64_t end = static_cast<int64_t> (std::llround ((f.centre + lpDelay + 1.0) / decim - 1.0)) + (wt - 1) / 2;
        end = std::min (end, newest);
        for (int i = 0; i < wt; ++i)
            scratch[static_cast<size_t> (i)] = dRing[static_cast<size_t> ((end - wt + 1 + i) & dMask)];
        tracker.analyze (scratch.data(), usable, anchors, f.fm);
    }
    else
        tracker.analyze (scratch.data(), false, anchors, f.fm);
    f.voiced = pitch.voiced;
    f.period = pitch.voiced ? static_cast<float> (fs / pitch.f0) : 0.0f;
    stat.voicing[static_cast<size_t> (frameCount % EngineStatus::kVoicingHistory)].store (pitch.voiced);
    ++frameCount;
    stat.voicingCount.store (frameCount);
    stat.f0.store (pitch.voiced ? pitch.f0 : 0.0f);
    if (frameLog != nullptr)
        frameLog->push_back ({ static_cast<float> (f.centre / fs), pitch.voiced ? 1.0f : 0.0f, pitch.voiced ? pitch.f0 : 0.0f });
    for (int k = 0; k < 4; ++k)
    {
        stat.formant[static_cast<size_t> (k)].store (f.fm.f[static_cast<size_t> (k)]);
        stat.tracked[static_cast<size_t> (k)].store (f.fm.tracked[static_cast<size_t> (k)]);
    }
}

double Engine::frameCentre (int64_t j) const
{
    // j 番目のフレームは、入力が (j + 1) × hop 標本届いたときに分析する。そのときの落とした音の一番新しい標本から、窓の半分だけ前
    // （高さが決まるのは、さらに先読みの lag フレーム後）
    const int64_t dc = ((j + 1) * hop) / decim;
    return static_cast<double> (dc * decim - 1 - lpDelay) - 0.5 * (detector.windowLength() - 1) * decim;
}

int64_t Engine::nearestFrame (double t) const
{
    int64_t j = std::max<int64_t> (0, static_cast<int64_t> (std::llround ((t - frameCentre (0)) / hop)));
    if (j > 0 && std::abs (frameCentre (j - 1) - t) < std::abs (frameCentre (j) - t))
        --j;
    else if (std::abs (frameCentre (j + 1) - t) < std::abs (frameCentre (j) - t))
        ++j;
    return j;
}

int64_t Engine::inPosWhenKnown (double t) const
{
    // frameAt が使う（一番近い）フレームが決まる時刻
    return (nearestFrame (t) + 1 + lag) * hop;
}

const Engine::Frame& Engine::frameAt (double t, int64_t availInPos) const
{
    static const Frame none {};
    int64_t latest = frameCount - 1;
    if (availInPos != INT64_MAX)
        latest = std::min (latest, availInPos / hop - 1 - lag);
    if (latest < 0)
        return none;
    int64_t j = nearestFrame (t);
    j = std::clamp (j, std::max<int64_t> (0, frameCount - kFrameRing + 1), latest);
    return frames[static_cast<size_t> (j % kFrameRing)];
}

void Engine::placeMarks()
{
    if (frameCount == 0)
        return;
    auto push = [this] (int64_t t, float period, bool voiced, int64_t ready)
    {
        marks[static_cast<size_t> (markCount % kMarkRing)] = { t, period, voiced, ready };
        ++markCount;
    };
    if (markCount == 0)
    {
        // 最初の印は、最初のフレームの時刻に置く（ブロックの区切りによらないように）
        push (static_cast<int64_t> (std::floor (frameCentre (0))), 0.0f, false, (1 + lag) * hop);
        markCursor = 0;
    }

    for (int guard = 0; guard < 256; ++guard)
    {
        const Mark last = marks[static_cast<size_t> ((markCount - 1) % kMarkRing)];
        if (last.voiced)
        {
            if (nearestFrame (last.t + last.period) >= frameCount) // 使うフレームがまだ決まっていない
                break;
            const Frame& f = frameAt (last.t + last.period);
            const int64_t knownAt = std::max (last.ready, inPosWhenKnown (last.t + last.period));
            if (! f.voiced)
            {
                push (last.t + unvoicedStep, 0.0f, false, knownAt);
                continue;
            }
            // 前の印の 1 周期先を、波形の重なり（正規化した相互相関）で探す
            const float period = f.period;
            const int lw = std::max (8, static_cast<int> (std::lround (period)));
            const int64_t lo = last.t + static_cast<int64_t> (std::floor (0.8 * period));
            const int64_t hi = last.t + static_cast<int64_t> (std::ceil (1.2 * period));
            if (hi + lw / 2 >= inPos)
                break;
            const int64_t base = last.t - lw / 2;
            const int64_t span = hi + lw / 2 - base + 1;
            if (span > static_cast<int64_t> (corr.size()))
                break;
            for (int64_t i = 0; i < span; ++i)
                corr[static_cast<size_t> (i)] = x (base + i);
            double refE = 0.0;
            for (int i = 0; i < lw; ++i)
                refE += static_cast<double> (corr[static_cast<size_t> (i)]) * corr[static_cast<size_t> (i)];
            int64_t best = last.t + static_cast<int64_t> (std::lround (period));
            double bestScore = -2.0;
            for (int64_t c = lo; c <= hi; ++c)
            {
                const int64_t off = c - lw / 2 - base;
                double num = 0.0, e = 0.0;
                for (int i = 0; i < lw; ++i)
                {
                    const double v = corr[static_cast<size_t> (off + i)];
                    num += corr[static_cast<size_t> (i)] * v;
                    e += v * v;
                }
                const double score = num / std::sqrt (refE * e + 1.0e-30);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = c;
                }
            }
            push (best, period, true, std::max (knownAt, hi + lw / 2 + 1));
            if (markLog != nullptr)
                markLog->push_back ({ static_cast<double> (best), period, 1.0, bestScore });
        }
        else
        {
            const int64_t tn = last.t + unvoicedStep;
            if (nearestFrame (static_cast<double> (tn)) >= frameCount)
                break;
            const Frame& f = frameAt (static_cast<double> (tn));
            const int64_t knownAt = std::max (last.ready, inPosWhenKnown (static_cast<double> (tn)));
            if (! f.voiced)
            {
                push (tn, 0.0f, false, knownAt);
                continue;
            }
            // 有声の始まり: 1 周期の中で、声帯のパルスらしい所に印を置く。
            // そのフレームの LPC で声道の響きを除いた音（残差）の一番大きい所。LPC がなければ、元の音の一番大きい所。
            // （元の音のままだと、声の出だしで音量が上がっていく途中は、パルスでない響きの山を選ぶことがあり、
            //   以後の印もその位置を保つので、声の区間ごとにパルスに対する印の位置が運次第になっていた）
            const int len = static_cast<int> (std::lround (f.period));
            if (tn + len >= inPos)
                break;
            int64_t best = tn;
            int64_t readyAt = tn + len + 1;
            const int64_t d0 = static_cast<int64_t> (std::ceil ((static_cast<double> (tn) + lpDelay + 1.0) / decim - 1.0));
            const int64_t d1 = static_cast<int64_t> (std::floor ((static_cast<double> (tn + len - 1) + lpDelay + 1.0) / decim - 1.0));
            constexpr int order = FormantFrame::kLpcOrder;
            if (f.fm.lpcValid && d1 >= d0 && d0 - order - 1 >= 0 && d0 - order - 1 > dCount - static_cast<int64_t> (dRing.size()))
            {
                if (d1 >= dCount)
                    break;
                const auto dr = [this] (int64_t k) { return dRing[static_cast<size_t> (k & dMask)]; };
                float bestE = -1.0f;
                int64_t bestD = d0;
                for (int64_t d = d0; d <= d1; ++d)
                {
                    double e = 0.0;
                    for (int k = 0; k <= order; ++k)
                    {
                        const int64_t j = d - k;
                        const double y = dr (j) - kLpcPreEmphasis * dr (j - 1);
                        e += f.fm.lpc[static_cast<size_t> (k)] * y;
                    }
                    if (static_cast<float> (std::abs (e)) > bestE)
                    {
                        bestE = static_cast<float> (std::abs (e));
                        bestD = d;
                    }
                }
                // 落とした音の番号から、入力の時刻へ
                best = std::clamp<int64_t> ((bestD + 1) * decim - 1 - lpDelay, tn, tn + len - 1);
                readyAt = std::max (readyAt, (d1 + 1) * decim + lpDelay + 1);
            }
            else
            {
                float bestV = -1.0f;
                for (int i = 0; i < len; ++i)
                {
                    const float v = std::abs (x (tn + i));
                    if (v > bestV)
                    {
                        bestV = v;
                        best = tn + i;
                    }
                }
            }
            push (best, f.period, true, std::max (knownAt, readyAt));
            if (markLog != nullptr)
                markLog->push_back ({ static_cast<double> (best), f.period, 1.0, -2.0 });
        }
    }
}

float Engine::ratioFor (const Frame& f, int stream)
{
    // 写像は、入力（つまみ、差、目印、F4以降）が同じ流れの前の断片と同じなら作り直さない。
    // 無声の流れは、無声フォルマント・無声の差・無声F4以降を使う（目印の位置は有声と同じ）
    auto& w = warps[static_cast<size_t> (stream)];
    const bool v = stream == 0;
    const float formant = v ? settings.formant : settings.unvoicedFormant;
    const auto& delta = v ? settings.delta : settings.unvoicedDelta;
    const AboveF4 above = v ? settings.aboveF4 : settings.unvoicedAboveF4;
    const auto& anchors = settings.warpMode == WarpMode::formants ? f.fm.f : settings.anchor;
    if (! w.valid || formant != w.formant || delta != w.delta || anchors != w.anchors || above != w.above)
    {
        w.map.build (formant, delta, anchors, above);
        w.formant = formant;
        w.delta = delta;
        w.anchors = anchors;
        w.above = above;
        w.valid = true;
    }
    return std::clamp (w.map.uniformRatio(), kMinRatio, kMaxRatio);
}

bool Engine::voicedThroughout (int64_t from, int64_t to, int64_t availInPos) const
{
    // フレームは hop ごとなので、半分の間隔で見れば、間のフレームを見落とさない
    const int64_t step = std::max (1, hop / 2);
    for (int64_t t = from; t < to; t += step)
        if (! frameAt (static_cast<double> (t), availInPos).voiced)
            return false;
    return frameAt (static_cast<double> (to), availInPos).voiced;
}

bool Engine::makeGrain (int64_t centreIn, float halfIn, int halfOut, float ratio, const WarpMap* correction, int lifter, int stream, bool powerWindow)
{
    const int hi = static_cast<int> (std::ceil (halfIn));
    if (centreIn + hi >= inPos || 2 * hi + 1 > static_cast<int> (seg.size()))
        return false;
    float peak = 0.0f;
    for (int j = -hi; j <= hi; ++j)
    {
        const float v = x (centreIn + j);
        seg[static_cast<size_t> (hi + j)] = v;
        peak = std::max (peak, std::abs (v));
    }
    // 無音（-120dBFS より小さい）の断片は、作らずに 0 を出す（手前のゲートが閉じている間など。伸縮や補正の FFT を省く）
    if (peak < kSilentGrainPeak)
    {
        std::fill (grainOut.begin(), grainOut.begin() + 2 * halfOut + 1, 0.0f);
        return true;
    }
    grain.make (seg.data(), hi, halfIn, halfOut, ratio, correction, lifter, grainOut.data(), centreIn, stream, powerWindow);
    return true;
}

void Engine::writeGrain (std::vector<float>& acc, double centreOut, int halfOut, int64_t from)
{
    const int64_t c = static_cast<int64_t> (std::llround (centreOut));
    for (int i = 0; i <= 2 * halfOut; ++i)
    {
        const int64_t t = c - halfOut + i;
        if (t < from)
            continue;
        acc[static_cast<size_t> (t & accMask)] += grainOut[static_cast<size_t> (i)];
    }
}

void Engine::synthesize (int64_t o, int64_t e)
{
    const double p = std::exp2 (settings.pitch / 12.0);

    // 有声の流れ: 出力の印 nextVoiced に、その時刻より前の一番新しい元の印の断片を置く。
    // 使う印とフレームは、入力が「nextVoiced − 探索下限の周期 ＋ 遅延」まで届いた時点で決まっていたものに限る
    // （断片を作るのはそれより後なので、どのブロックの大きさでも同じものを使える）
    if (nextVoiced < static_cast<double> (o))
        nextVoiced = static_cast<double> (o);
    for (int guard = 0; guard < 8192 && markCount > 0; ++guard)
    {
        const int64_t avail = static_cast<int64_t> (std::floor (nextVoiced)) - tMax + latency;
        markCursor = std::max (markCursor, markCount - kMarkRing + 1);
        while (markCursor + 1 < markCount)
        {
            const Mark& next = marks[static_cast<size_t> ((markCursor + 1) % kMarkRing)];
            if (static_cast<double> (next.t) > nextVoiced || next.ready > avail)
                break;
            ++markCursor;
        }
        const Mark m = marks[static_cast<size_t> (markCursor % kMarkRing)];
        if (! m.voiced)
        {
            // 次に有声の断片が来ても、その書き始め（nextVoiced − 周期の上限）が次のブロックより前にならない所で止める
            if (nextVoiced - (tMax + 1) >= static_cast<double> (e))
                break;
            nextVoiced += unvoicedStep;
            continue;
        }
        const float ratio = ratioFor (frameAt (static_cast<double> (m.t), avail), 0);
        const int halfOut = std::max (2, static_cast<int> (std::lround (ratio >= 1.0f ? m.period / ratio : m.period)));
        const float halfIn = static_cast<float> (halfOut) * ratio;
        if (nextVoiced - halfOut >= static_cast<double> (e))
            break;
        const int lifter = std::min (static_cast<int> (std::lround (m.period * 0.5f)), lifterMax);
        if (! makeGrain (m.t, halfIn, halfOut, ratio, correctionFor (0), lifter, 0, false))
            break;
        writeGrain (accVoiced, nextVoiced, halfOut, o);
        if (grainLog != nullptr)
            grainLog->push_back ({ nextVoiced, static_cast<double> (m.t), m.period, 0.0 });
        nextVoiced += m.period / p;
    }

    // 無声の流れ: 一定の間隔 U で、伸縮後の長さを ±U にそろえる（重なりの和が一定）
    if (nextUnvoiced < static_cast<double> (o))
        nextUnvoiced = static_cast<double> (o);
    for (int guard = 0; guard < 8192; ++guard)
    {
        if (nextUnvoiced - unvoicedStep >= static_cast<double> (e))
            break;
        const int64_t c = static_cast<int64_t> (std::llround (nextUnvoiced));
        // 断片の範囲と、その前のつなぎの長さの間ずっと有声なら、無声の重みは 0 なので作らない
        const int64_t avail = c - unvoicedStep + latency; // 断片を作り始める時点で届いている入力
        if (voicedThroughout (c - unvoicedStep - fadeSamples, c + unvoicedStep, avail))
        {
            nextUnvoiced += unvoicedStep;
            continue;
        }
        const float ratio = ratioFor (frameAt (static_cast<double> (c), avail), 1);
        const float halfIn = static_cast<float> (unvoicedStep) * ratio;
        // 伸縮するときは、重なる 2 つの断片の中身が元の音の少しずれた位置から来て別物になる。ハン窓で重ねると、つなぎ目で強さが
        // 半分に下がり、1/U の周期の強弱の揺れ（ブーン、ピロピロ）になるので、強さの合計が一定になる窓（ハン窓の平方根）にする。
        // 切り出す位置も、0〜U/2 だけ前へ、時刻から決まる乱数でずらす。伸縮しないときは、どちらもしない（そのまま元に戻るため）
        const bool stretched = std::abs (ratio - 1.0f) > 1.0e-3f;
        int64_t from = c;
        if (stretched)
        {
            uint64_t h = static_cast<uint64_t> (c) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
            from -= static_cast<int64_t> (h % static_cast<uint64_t> (unvoicedStep / 2 + 1));
        }
        if (! makeGrain (from, halfIn, unvoicedStep, ratio, correctionFor (1), lifterUnvoiced, 1, stretched))
            break;
        writeGrain (accUnvoiced, nextUnvoiced, unvoicedStep, o);
        nextUnvoiced += unvoicedStep;
    }
}

void Engine::process (const float* const* in, int numIn, float* out, int n, float* delayedInput)
{
    for (int done = 0; done < n;)
    {
        const int len = std::min (n - done, static_cast<int> (mono.size()));
        const float* chans[2] = { nullptr, nullptr };
        for (int c = 0; c < std::min (numIn, 2); ++c)
            chans[c] = in[c] + done;
        selectInput (chans, std::min (numIn, 2), len, mono.data());
        for (int i = 0; i < len; ++i)
            pushInput (mono[static_cast<size_t> (i)]);

        placeMarks();
        const int64_t o = outPos;
        synthesize (o, o + len);

        constexpr float halfPi = static_cast<float> (kPi * 0.5);
        const float step = halfPi / static_cast<float> (fadeSamples);
        for (int i = 0; i < len; ++i)
        {
            const int64_t t = o + i;
            const bool voiced = frameAt (static_cast<double> (t)).voiced;
            theta = voiced ? std::min (halfPi, theta + step) : std::max (0.0f, theta - step);
            float& v = accVoiced[static_cast<size_t> (t & accMask)];
            float& u = accUnvoiced[static_cast<size_t> (t & accMask)];
            // つなぎ終わった所は、重みをちょうど 1 と 0 にする（無声の断片を作らなかった所に、前の断片の裾を混ぜない）
            if (theta >= halfPi)
                out[done + i] = v;
            else if (theta <= 0.0f)
                out[done + i] = u;
            else
                out[done + i] = std::sin (theta) * v + std::cos (theta) * u;
            v = 0.0f;
            u = 0.0f;
            if (delayedInput != nullptr)
                delayedInput[done + i] = x (t);
        }
        outPos += len;
        done += len;
    }
}

} // namespace vcab
