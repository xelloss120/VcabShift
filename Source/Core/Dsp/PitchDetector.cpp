#include "PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcab
{

namespace
{
constexpr float kSilentRms = 0.001f;  // -60dBFS より小さい窓は無声とする（有声の候補を出さない）
constexpr float kAbsentWeight = 0.01f; // しきい値を下回る谷がない段は、一番深い谷にこの割合だけ足す（pYIN と同じ）
constexpr float kYinTrust = 0.5f;     // 候補の確率の合計にかける重み（pYIN と同じ）
constexpr float kSwitch = 0.01f;      // 1 フレームで有声と無声が入れ替わる確率（pYIN と同じ）
// 1 フレームで動ける区画の数。pYIN は 5.8ms ごとに 25 区画（2.5 半音）。1 秒あたりで同じになるよう、5ms では 21 区画
constexpr int kNearJump = 21;
constexpr int kOctaveSpread = 3;      // オクターブ跳びの先の幅（±区画）
constexpr float kCueDipFull = 0.5f, kCueDipNone = 0.9f; // 有声に寄せる手がかりを効かせる、谷の深さ

// ベータ分布の確率密度（しきい値の重み。pYIN と同じ平均 0.15 の形: a = 2, b = 11 + 1/3）
double betaPdf (double x, double a, double b)
{
    return std::pow (x, a - 1.0) * std::pow (1.0 - x, b - 1.0);
}

// 三角形の重み（-w〜+w の合計が 1）
void triangle (std::vector<float>& out, int w, float total)
{
    out.assign (static_cast<size_t> (w + 1), 0.0f);
    double sum = 0.0;
    for (int d = -w; d <= w; ++d)
        sum += w + 1 - std::abs (d);
    for (int d = 0; d <= w; ++d)
        out[static_cast<size_t> (d)] = static_cast<float> (total * (w + 1 - d) / sum);
}
} // namespace

void PitchDetector::prepare (float sampleRate, float lowestMin, float highestMax, int lookaheadFrames, float octaveJumpProb)
{
    fs = sampleRate;
    lag = std::max (0, lookaheadFrames);
    octaveJump = std::clamp (octaveJumpProb, 0.0f, 0.5f);
    const int maxTau = static_cast<int> (std::ceil (fs / lowestMin)) + 2;
    diff.assign (static_cast<size_t> (maxTau + 2), 0.0f);
    cmnd.assign (diff.size(), 0.0f);
    tauProb.assign (diff.size(), 0.0f);

    thresholdWeight.resize (kThresholds);
    double sum = 0.0;
    for (int i = 0; i < kThresholds; ++i)
    {
        const double s = (i + 0.5) / kThresholds;
        thresholdWeight[static_cast<size_t> (i)] = static_cast<float> (betaPdf (s, 2.0, 11.0 + 1.0 / 3.0));
        sum += thresholdWeight[static_cast<size_t> (i)];
    }
    for (auto& w : thresholdWeight)
        w = static_cast<float> (w / sum);

    // 有声から有声へ: 近い区画へ (1 − J)（analyze で掛ける）、1 オクターブ上と下へ J / 2 ずつ
    triangle (nearWeight, kNearJump, 1.0f);
    triangle (octaveWeight, kOctaveSpread, 0.5f * octaveJump);

    maxBins = static_cast<int> (std::ceil (12.0 * kBinsPerSemitone * std::log2 (highestMax / lowestMin))) + 1;
    delta.assign (static_cast<size_t> (2 * maxBins), 0.0f);
    deltaNew.assign (delta.size(), 0.0f);
    obs.assign (delta.size(), 0.0f);
    for (auto* v : { &bestA, &bestB, &bestO })
        v->assign (static_cast<size_t> (maxBins), 0.0f);
    for (auto* v : { &argA, &argB, &argO, &live })
        v->assign (static_cast<size_t> (maxBins), 0);
    back.assign (static_cast<size_t> ((lag + 1) * 2 * maxBins), 0);
    cand.assign (static_cast<size_t> ((lag + 1) * kMaxCandidates), {});
    candCount.assign (static_cast<size_t> (lag + 1), 0);

    configure (80.0f, 400.0f);
}

void PitchDetector::configure (float fmin, float fmax)
{
    tauMax = std::min (static_cast<int> (std::ceil (fs / fmin)), static_cast<int> (diff.size()) - 2);
    tauMin = std::max (2, static_cast<int> (std::floor (fs / fmax)));
    binBase = fmin;
    numBins = std::clamp (static_cast<int> (std::ceil (12.0 * kBinsPerSemitone * std::log2 (fmax / fmin))) + 1, 1, maxBins);

    // 最初はどの状態も同じ確からしさ
    std::fill (delta.begin(), delta.end(), 0.0f);
    std::fill (delta.begin(), delta.begin() + 2 * numBins, 1.0f);
    std::fill (candCount.begin(), candCount.end(), 0);
    slot = 0;
    framesSeen = 0;
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

int PitchDetector::binOf (float f0) const
{
    return static_cast<int> (std::lround (12.0 * kBinsPerSemitone * std::log2 (f0 / binBase)));
}

// 1 フレームの観測: 候補（高さと区画）を c に、状態ごとの確率を obs に書く
void PitchDetector::observe (const float* x, float cueUp, float cueDown, Candidate* c, int& count)
{
    count = 0;
    const int n = windowLength();
    double e = 0.0;
    for (int i = 0; i < n; ++i)
        e += static_cast<double> (x[i]) * x[i];

    std::fill (obs.begin(), obs.begin() + 2 * numBins, 0.0f);
    float voicedMass = 0.0f;
    float deepest = 1.0f; // 一番深い谷（周期性の強さ）
    if (std::sqrt (e / n) >= kSilentRms)
    {
        // 差の関数（窓の長さは tauMax）と、累積平均で正規化したもの。
        // 比べる区間（i〜i + w + tau）を、ずらす量 tau によらず窓の中心にそろえる
        // （そろえないと、窓の前寄りの音で測ることになり、高さが (tauMax − tau) / 2 だけ古い時刻の値になる）
        const int w = tauMax;
        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            const float* xs = x + std::max (0, (tauMax - tau) / 2);
            float s = 0.0f;
            for (int i = 0; i < w; ++i)
            {
                const float d = xs[i] - xs[i + tau];
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

        // しきい値 (i + 1) / 100 ごとに「最初に下回った所から下った谷」を選び、その段の重みを足す。
        // しきい値が高い段ほど早く下回るので、短い周期から順に見て、まだ決まっていない高い段から決めていく
        std::fill (tauProb.begin(), tauProb.begin() + tauMax + 2, 0.0f);
        int globalMin = tauMin;
        float prefixMin = std::numeric_limits<float>::max();
        int undecided = kThresholds - 1; // これより上の段は決まった
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            const float v = cmnd[static_cast<size_t> (tau)];
            if (v < cmnd[static_cast<size_t> (globalMin)])
                globalMin = tau;
            if (v >= prefixMin || undecided < 0)
                continue;
            prefixMin = v;
            const int lowest = std::max (0, static_cast<int> (std::floor (v * kThresholds))); // (i + 1) / 100 > v となる一番低い段
            if (lowest > undecided)
                continue;
            int dip = tau;
            while (dip + 1 <= tauMax && cmnd[static_cast<size_t> (dip + 1)] < cmnd[static_cast<size_t> (dip)])
                ++dip;
            float wsum = 0.0f;
            for (int i = lowest; i <= undecided; ++i)
                wsum += thresholdWeight[static_cast<size_t> (i)];
            tauProb[static_cast<size_t> (dip)] += wsum;
            undecided = lowest - 1;
        }
        // どの谷も下回らなかった段は、一番深い谷に少しだけ
        float rest = 0.0f;
        for (int i = 0; i <= undecided; ++i)
            rest += thresholdWeight[static_cast<size_t> (i)];
        tauProb[static_cast<size_t> (globalMin)] += rest * kAbsentWeight;
        deepest = cmnd[static_cast<size_t> (globalMin)];

        for (int t = tauMin; t <= tauMax; ++t)
        {
            const float p = tauProb[static_cast<size_t> (t)];
            if (p <= 0.0f)
                continue;
            const float f0 = fs / refineTau (t);
            const int bin = std::clamp (binOf (f0), 0, numBins - 1);
            obs[static_cast<size_t> (bin)] += p * kYinTrust;
            voicedMass += p * kYinTrust;
            if (count < kMaxCandidates)
                c[count++] = { f0, bin };
        }
    }
    const float unvoiced = std::max (1.0f - voicedMass, 1.0e-6f) / static_cast<float> (numBins);
    std::fill (obs.begin() + numBins, obs.begin() + 2 * numBins, unvoiced);
    // 周期性以外の手がかり（本来の pYIN にはない）: 有声の観測だけに掛ける。
    // 有声に寄せる分は、周期性がある程度ある（一番深い谷が kCueDipFull で全部、kCueDipNone で 0）ときだけ効かせる
    const float up = cueUp * std::clamp ((kCueDipNone - deepest) / (kCueDipNone - kCueDipFull), 0.0f, 1.0f);
    const float logL = std::clamp (up + cueDown, -1.0f, 1.0f);
    if (logL != 0.0f)
    {
        const float L = std::pow (10.0f, logL);
        for (int b = 0; b < numBins; ++b)
            obs[static_cast<size_t> (b)] *= L;
    }
}

bool PitchDetector::analyze (const float* x, float cueUp, float cueDown, PitchResult& decided)
{
    const int slots = lag + 1;
    Candidate* c = &cand[static_cast<size_t> (slot * kMaxCandidates)];
    observe (x, cueUp, cueDown, c, candCount[static_cast<size_t> (slot)]);

    // ビタビ（確率のまま。フレームごとに一番大きい値で割って、小さくなりすぎないようにする）。
    // 状態 0〜N−1 が有声、N〜2N−1 が無声（同じ並びの高さの区画）。
    // 同じ側どうしは近い区画へ。入れ替わりは近い区画どうしで kSwitch。有声から有声へは 1 オクターブの跳びもある
    const int nb = numBins;
    int16_t* bp = &back[static_cast<size_t> (slot * 2 * maxBins)];
    const float stay = 1.0f - kSwitch;
    const int octave = 12 * kBinsPerSemitone;
    float mx = 0.0f;
    int arg = nb; // 何も残らなければ無声

    // 軽くするため、遷移の重みの共通の掛け算をくくり出し、3 つの「近い区画からの一番大きい値」を先に求める。
    // 有声の確率は、YIN の候補が出た区画（数個）にしか残らないので、残っている区画から広げる（全部の区画を調べない）。
    // 無声は、どの区画にも確率があるので、区画ごとに近い区画を調べる
    int numLive = 0;
    for (int pb = 0; pb < nb; ++pb)
        if (delta[static_cast<size_t> (pb)] > 0.0f)
            live[static_cast<size_t> (numLive++)] = pb;
    std::fill (bestA.begin(), bestA.begin() + nb, 0.0f);
    std::fill (bestO.begin(), bestO.begin() + nb, 0.0f);
    for (int k = 0; k < numLive; ++k)
    {
        const int pb = live[static_cast<size_t> (k)];
        const float dv = delta[static_cast<size_t> (pb)];
        const int lo = std::max (0, pb - kNearJump), hi = std::min (nb - 1, pb + kNearJump);
        for (int b = lo; b <= hi; ++b)
        {
            const float v = dv * nearWeight[static_cast<size_t> (std::abs (b - pb))];
            if (v > bestA[static_cast<size_t> (b)]) { bestA[static_cast<size_t> (b)] = v; argA[static_cast<size_t> (b)] = pb; }
        }
        if (octaveJump > 0.0f)
            for (int sign = -1; sign <= 1; sign += 2)
            {
                // 区画 b へは、b + sign × 1 オクターブの近く（±kOctaveSpread）から来る
                const int centre = pb - sign * octave;
                const int olo = std::max (0, centre - kOctaveSpread), ohi = std::min (nb - 1, centre + kOctaveSpread);
                for (int b = olo; b <= ohi; ++b)
                {
                    const float v = dv * octaveWeight[static_cast<size_t> (std::abs (b - centre))];
                    if (v > bestO[static_cast<size_t> (b)]) { bestO[static_cast<size_t> (b)] = v; argO[static_cast<size_t> (b)] = pb; }
                }
            }
    }
    for (int b = 0; b < nb; ++b)
    {
        const int lo = std::max (0, b - kNearJump), hi = std::min (nb - 1, b + kNearJump);
        float m = 0.0f;
        int am = b;
        for (int pb = lo; pb <= hi; ++pb)
        {
            const float v = delta[static_cast<size_t> (nb + pb)] * nearWeight[static_cast<size_t> (std::abs (pb - b))];
            if (v > m) { m = v; am = pb; }
        }
        bestB[static_cast<size_t> (b)] = m;
        argB[static_cast<size_t> (b)] = am;
    }

    const float stayVoiced = stay * (1.0f - octaveJump); // 有声から有声の近い区画へ
    for (int b = 0; b < nb; ++b)
    {
        const float a = bestA[static_cast<size_t> (b)], u = bestB[static_cast<size_t> (b)];
        // 有声 b へ: 有声の近い区画から、無声の近い区画から、有声のオクターブ違いから
        float bestV = 0.0f;
        int fromV = nb + b;
        if (a * stayVoiced > bestV) { bestV = a * stayVoiced; fromV = argA[static_cast<size_t> (b)]; }
        if (u * kSwitch > bestV) { bestV = u * kSwitch; fromV = nb + argB[static_cast<size_t> (b)]; }
        if (bestO[static_cast<size_t> (b)] * stay > bestV) { bestV = bestO[static_cast<size_t> (b)] * stay; fromV = argO[static_cast<size_t> (b)]; }
        // 無声 b へ: 無声の近い区画から、有声の近い区画から
        float bestU = 0.0f;
        int fromU = nb + b;
        if (u * stay > bestU) { bestU = u * stay; fromU = nb + argB[static_cast<size_t> (b)]; }
        if (a * kSwitch > bestU) { bestU = a * kSwitch; fromU = argA[static_cast<size_t> (b)]; }
        const float nv = bestV * obs[static_cast<size_t> (b)];
        const float nu = bestU * obs[static_cast<size_t> (nb + b)];
        deltaNew[static_cast<size_t> (b)] = nv;
        deltaNew[static_cast<size_t> (nb + b)] = nu;
        bp[b] = static_cast<int16_t> (fromV);
        bp[nb + b] = static_cast<int16_t> (fromU);
        if (nv > mx) { mx = nv; arg = b; }
        if (nu > mx) { mx = nu; arg = nb + b; }
    }
    if (mx > 0.0f && std::isfinite (mx))
    {
        const float inv = 1.0f / mx;
        for (int s = 0; s < 2 * nb; ++s)
            delta[static_cast<size_t> (s)] = deltaNew[static_cast<size_t> (s)] * inv;
    }
    else
    {
        // 起きないはず（無声の観測は必ず正）。念のため、どの状態も同じに戻す
        std::fill (delta.begin(), delta.begin() + 2 * nb, 1.0f);
        arg = nb;
    }

    const int current = slot;
    slot = (slot + 1) % slots;
    ++framesSeen;
    if (framesSeen <= lag)
        return false;

    // 先読みの分だけさかのぼる
    int state = arg;
    int s = current;
    for (int k = 0; k < lag; ++k)
    {
        state = back[static_cast<size_t> (s * 2 * maxBins + state)];
        s = (s - 1 + slots) % slots;
    }

    decided = {};
    if (state < nb)
    {
        decided.voiced = true;
        decided.f0 = binBase * std::exp2 (static_cast<float> (state) / (12.0f * kBinsPerSemitone));
        // その区画に近い候補があれば、その細かい値を使う（1 半音以内）
        const Candidate* cs = &cand[static_cast<size_t> (s * kMaxCandidates)];
        int bestDist = kBinsPerSemitone + 1;
        for (int i = 0; i < candCount[static_cast<size_t> (s)]; ++i)
        {
            const int d = std::abs (cs[i].bin - state);
            if (d < bestDist)
            {
                bestDist = d;
                decided.f0 = cs[i].f0;
            }
        }
    }
    return true;
}

} // namespace vcab
