#include "FormantTracker.h"

#include "../Params.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcab
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kPreEmphasis = 0.97;
constexpr double kMaxBandwidth = 700.0;  // Hz。これより広い根は山とみなさない
constexpr double kHoldSeconds = 0.150;
constexpr double kGlideSeconds = 0.050;
constexpr double kSmoothSeconds = 0.010;
constexpr double kF4OverF3 = 3500.0 / 2700.0;
// F1・F2 が 1 つにまとまった山の扱い
constexpr double kMergeLo = 550.0, kMergeHi = 1300.0, kMergeMaxBw = 400.0, kMergeRatio = 1.3, kMergePenaltyBw = 300.0;
constexpr int kMaxPeaks = 32;

} // namespace

void FormantTracker::prepare (float sampleRate, float hopSeconds)
{
    fs = sampleRate;
    hop = hopSeconds;
    window = static_cast<int> (std::lround (0.025 * fs));
    order = std::clamp (static_cast<int> (std::lround (fs / 1000.0)) + 2, 12, 24);

    hamming.resize (static_cast<size_t> (window));
    for (int i = 0; i < window; ++i)
        hamming[static_cast<size_t> (i)] = 0.54 - 0.46 * std::cos (2.0 * kPi * i / (window - 1));
    buf.assign (static_cast<size_t> (window), 0.0);
    autoc.assign (static_cast<size_t> (order + 1), 0.0);
    lpc.assign (static_cast<size_t> (order + 1), 0.0);
    tmp.assign (static_cast<size_t> (order + 1), 0.0);
    roots.assign (static_cast<size_t> (order), C {});
    reset ({ 500.0f, 1400.0f, 2700.0f, 3500.0f });
}

void FormantTracker::reset (const std::array<float, 4>& anchors)
{
    for (int i = 0; i < 4; ++i)
    {
        value[static_cast<size_t> (i)] = anchors[static_cast<size_t> (i)];
        missing[static_cast<size_t> (i)] = std::numeric_limits<int>::max() / 2;
        glideFrom[static_cast<size_t> (i)] = anchors[static_cast<size_t> (i)];
        prevTracked[static_cast<size_t> (i)] = false;
    }
    rootsValid = false;
}

// 高域強調、ハミング窓、自己相関、レビンソン・ダービン
bool FormantTracker::computeLpc (const float* x)
{
    double prev = x[0];
    for (int i = 0; i < window; ++i)
    {
        const double v = x[i] - kPreEmphasis * (i > 0 ? prev : x[0]);
        prev = x[i];
        buf[static_cast<size_t> (i)] = v * hamming[static_cast<size_t> (i)];
    }
    for (int k = 0; k <= order; ++k)
    {
        double s = 0.0;
        for (int i = k; i < window; ++i)
            s += buf[static_cast<size_t> (i)] * buf[static_cast<size_t> (i - k)];
        autoc[static_cast<size_t> (k)] = s;
    }
    if (autoc[0] <= 1.0e-12)
        return false;
    autoc[0] *= 1.0 + 1.0e-9; // 計算の安定のため、ごくわずかに白色雑音を足す

    std::fill (lpc.begin(), lpc.end(), 0.0);
    lpc[0] = 1.0;
    double err = autoc[0];
    for (int i = 1; i <= order; ++i)
    {
        double acc = autoc[static_cast<size_t> (i)];
        for (int j = 1; j < i; ++j)
            acc += lpc[static_cast<size_t> (j)] * autoc[static_cast<size_t> (i - j)];
        const double k = -acc / err;
        for (int j = 0; j <= i; ++j)
            tmp[static_cast<size_t> (j)] = lpc[static_cast<size_t> (j)];
        for (int j = 1; j < i; ++j)
            lpc[static_cast<size_t> (j)] = tmp[static_cast<size_t> (j)] + k * tmp[static_cast<size_t> (i - j)];
        lpc[static_cast<size_t> (i)] = k;
        err *= 1.0 - k * k;
        if (err <= 0.0)
            return false;
    }
    return true;
}

