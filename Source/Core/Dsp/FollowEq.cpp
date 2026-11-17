#include "FollowEq.h"
#include "WarpMap.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kUpdateSamples = 16;          // 係数を求め直す間隔
constexpr double kFreqSeconds = 0.005;      // 中心のならし（フレームの間隔 5ms で段になって動くのを、なめらかにする）
constexpr double kGainSeconds = 0.020;      // 量のならし（つまみを動かしたとき）
constexpr float kMinHz = 20.0f;
constexpr float kMaxFraction = 0.45f;       // 中心の上限（標本化周波数に対して）

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

float eqBellDb (float f, float centre, float gainDb, float bandwidthOct, double fs)
{
    if (gainDb == 0.0f)
        return 0.0f;
    // SVF のベル形は、アナログの (s² + (A/Q)s + 1) / (s² + s/(AQ) + 1) を、中心で合わせて双一次変換したもの
    const double fc = eqClampHz (centre, fs);
    const double fe = std::min (static_cast<double> (f), 0.4999 * fs);
    const double w = std::tan (kPi * fe / fs) / std::tan (kPi * fc / fs);
    const double a = std::pow (10.0, gainDb / 40.0);
    const double q = qForBandwidth (bandwidthOct);
    const double re = 1.0 - w * w;
    const double num = re * re + (a / q * w) * (a / q * w);
    const double den = re * re + (w / (a * q)) * (w / (a * q));
    return static_cast<float> (10.0 * std::log10 (num / den));
}

void FollowEq::prepare (double sampleRate, int numBands, const std::array<float, kMaxBands>& bandwidthOct)
{
    fs = sampleRate;
    bands = std::clamp (numBands, 0, kMaxBands);
    for (int i = 0; i < kMaxBands; ++i)
        band[static_cast<size_t> (i)].q = qForBandwidth (bandwidthOct[static_cast<size_t> (i)]);
    freqAlpha = static_cast<float> (1.0 - std::exp (-kUpdateSamples / (kFreqSeconds * fs)));
    gainAlpha = static_cast<float> (1.0 - std::exp (-kUpdateSamples / (kGainSeconds * fs)));
    reset();
}

void FollowEq::reset()
{
    for (auto& b : band)
    {
        const float q = b.q;
        b = Band {};
        b.q = q;
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
        const double g = std::tan (kPi * std::exp2 (static_cast<double> (b.log)) / fs);
        const double a = std::pow (10.0, b.db / 40.0);
        const double k = 1.0 / (b.q * a);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        b.a1 = static_cast<float> (a1);
        b.a2 = static_cast<float> (g * a1);
        b.a3 = static_cast<float> (g * g * a1);
        b.m1 = static_cast<float> (k * (a * a - 1.0));
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
        x += b.m1 * v1;
    }
    return x;
}

} // namespace vcab
