// 声に追従する EQ。中心の周波数が時間とともに動くベル形の EQ を、いくつか直列につなぐ。
// 有声の流れには、出力の声の高さ（基音）と、動かした後の F1〜F4 の 5 つ。無声の流れには、動かした後の F1〜F4 の 4 つ。
// 1 つ 1 つは Simper の SVF（状態変数フィルタ）のベル形。係数を動かしても乱れにくい。量が 0dB ならちょうど素通し。
// 両端の点（有声の基音と F4、無声の F1 と F4）は、下げるときだけ棚形（シェルフ）にして、外側をまとめて下げる。
#pragma once

#include "../Params.h"

#include <array>

namespace vcab
{

// 山の幅（オクターブ）。基音は 2 倍音にかからないよう狭く、F1〜F4 は山全体を持ち上げ下げするよう広く
constexpr float kEqBandwidthF0 = 2.0f / 3.0f;
constexpr float kEqBandwidthFormant = 1.0f;

// 下げるときだけ棚形にする側。low: その点より下をまとめて下げる、high: 上をまとめて下げる
enum class EqShelf { none, low, high };
constexpr std::array<EqShelf, 5> kEqShelfVoiced { EqShelf::low, EqShelf::none, EqShelf::none, EqShelf::none, EqShelf::high };   // 基音、F1〜F4
constexpr std::array<EqShelf, 5> kEqShelfUnvoiced { EqShelf::low, EqShelf::none, EqShelf::none, EqShelf::high, EqShelf::none }; // F1〜F4

// EQ の F1〜F4 の目印（動かす前の位置）。帯域固定は決まった位置、ほかは追いかけた位置（一律でも追いかけは動いている）
std::array<float, 4> eqAnchors (const Settings& s, const std::array<float, 4>& tracked);
// 動かした後の F1〜F4 の位置（Hz）。stream: 0 = 有声（フォルマント＋差）、1 = 無声（無声フォルマント＋無声の差）
std::array<float, 4> eqFormantCentres (const Settings& s, const std::array<float, 4>& anchors, int stream);
// 実際に使う中心（Hz）。20Hz〜標本化周波数の 0.45 倍に収める（音の処理と画面で同じ値にする）
float eqClampHz (float hz, double fs);
// 点 1 つの、周波数 f での大きさ（dB。画面の線用）。shelf が none 以外で量が負なら棚形
float eqBandDb (float f, float centre, float gainDb, float bandwidthOct, EqShelf shelf, double fs);

class FollowEq
{
public:
    static constexpr int kMaxBands = 5;

    void prepare (double sampleRate, int numBands, const std::array<float, kMaxBands>& bandwidthOct,
                  const std::array<EqShelf, kMaxBands>& shelf);
    void reset();
    // 狙いの中心（Hz）と量（dB）。中心が 0 以下なら、中心は前の狙いのまま
    void setTarget (int band, float hz, float db);
    float process (float x);

private:
    struct Band
    {
        float q = 1.0f;
        float shelfOffset = 0.0f;             // 棚形の折れ目の位置（中心からの log2。内側へ幅の半分）
        EqShelf shelf = EqShelf::none;
        float targetLog = 0.0f, log = 0.0f;   // 中心（log2 Hz）
        float targetDb = 0.0f, db = 0.0f;
        bool started = false;                 // 中心の狙いを一度でも受け取ったか（最初はならさずにそこへ置く）
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;
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
