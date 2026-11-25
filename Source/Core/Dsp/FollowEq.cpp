#include "FollowEq.h"
#include "MathConstants.h"
#include "WarpMap.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kUpdateSamples = 16;          // 係数を求め直す間隔
constexpr double kFreqSeconds = 0.005;      // 中心のならし（フレームの間隔 5ms で段になって動くのを、なめらかにする）
constexpr double kGainSeconds = 0.020;      // 量のならし（つまみを動かしたとき）
constexpr float kMinHz = 20.0f;
constexpr float kMaxFraction = 0.45f;       // 中心の上限（標本化周波数に対して）
// 棚形の Q。0.707 より少し急にして、点の所で量に近い分だけ下がるようにする（行き過ぎは量の 1% ほど）
constexpr double kShelfQ = 0.8;

float qForBandwidth (float octaves)
{
    return static_cast<float> (1.0 / (2.0 * std::sinh (std::log (2.0) * 0.5 * octaves)));
}
} // namespace

std::array<float, 4> eqAnchors (const Settings& s, const std::array<float, 4>& tracked)
{
    return WarpMap::separateAnchors (s.warpMode == WarpMode::bands ? s.anchor : tracked);
}

std::array<float, 4> eqFormantCentres (const Settings& s, const std::array<float, 4>& anchors, int stream)
{
    const bool v = stream == 0;
    const float formant = v ? s.formant : s.unvoicedFormant;
    const auto& delta = v ? s.delta : s.unvoicedDelta;
    std::array<float, 4> c {};
    for (size_t k = 0; k < 4; ++k)
        c[k] = anchors[k] * std::exp2 ((formant + delta[k]) / 12.0f);
    return c;
}

float eqClampHz (float hz, double fs)
{
    return std::clamp (hz, kMinHz, static_cast<float> (kMaxFraction * fs));
}

namespace
{
bool useShelf (EqShelf shelf, float gainDb)
{
    return shelf != EqShelf::none && gainDb < 0.0f;
}

// 棚形の折れ目（中心から内側へ幅の半分。棚形が半分の量になる所を、ベル形が半分の量になる所にそろえる）
float shelfOffsetOct (EqShelf shelf, float bandwidthOct)
{
    return (shelf == EqShelf::low ? 0.5f : -0.5f) * bandwidthOct;
}
} // namespace

float eqBandDb (float f, float centre, float gainDb, float bandwidthOct, EqShelf shelf, double fs)
{
    if (gainDb == 0.0f)
        return 0.0f;
    const double fe = std::min (static_cast<double> (f), 0.4999 * fs);
    const double a = std::pow (10.0, gainDb / 40.0);
    if (useShelf (shelf, gainDb))
    {
        // SVF の棚形は、アナログの低域の棚 A (s² + (√A/Q)s + A) / (A s² + (√A/Q)s + 1)（高域の棚は A (A s² + (√A/Q)s + 1) / (s² + (√A/Q)s + A)）を、
        // 折れ目で合わせて双一次変換したもの
        const double fc = eqClampHz (eqClampHz (centre, fs) * std::exp2 (shelfOffsetOct (shelf, bandwidthOct)), fs);
        const double w = std::tan (kPi * fe / fs) / std::tan (kPi * fc / fs);
        const double mid = a * w * w / (kShelfQ * kShelfQ);
        const double p = (a - w * w) * (a - w * w) + mid;
        const double r = (1.0 - a * w * w) * (1.0 - a * w * w) + mid;
        return static_cast<float> (10.0 * std::log10 (a * a * (shelf == EqShelf::low ? p / r : r / p)));
    }
    // SVF のベル形は、アナログの (s² + (A/Q)s + 1) / (s² + s/(AQ) + 1) を、中心で合わせて双一次変換したもの
    const double fc = eqClampHz (centre, fs);
    const double w = std::tan (kPi * fe / fs) / std::tan (kPi * fc / fs);
    const double q = qForBandwidth (bandwidthOct);
    const double re = 1.0 - w * w;
    const double num = re * re + (a / q * w) * (a / q * w);
    const double den = re * re + (w / (a * q)) * (w / (a * q));
    return static_cast<float> (10.0 * std::log10 (num / den));
}

