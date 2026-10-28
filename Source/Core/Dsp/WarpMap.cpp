#include "WarpMap.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr float kPi = 3.14159265f;
constexpr float kMinTailOctaves = 1.0f / 3.0f;
constexpr float kTailMargin = 1.25f;
constexpr float kMinOutStep = 1.0e-4f; // 単調に保つときの、格子 1 つ分の最小の増え（log2）
} // namespace

std::array<float, 4> WarpMap::separateAnchors (std::array<float, 4> a)
{
    for (int i = 0; i < 4; ++i)
    {
        const auto& r = kFormantRanges[static_cast<size_t> (i)];
        float v = std::clamp (a[static_cast<size_t> (i)], r.lo, r.hi);
        if (i > 0)
            v = std::max (v, a[static_cast<size_t> (i - 1)] * kMinAnchorRatio);
        a[static_cast<size_t> (i)] = v;
    }
    return a;
}

void WarpMap::build (float formant, const std::array<float, 4>& delta,
                     const std::array<float, 4>& anchors, AboveF4 above)
{
    ++buildCount;
    numPoints = 0;
    anchorPos = separateAnchors (anchors);
    for (int i = 0; i < 4; ++i)
        points[numPoints++] = { std::log2 (anchorPos[static_cast<size_t> (i)]), formant + delta[static_cast<size_t> (i)] };
    const float d4 = formant + delta[3];
    const float dAbove = above == AboveF4::followF4 ? d4 : above == AboveF4::followUniform ? formant : 0.0f;
    // 差が大きいほど広くつなぐ（余弦の一番急な所の傾き π/2 × 差 が 1 オクターブで 12 半音を超えないように）
    const float width = std::max (kMinTailOctaves, std::abs (dAbove - d4) / 12.0f * (kPi * 0.5f) * kTailMargin);
    points[numPoints++] = { points[3].x + width, dAbove };
    uniform = std::exp2 ((formant + delta[1]) / 12.0f);

    flat = true;
    for (int i = 1; i < numPoints; ++i)
        if (std::abs (points[static_cast<size_t> (i)].d - points[0].d) > 1.0e-4f)
            flat = false;

    float prev = -1.0e9f;
    for (int i = 0; i < kGrid; ++i)
    {
        const float x = kGridLo + kGridStep * static_cast<float> (i);
        float y = x + curve (x) / 12.0f;
        y = std::max (y, prev + kMinOutStep);
        outX[static_cast<size_t> (i)] = y;
        prev = y;
    }
}

float WarpMap::curve (float x) const
{
    if (numPoints == 1 || x <= points[0].x)
        return points[0].d;
    for (int i = 1; i < numPoints; ++i)
    {
        const Point& a = points[static_cast<size_t> (i - 1)];
        const Point& b = points[static_cast<size_t> (i)];
        if (x <= b.x)
        {
            const float t = (x - a.x) / std::max (b.x - a.x, 1.0e-6f);
            return a.d + (b.d - a.d) * 0.5f * (1.0f - std::cos (kPi * t));
        }
    }
    return points[static_cast<size_t> (numPoints - 1)].d;
}

float WarpMap::forward (float f) const
{
    const float x = std::log2 (std::max (f, 1.0f));
    const float pos = (x - kGridLo) / kGridStep;
    if (pos <= 0.0f)
        return std::exp2 (outX[0] + (x - kGridLo));
    if (pos >= kGrid - 1)
        return std::exp2 (outX[kGrid - 1] + (x - (kGridLo + kGridStep * (kGrid - 1))));
    const int i = static_cast<int> (pos);
    const float t = pos - static_cast<float> (i);
    return std::exp2 (outX[static_cast<size_t> (i)] + t * (outX[static_cast<size_t> (i + 1)] - outX[static_cast<size_t> (i)]));
}

float WarpMap::inverse (float g) const
{
    const float y = std::log2 (std::max (g, 1.0f));
    if (y <= outX[0])
        return std::exp2 (kGridLo + (y - outX[0]));
    if (y >= outX[kGrid - 1])
        return std::exp2 (kGridLo + kGridStep * (kGrid - 1) + (y - outX[kGrid - 1]));
    const auto it = std::upper_bound (outX.begin(), outX.end(), y);
    const int i = static_cast<int> (it - outX.begin()) - 1;
    const float y0 = outX[static_cast<size_t> (i)], y1 = outX[static_cast<size_t> (i + 1)];
    const float t = (y - y0) / std::max (y1 - y0, 1.0e-9f);
    return std::exp2 (kGridLo + kGridStep * (static_cast<float> (i) + t));
}

float WarpMap::shiftAt (float f) const
{
    return 12.0f * std::log2 (forward (f) / std::max (f, 1.0f));
}

} // namespace vcab