// デュラン・ケルナー法で z^p + a1 z^(p-1) + ... + ap の根を求める（前のフレームの根から始める）
void FormantTracker::findRoots()
{
    const int p = order;
    if (! rootsValid)
        for (int k = 0; k < p; ++k)
            roots[static_cast<size_t> (k)] = std::polar (0.9, 2.0 * kPi * k / p + 0.4);

    auto poly = [this, p] (C z)
    {
        C v = 1.0;
        for (int j = 1; j <= p; ++j)
            v = v * z + lpc[static_cast<size_t> (j)];
        return v;
    };

    bool converged = false;
    for (int iter = 0; iter < 80 && ! converged; ++iter)
    {
        double maxStep = 0.0;
        for (int k = 0; k < p; ++k)
        {
            const C zk = roots[static_cast<size_t> (k)];
            C den = 1.0;
            for (int j = 0; j < p; ++j)
                if (j != k)
                    den *= zk - roots[static_cast<size_t> (j)];
            if (std::abs (den) < 1.0e-300)
                den = 1.0e-12;
            const C step = poly (zk) / den;
            roots[static_cast<size_t> (k)] = zk - step;
            maxStep = std::max (maxStep, std::abs (step));
        }
        converged = maxStep < 1.0e-9;
    }
    rootsValid = converged;
    for (const auto& r : roots)
        if (! std::isfinite (r.real()) || ! std::isfinite (r.imag()))
            rootsValid = false;
}

