#include "PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcab
{

namespace
{
constexpr float kSilentRms = 0.001f;   // -60dBFS より小さい窓は無声とする
constexpr float kThreshold = 0.15f;    // この値を最初に下回る谷を選ぶ
constexpr float kVoicedLimit = 0.25f;  // 選んだ谷がこれより深ければ有声
constexpr float kUnvoicedLimit = 0.35f; // 有声が続いていて高さも近いときは、選んだ谷がこれより浅くなるまで無声にしない
constexpr float kJumpSemitones = 5.0f; // 前のフレームからこれ以上跳んだら、間違いを疑う
constexpr int kMaxCorrections = 4;     // 飛び止めで続けて直せるフレームの数（これを超えたら本当に変わったとみなす）
} // namespace

void PitchDetector::prepare (float sampleRate, float lowestMin)
{
    fs = sampleRate;
    const int maxTau = static_cast<int> (std::ceil (fs / lowestMin)) + 2;
    diff.assign (static_cast<size_t> (maxTau + 2), 0.0f);
    cmnd.assign (diff.size(), 0.0f);
    configure (80.0f, 400.0f);
}

void PitchDetector::configure (float fmin, float fmax)
{
    tauMax = std::min (static_cast<int> (std::ceil (fs / fmin)), static_cast<int> (diff.size()) - 2);
    tauMin = std::max (2, static_cast<int> (std::floor (fs / fmax)));
    lastF0 = 0.0f;
}

// 放物線で谷の位置を細かく求める
float PitchDetector::refineTau (int tau) const
{
    if (tau <= 0 || tau >= static_cast<int> (cmnd.size()) - 1)
        return static_cast<float> (tau);
    const float a = cmnd[static_cast<size_t> (tau - 1)], b = cmnd[static_cast<size_t> (tau)], c = cmnd[static_cast<size_t> (tau + 1)];
    const float den = a - 2.0f * b + c;
    if (den <= 0.0f)
        return static_cast<float> (tau);
    return static_cast<float> (tau) + std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f);
}

PitchResult PitchDetector::analyze (const float* x)
{
    const int n = windowLength();
    double e = 0.0;
    for (int i = 0; i < n; ++i)
        e += static_cast<double> (x[i]) * x[i];
    if (std::sqrt (e / n) < kSilentRms)
    {
        lastF0 = 0.0f;
        return {};
    }

    // 差の関数（窓の長さは tauMax）と、累積平均で正規化したもの
    const int w = tauMax;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        float s = 0.0f;
        for (int i = 0; i < w; ++i)
        {
            const float d = x[i] - x[i + tau];
            s += d * d;
        }
        diff[static_cast<size_t> (tau)] = s;
    }
    cmnd[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        running += diff[static_cast<size_t> (tau)];
        cmnd[static_cast<size_t> (tau)] = running > 0.0 ? static_cast<float> (diff[static_cast<size_t> (tau)] * tau / running) : 1.0f;
    }

    // しきい値を最初に下回る谷。なければ一番深い谷
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (cmnd[static_cast<size_t> (tau)] < kThreshold)
        {
            while (tau + 1 <= tauMax && cmnd[static_cast<size_t> (tau + 1)] < cmnd[static_cast<size_t> (tau)])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        float m = std::numeric_limits<float>::max();
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (cmnd[static_cast<size_t> (tau)] < m)
            {
                m = cmnd[static_cast<size_t> (tau)];
                best = tau;
            }
    }

    PitchResult r;
    if (best >= 0)
    {
        const float depth = cmnd[static_cast<size_t> (best)];
        const float f = fs / refineTau (best);
        // ヒステリシス: 有声が続いていて高さも近ければ、kUnvoicedLimit まで有声のままにする（浅い谷で別の高さへ跳ばないように）
        const bool continues = lastF0 > 0.0f && depth < kUnvoicedLimit && std::abs (12.0f * std::log2 (f / lastF0)) < kJumpSemitones;
        if (depth < kVoicedLimit || continues)
        {
            r.voiced = true;
            r.f0 = f;
        }
    }

    // 飛び止め: 有声が続いている途中で大きく跳んだら、前の高さから ±kJumpSemitones の範囲で一番深い谷を探す。
    // 有声とみなせる深さならそちらに直し、なければ本当に変わったとみなす（間違った高さに張り付かないように）。
    // 周期 T の声には 2T にも同じくらい深い谷があるので、短い周期へ跳ぶときは、前の高さの谷のほうが深いときだけ直す。
    // 続けて直すのは kMaxCorrections フレームまで
    bool corrected = false;
    if (r.voiced && lastF0 > 0.0f && corrections < kMaxCorrections && std::abs (12.0f * std::log2 (r.f0 / lastF0)) >= kJumpSemitones)
    {
        const float range = std::exp2 (kJumpSemitones / 12.0f);
        const int lo = std::max (tauMin, static_cast<int> (std::ceil (fs / lastF0 / range)));
        const int hi = std::min (tauMax, static_cast<int> (std::floor (fs / lastF0 * range)));
        int near = -1;
        for (int tau = lo; tau <= hi; ++tau)
        {
            const float c = cmnd[static_cast<size_t> (tau)];
            const bool localMin = c <= cmnd[static_cast<size_t> (tau - 1)] && c <= cmnd[static_cast<size_t> (tau + 1)];
            if (localMin && (near < 0 || c < cmnd[static_cast<size_t> (near)]))
                near = tau;
        }
        if (near >= 0 && cmnd[static_cast<size_t> (near)] < kVoicedLimit
            && (near <= best || cmnd[static_cast<size_t> (near)] < cmnd[static_cast<size_t> (best)]))
        {
            r.f0 = fs / refineTau (near);
            corrected = true;
        }
    }
    corrections = corrected ? corrections + 1 : 0;
    lastF0 = r.voiced ? r.f0 : 0.0f;
    return r;
}

} // namespace vcab
