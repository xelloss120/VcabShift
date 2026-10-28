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
constexpr double kChannelSwitchSeconds = 2.0;
constexpr float kChannelSwitchRatio = 3.98f; // 6dB（パワー）
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

    detector.prepare (static_cast<float> (fsd), static_cast<float> (kLowestPitch));
    hop = static_cast<int> (std::lround (kHopSeconds * fs));
    tracker.prepare (static_cast<float> (fsd), static_cast<float> (hop / fs));
    scratch.assign (static_cast<size_t> (std::max (2 * (static_cast<int> (std::ceil (fsd / kLowestPitch)) + 4) + 2,
                                                   tracker.windowLength()) + 8), 0.0f);
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
    latency = 2 * tMax;
    // 無声の断片: 伸縮後 ±U、元は ±U × 倍率（最大 4）。U (1 + 4) が遅延 2 × tMax に収まるようにする
    unvoicedStep = std::max (8, std::min (static_cast<int> (std::lround (kUnvoicedSeconds * fs)), static_cast<int> (0.4 * tMax)));
    fadeSamples = std::max (1, static_cast<int> (std::ceil (kFadeSeconds * fs)));

    detector.configure (settings.pitchMin, settings.pitchMax);
    tracker.reset (WarpMap::separateAnchors (settings.anchor));

    std::fill (xRing.begin(), xRing.end(), 0.0f);
    std::fill (dRing.begin(), dRing.end(), 0.0f);
    std::fill (accVoiced.begin(), accVoiced.end(), 0.0f);
    std::fill (accUnvoiced.begin(), accUnvoiced.end(), 0.0f);
    inPos = 0;
    dCount = 0;
    frameCount = 0;
    nextFrameAt = hop;
    levelPeak = -120.0f;
    markCount = 0;
    markCursor = 0;
    outPos = -latency;
    nextVoiced = nextUnvoiced = static_cast<double> (outPos);
    theta = 0.0f;
    warpValid = false;
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
    // 落とした音の i 番目の標本の時刻（元の標本の番号）
    auto dTime = [this] (int64_t i) { return static_cast<double> ((i + 1) * decim - 1 - lpDelay); };

    const int wp = detector.windowLength();
    const int64_t newest = dCount - 1;
    Frame& f = frames[static_cast<size_t> (frameCount % kFrameRing)];
    f = Frame {};
    f.centre = dTime (newest) - 0.5 * (wp - 1) * decim;

    PitchResult pitch;
    float levelDb = -120.0f;
    if (dCount >= wp)
    {
        double e = 0.0;
        for (int i = 0; i < wp; ++i)
        {
            const float v = dRing[static_cast<size_t> ((dCount - wp + i) & dMask)];
            scratch[static_cast<size_t> (i)] = v;
            e += static_cast<double> (v) * v;
        }
        levelDb = static_cast<float> (10.0 * std::log10 (e / wp + 1.0e-20));
        pitch = detector.analyze (scratch.data());
    }
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
    ++frameCount;
    stat.f0.store (pitch.voiced ? pitch.f0 : 0.0f);
    for (int k = 0; k < 4; ++k)
    {
        stat.formant[static_cast<size_t> (k)].store (f.fm.f[static_cast<size_t> (k)]);
        stat.tracked[static_cast<size_t> (k)].store (f.fm.tracked[static_cast<size_t> (k)]);
    }
}

const Engine::Frame& Engine::frameAt (double t) const
{
    static const Frame none {};
    const int64_t latest = frameCount - 1;
    if (latest < 0)
        return none;
    const Frame& last = frames[static_cast<size_t> (latest % kFrameRing)];
    int64_t j = latest + static_cast<int64_t> (std::llround ((t - last.centre) / hop));
    j = std::clamp (j, std::max<int64_t> (0, frameCount - kFrameRing + 1), latest);
    return frames[static_cast<size_t> (j % kFrameRing)];
}