void FormantTracker::analyze (const float* x, bool usable, const std::array<float, 4>& anchors, FormantFrame& out)
{
    Peak peaks[kMaxPeaks];
    int numPeaks = 0;
    // 1 つにまとまった山を分けた組（F1・F2 として一緒に使う）
    Peak splits[kMaxPeaks][2];
    int splitOf[kMaxPeaks];
    int numSplits = 0;

    bool found4 = false, found3 = false;
    std::array<double, 4> best {};

    if (usable && computeLpc (x))
    {
        findRoots();
        if (rootsValid)
        {
            for (const auto& r : roots)
            {
                if (r.imag() <= 0.0 || numPeaks >= kMaxPeaks)
                    continue;
                const double f = std::arg (r) * fs / (2.0 * kPi);
                const double bw = -std::log (std::abs (r)) * fs / kPi;
                if (f < 150.0 || f > fs * 0.5 - 200.0 || bw >= kMaxBandwidth || bw <= 0.0)
                    continue;
                peaks[numPeaks++] = { f, bw };
            }
            std::sort (peaks, peaks + numPeaks, [] (const Peak& a, const Peak& b) { return a.f < b.f; });
            for (int i = 0; i < numPeaks; ++i)
            {
                if (peaks[i].f >= kMergeLo && peaks[i].f <= kMergeHi && peaks[i].bw < kMergeMaxBw && numSplits < kMaxPeaks)
                {
                    splits[numSplits][0] = { peaks[i].f / kMergeRatio, peaks[i].bw + kMergePenaltyBw };
                    splits[numSplits][1] = { peaks[i].f * kMergeRatio, peaks[i].bw + kMergePenaltyBw };
                    splitOf[numSplits] = i;
                    ++numSplits;
                }
            }

            auto cost = [this] (int k, const Peak& p)
            {
                const auto& range = kFormantRanges[static_cast<size_t> (k)];
                if (p.f < range.lo || p.f > range.hi)
                    return std::numeric_limits<double>::infinity();
                double c = std::abs (std::log (p.f / range.target)) + p.bw / 1000.0;
                if (prevTracked[static_cast<size_t> (k)])
                    c += 2.0 * std::abs (std::log (p.f / value[static_cast<size_t> (k)]));
                return c;
            };

            double bestCost4 = std::numeric_limits<double>::infinity();
            double bestCost3 = std::numeric_limits<double>::infinity();
            std::array<double, 4> best3 {};

            // F3・F4 を、index より上の山から選ぶ（exclude の山は使わない）
            auto chooseUpper = [&] (double c12, double f1, double f2, int start, int exclude)
            {
                for (int k = start; k < numPeaks; ++k)
                {
                    if (k == exclude || peaks[k].f <= f2)
                        continue;
                    const double c3 = c12 + cost (2, peaks[k]);
                    if (! std::isfinite (c3))
                        continue;
                    if (c3 < bestCost3)
                    {
                        bestCost3 = c3;
                        best3 = { f1, f2, peaks[k].f, 0.0 };
                    }
                    for (int l = k + 1; l < numPeaks; ++l)
                    {
                        if (l == exclude)
                            continue;
                        const double c4 = c3 + cost (3, peaks[l]);
                        if (c4 < bestCost4)
                        {
                            bestCost4 = c4;
                            best = { f1, f2, peaks[k].f, peaks[l].f };
                        }
                    }
                }
            };

            for (int i = 0; i < numPeaks; ++i)
            {
                const double c1 = cost (0, peaks[i]);
                if (! std::isfinite (c1))
                    continue;
                for (int j = i + 1; j < numPeaks; ++j)
                {
                    const double c12 = c1 + cost (1, peaks[j]);
                    if (std::isfinite (c12))
                        chooseUpper (c12, peaks[i].f, peaks[j].f, j + 1, -1);
                }
            }
            for (int s = 0; s < numSplits; ++s)
            {
                const double c12 = cost (0, splits[s][0]) + cost (1, splits[s][1]);
                if (std::isfinite (c12))
                    chooseUpper (c12, splits[s][0].f, splits[s][1].f, 0, splitOf[s]);
            }

            found4 = std::isfinite (bestCost4);
            found3 = std::isfinite (bestCost3);
            if (! found4 && found3)
                best = best3;
        }
    }

    const double alpha = 1.0 - std::exp (-hop / kSmoothSeconds);
    const int holdFrames = static_cast<int> (std::lround (kHoldSeconds / hop));
    const int glideFrames = std::max (1, static_cast<int> (std::lround (kGlideSeconds / hop)));

    for (int k = 0; k < 4; ++k)
    {
        const auto uk = static_cast<size_t> (k);
        bool tracked = (found4 || found3) && (k < 3 || found4);
        double target = best[uk];
        if (k == 3 && ! found4 && found3)
        {
            // F4 だけ取れないときは、F3 の一定の比の位置に置く
            target = std::min (static_cast<double> (kFormantRanges[3].hi), best[2] * kF4OverF3);
            tracked = false;
            missing[uk] = 0;
            value[uk] = std::exp (std::log (value[uk]) + alpha * (std::log (target) - std::log (value[uk])));
            prevTracked[uk] = false;
            continue;
        }

        if (tracked)
        {
            const bool wasHeld = missing[uk] <= holdFrames;
            if (prevTracked[uk] || wasHeld)
                value[uk] = std::exp (std::log (value[uk]) + alpha * (std::log (target) - std::log (value[uk])));
            else
                value[uk] = target;
            missing[uk] = 0;
        }
        else
        {
            ++missing[uk];
            if (missing[uk] == holdFrames + 1)
                glideFrom[uk] = value[uk];
            if (missing[uk] > holdFrames)
            {
                const double t = std::min (1.0, static_cast<double> (missing[uk] - holdFrames) / glideFrames);
                value[uk] = std::exp (std::log (glideFrom[uk]) + t * (std::log (static_cast<double> (anchors[uk])) - std::log (glideFrom[uk])));
            }
        }
        prevTracked[uk] = tracked;
        out.tracked[uk] = tracked;
    }
    out.tracked[3] = found4;
    for (int k = 0; k < 4; ++k)
        out.f[static_cast<size_t> (k)] = static_cast<float> (value[static_cast<size_t> (k)]);
}

} // namespace vcab
