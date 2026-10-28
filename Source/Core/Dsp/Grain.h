// PSOLA の断片を 1 つ作る: 窓をかけ、一律の倍率で伸縮し、一律でない動かし方なら包絡の差を補正する。
#pragma once

#include "Fft.h"
#include "WarpMap.h"

#include <cstdint>
#include <vector>

namespace vcab
{

class GrainMaker
{
public:
    // maxHalfIn: 元の断片の半分の長さの上限、maxHalfOut: 伸縮後の半分の長さの上限、maxLifter: 補正の包絡の残す長さの上限（どれも標本数）
    void prepare (double sampleRate, int maxHalfIn, int maxHalfOut, int maxLifter);

    // 元の断片を読むのに、中心の前後に要る標本の数（窓の半分＋補間の幅）
    static int readMargin (float halfIn, float ratio);

    // seg: 元の音（seg[centre] が断片の中心）。halfIn: 元の断片の半分の長さ（halfOut × ratio）。
    // ratio: 一律の倍率（> 1 で縮めてフォルマントを上げる）。warp: 一律でなければ補正に使う写像（一律なら nullptr）。
    // lifter: 包絡の残す長さ（標本数）。out: 2 × halfOut + 1 個。
    // key: 元の断片の時刻。前と同じ時刻・同じ長さなら、包絡を求め直さない（ピッチを上げると同じ元の断片を何度も使うため）
    void make (const float* seg, int centre, float halfIn, int halfOut, float ratio,
               const WarpMap* warp, int lifter, float* out, int64_t key);

    // 使い回している包絡を捨てる（音の時刻を数え直すとき）
    void forgetEnvelope() { envValid = false; }

private:
    static constexpr int kZeroCrossings = 8;
    static constexpr int kTableRes = 256;

    double fs = 48000.0;
    Fft fft;
    std::vector<float> kernel;            // 窓をかけた sinc（0〜kZeroCrossings、kTableRes 刻み）
    std::vector<float> windowed;
    std::vector<Fft::Complex> spec, spec2;
    std::vector<float> envelope;          // 包絡（自然対数）
    bool envValid = false;
    int64_t envKey = 0;
    int envN = 0, envLifter = 0, envM = 0;
    // FFT の区画ごとの「元の周波数」（写像の逆）。写像と FFT の大きさが同じなら使い回す
    std::vector<float> inverseBins;
    const WarpMap* inverseWarp = nullptr;
    unsigned inverseVersion = 0;
    int inverseSize = 0;

    float kernelAt (float x) const;
    void computeEnvelope (const float* w, int n, int lifter, int& m);
};

} // namespace vcab