void Engine::placeMarks()
{
    if (frameCount == 0)
        return;
    const double known = frames[static_cast<size_t> ((frameCount - 1) % kFrameRing)].centre;
    auto push = [this] (int64_t t, float period, bool voiced)
    {
        marks[static_cast<size_t> (markCount % kMarkRing)] = { t, period, voiced };
        ++markCount;
    };
    if (markCount == 0)
    {
        push (static_cast<int64_t> (std::floor (known)), 0.0f, false);
        markCursor = 0;
    }

    for (int guard = 0; guard < 256; ++guard)
    {
        const Mark last = marks[static_cast<size_t> ((markCount - 1) % kMarkRing)];
        if (last.voiced)
        {
            if (last.t + last.period > known)
                break;
            const Frame& f = frameAt (last.t + last.period);
            if (! f.voiced)
            {
                push (last.t + unvoicedStep, 0.0f, false);
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
            push (best, period, true);
        }
        else
        {
            const int64_t tn = last.t + unvoicedStep;
            if (tn > known)
                break;
            const Frame& f = frameAt (static_cast<double> (tn));
            if (! f.voiced)
            {
                push (tn, 0.0f, false);
                continue;
            }
            // 有声の始まり: 1 周期の中で一番大きい所に印を置く
            const int len = static_cast<int> (std::lround (f.period));
            if (tn + len >= inPos)
                break;
            int64_t best = tn;
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
            push (best, f.period, true);
        }
    }
}

float Engine::ratioFor (const Frame& f)
{
    // 写像は、入力（つまみ、差、目印、F4以降）が前の断片と同じなら作り直さない
    const auto& anchors = settings.warpMode == WarpMode::formants ? f.fm.f : settings.anchor;
    if (! warpValid || settings.formant != warpFormant || settings.delta != warpDelta
        || anchors != warpAnchors || settings.aboveF4 != warpAbove)
    {
        warp.build (settings.formant, settings.delta, anchors, settings.aboveF4);
        warpFormant = settings.formant;
        warpDelta = settings.delta;
        warpAnchors = anchors;
        warpAbove = settings.aboveF4;
        warpValid = true;
    }
    return std::clamp (warp.uniformRatio(), kMinRatio, kMaxRatio);
}

bool Engine::voicedThroughout (int64_t from, int64_t to) const
{
    // フレームは hop ごとなので、半分の間隔で見れば、間のフレームを見落とさない
    const int64_t step = std::max (1, hop / 2);
    for (int64_t t = from; t < to; t += step)
        if (! frameAt (static_cast<double> (t)).voiced)
            return false;
    return frameAt (static_cast<double> (to)).voiced;
}

bool Engine::makeGrain (int64_t centreIn, float halfIn, int halfOut, float ratio, bool correct, int lifter)
{
    const int hi = static_cast<int> (std::ceil (halfIn));
    if (centreIn + hi >= inPos || 2 * hi + 1 > static_cast<int> (seg.size()))
        return false;
    for (int j = -hi; j <= hi; ++j)
        seg[static_cast<size_t> (hi + j)] = x (centreIn + j);
    grain.make (seg.data(), hi, halfIn, halfOut, ratio, correct ? &warp : nullptr, lifter, grainOut.data(), centreIn);
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

    // 有声の流れ: 出力の印 nextVoiced に、その時刻より前の一番新しい元の印の断片を置く
    if (nextVoiced < static_cast<double> (o))
        nextVoiced = static_cast<double> (o);
    for (int guard = 0; guard < 8192 && markCount > 0; ++guard)
    {
        markCursor = std::max (markCursor, markCount - kMarkRing + 1);
        while (markCursor + 1 < markCount && static_cast<double> (marks[static_cast<size_t> ((markCursor + 1) % kMarkRing)].t) <= nextVoiced)
            ++markCursor;
        const Mark m = marks[static_cast<size_t> (markCursor % kMarkRing)];
        if (! m.voiced)
        {
            if (nextVoiced >= static_cast<double> (e))
                break;
            nextVoiced += unvoicedStep;
            continue;
        }
        const float ratio = ratioFor (frameAt (static_cast<double> (m.t)));
        const int halfOut = std::max (2, static_cast<int> (std::lround (ratio >= 1.0f ? m.period / ratio : m.period)));
        const float halfIn = static_cast<float> (halfOut) * ratio;
        if (nextVoiced - halfOut >= static_cast<double> (e))
            break;
        const int lifter = std::min (static_cast<int> (std::lround (m.period * 0.5f)), lifterMax);
        if (! makeGrain (m.t, halfIn, halfOut, ratio, ! warp.isUniform(), lifter))
            break;
        writeGrain (accVoiced, nextVoiced, halfOut, o);
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
        if (voicedThroughout (c - unvoicedStep - fadeSamples, c + unvoicedStep))
        {
            nextUnvoiced += unvoicedStep;
            continue;
        }
        const float ratio = ratioFor (frameAt (static_cast<double> (c)));
        const float halfIn = static_cast<float> (unvoicedStep) * ratio;
        if (! makeGrain (c, halfIn, unvoicedStep, ratio, ! warp.isUniform(), lifterUnvoiced))
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
