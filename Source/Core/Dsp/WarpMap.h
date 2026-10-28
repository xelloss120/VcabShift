// 動かし方の写像。元の周波数 f の山を f × 2^(d(f)/12) へ動かす。
// 補間は周波数の対数の上で余弦の形。動かした後の周波数が逆転しないよう、単調に保つ。
#pragma once

#include "../Params.h"

#include <array>

namespace vcab
{

class WarpMap
{
public:
    // anchors: F1〜F4 の目印（Hz。帯域固定なら決まった位置、そうでなければ追いかけた位置）
    void build (float formant, const std::array<float, 4>& delta,
                const std::array<float, 4>& anchors, AboveF4 above);

    // 一律に伸縮する倍率（F2 の目印での動く量）
    float uniformRatio() const { return uniform; }
    // 一律だけで済むか（補正が要らない。差がすべて 0 のときなど）
    bool isUniform() const { return flat; }

    // 元の周波数 f が動く先
    float forward (float f) const;
    // 動いた先の周波数 g の、元の周波数
    float inverse (float g) const;
    // 元の周波数 f での動く量（半音。単調に保った後の値）
    float shiftAt (float f) const;

    // build のたびに増える番号（写像から求めた表を使い回すときの目印）
    unsigned version() const { return buildCount; }

    // 目印の位置（目印どうしを離した後の値）
    const std::array<float, 4>& anchorPositions() const { return anchorPos; }

    // 目印どうしを kMinAnchorRatio 倍以上離し、範囲に収める
    static std::array<float, 4> separateAnchors (std::array<float, 4> a);

private:
    static constexpr int kGrid = 720;            // 10Hz〜（1/48 オクターブ刻みで 15 オクターブ）
    static constexpr float kGridLo = 3.321928f;  // log2(10)
    static constexpr float kGridStep = 1.0f / 48.0f;

    struct Point { float x, d; };                 // x = log2(f)、d = 半音
    std::array<Point, 6> points {};
    int numPoints = 0;
    float uniform = 1.0f;
    bool flat = true;
    unsigned buildCount = 0;
    std::array<float, 4> anchorPos {};

    std::array<float, kGrid> outX {};            // 格子の各点 x の動く先（log2）

    float curve (float x) const;
};

} // namespace vcab
