// 声の高さの検出（YIN）。
// 低い標本化周波数に落とした音の、決まった長さの窓を 1 フレームずつ受け取り、そのフレームの値をすぐ返す（先の音は見ない）。
#pragma once

#include <vector>

namespace vcab
{

struct PitchResult
{
    float f0 = 0.0f;      // Hz（無声なら 0）
    bool voiced = false;
};

class PitchDetector
{
public:
    // fs: 受け取る音の標本化周波数。lowestMin: 探す範囲の下限の一番低い値（その分のメモリを確保する）
    void prepare (float fs, float lowestMin);

    // 探す範囲を変える
    void configure (float fmin, float fmax);

    // 1 フレームの窓の長さ（標本数）。窓の最後が、今のいちばん新しい標本
    int windowLength() const { return 2 * tauMax + 2; }

    // x: windowLength() 個の標本
    PitchResult analyze (const float* x);

private:
    float fs = 16000.0f;
    int tauMin = 0, tauMax = 0;
    std::vector<float> diff, cmnd;

    float refineTau (int tau) const;
};

} // namespace vcab
