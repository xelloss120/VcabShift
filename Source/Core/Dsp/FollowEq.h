// 声に追従する EQ。中心の周波数が時間とともに動くベル形の EQ を、いくつか直列につなぐ。
// 有声の流れには、出力の声の高さ（基音）と、動かした後の F1〜F4 の 5 つ。無声の流れには、動かした後の F1〜F4 の 4 つ。
// 1 つ 1 つは Simper の SVF（状態変数フィルタ）のベル形。係数を動かしても乱れにくい。量が 0dB ならちょうど素通し。
#pragma once

#include "../Params.h"

#include <array>

namespace vcab
{

// 山の幅（オクターブ）。基音は 2 倍音にかからないよう狭く、F1〜F4 は山全体を持ち上げ下げするよう広く
constexpr float kEqBandwidthF0 = 2.0f / 3.0f;
constexpr float kEqBandwidthFormant = 1.0f;

// EQ の F1〜F4 の目印（動かす前の位置）。帯域固定は決まった位置、ほかは追いかけた位置（一律でも追いかけは動いている）
std::array<float, 4> eqAnchors (const Settings& s, const std::array<float, 4>& tracked);
// 動かした後の F1〜F4 の位置（Hz）。stream: 0 = 有声（フォルマント＋差）、1 = 無声（無声フォルマント＋無声の差）
std::array<float, 4> eqFormantCentres (const Settings& s, const std::array<float, 4>& anchors, int stream);
// 実際に使う中心（Hz）。20Hz〜標本化周波数の 0.45 倍に収める（音の処理と画面で同じ値にする）
float eqClampHz (float hz, double fs);
// ベル形 1 つの、周波数 f での大きさ（dB。画面の線用）
float eqBellDb (float f, float centre, float gainDb, float bandwidthOct, double fs);

class FollowEq
{
public:
    static constexpr int kMaxBands = 5;

    void prepare (double sampleRate, int numBands, const std::array<float, kMaxBands>& bandwidthOct);
    void reset();
    // 狙いの中心（Hz）と量（dB）。中心が 0 以下なら、中心は前の狙いのまま
    void setTarget (int band, float hz, float db);
    float process (float x);

private:
    struct Band
    {
        float q = 1.0f;
        float targetLog = 0.0f, log = 0.0f;   // 中心（log2 Hz）
        float targetDb = 0.0f, db = 0.0f;
        bool started = false;                 // 中心の狙いを一度でも受け取ったか（最初はならさずにそこへ置く）
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, m1 = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f;
        bool on = false;                      // 量が 0 でない（0 のときは通さず、状態も 0 にしておく）
    };
    double fs = 48000.0;
    int bands = 0;
    std::array<Band, kMaxBands> band {};
    int countdown = 0;
    float freqAlpha = 1.0f, gainAlpha = 1.0f;

    void updateCoefficients();
};

} // namespace vcab