void FollowEq::prepare (double sampleRate, int numBands, const std::array<float, kMaxBands>& bandwidthOct,
                        const std::array<EqShelf, kMaxBands>& shelf)
{
    fs = sampleRate;
    bands = std::clamp (numBands, 0, kMaxBands);
    for (size_t i = 0; i < kMaxBands; ++i)
    {
        band[i].q = qForBandwidth (bandwidthOct[i]);
        band[i].shelf = shelf[i];
        band[i].shelfOffset = shelfOffsetOct (shelf[i], bandwidthOct[i]);
    }
    freqAlpha = static_cast<float> (1.0 - std::exp (-kUpdateSamples / (kFreqSeconds * fs)));
    gainAlpha = static_cast<float> (1.0 - std::exp (-kUpdateSamples / (kGainSeconds * fs)));
    reset();
}

void FollowEq::reset()
{
    for (auto& b : band)
    {
        const float q = b.q, offset = b.shelfOffset;
        const EqShelf shelf = b.shelf;
        b = Band {};
        b.q = q;
        b.shelfOffset = offset;
        b.shelf = shelf;
    }
    countdown = 0;
}

void FollowEq::setTarget (int i, float hz, float db)
{
    auto& b = band[static_cast<size_t> (i)];
    if (hz > 0.0f)
    {
        b.targetLog = std::log2 (eqClampHz (hz, fs));
        if (! b.started)
        {
            b.log = b.targetLog;
            b.started = true;
        }
    }
    b.targetDb = db;
}

void FollowEq::updateCoefficients()
{
    for (int i = 0; i < bands; ++i)
    {
        auto& b = band[static_cast<size_t> (i)];
        b.log += freqAlpha * (b.targetLog - b.log);
        b.db += gainAlpha * (b.targetDb - b.db);
        if (std::abs (b.targetDb - b.db) < 0.01f)
            b.db = b.targetDb; // 狙いに着いたら止める（0 に戻したときに、ちょうど素通しにするため）
        b.on = b.db != 0.0f && b.started;
        if (! b.on)
        {
            b.ic1 = 0.0f;
            b.ic2 = 0.0f;
            continue;
        }
        const double a = std::pow (10.0, b.db / 40.0);
        double g = 0.0, k = 0.0;
        if (useShelf (b.shelf, b.db))
        {
            // 棚形（Simper）。量がならしの途中で 0 をまたぐと形が替わるが、0 の近くではどちらもほぼ素通しなので段は出ない
            const double t = std::tan (kPi * eqClampHz (std::exp2 (b.log + b.shelfOffset), fs) / fs);
            k = 1.0 / kShelfQ;
            if (b.shelf == EqShelf::low)
            {
                g = t / std::sqrt (a);
                b.m0 = 1.0f;
                b.m1 = static_cast<float> (k * (a - 1.0));
                b.m2 = static_cast<float> (a * a - 1.0);
            }
            else
            {
                g = t * std::sqrt (a);
                b.m0 = static_cast<float> (a * a);
                b.m1 = static_cast<float> (k * (1.0 - a) * a);
                b.m2 = static_cast<float> (1.0 - a * a);
            }
        }
        else
        {
            g = std::tan (kPi * std::exp2 (static_cast<double> (b.log)) / fs);
            k = 1.0 / (b.q * a);
            b.m0 = 1.0f;
            b.m1 = static_cast<float> (k * (a * a - 1.0));
            b.m2 = 0.0f;
        }
        const double a1 = 1.0 / (1.0 + g * (g + k));
        b.a1 = static_cast<float> (a1);
        b.a2 = static_cast<float> (g * a1);
        b.a3 = static_cast<float> (g * g * a1);
    }
}

float FollowEq::process (float x)
{
    if (--countdown < 0)
    {
        updateCoefficients();
        countdown = kUpdateSamples - 1;
    }
    for (int i = 0; i < bands; ++i)
    {
        auto& b = band[static_cast<size_t> (i)];
        if (! b.on)
            continue;
        const float v3 = x - b.ic2;
        const float v1 = b.a1 * b.ic1 + b.a2 * v3;
        const float v2 = b.ic2 + b.a2 * b.ic1 + b.a3 * v3;
        b.ic1 = 2.0f * v1 - b.ic1;
        b.ic2 = 2.0f * v2 - b.ic2;
        x = b.m0 * x + b.m1 * v1 + b.m2 * v2;
    }
    return x;
}

} // namespace vcab
