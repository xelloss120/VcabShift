// F1〜F4 の追いかけ。約 16kHz に落とした音の 25ms の窓から LPC を求め、根から山を選ぶ。
// 追いかけられないときは 150ms 保ち、その後 50ms で目印へ戻る。
#pragma once

#include <array>
#include <complex>
#include <vector>

namespace vcab
{

struct FormantFrame
{
    std::array<float, 4> f { 500.0f, 1400.0f, 2700.0f, 3500.0f }; // Hz
    std::array<bool, 4> tracked { false, false, false, false };   // このフレームで追いかけられたか（画面の塗りつぶし）
};

class FormantTracker
{
public:
    // fs: 受け取る音の標本化周波数、hopSeconds: フレームの間隔
    void prepare (float fs, float hopSeconds);
    void reset (const std::array<float, 4>& anchors);

    int windowLength() const { return window; }

    // x: windowLength() 個の標本。usable: 声の区間か（有声で、小さすぎない）。
    // anchors: 追いかけられないときに戻る目印
    void analyze (const float* x, bool usable, const std::array<float, 4>& anchors, FormantFrame& out);

private:
    using C = std::complex<double>;
    struct Peak { double f, bw; };

    float fs = 16000.0f;
    float hop = 0.005f;
    int window = 400;
    int order = 18;

    std::vector<double> hamming, buf, autoc, lpc, tmp;
    std::vector<C> roots;
    bool rootsValid = false;

    std::array<double, 4> value {};      // 今の値（Hz）
    std::array<int, 4> missing {};       // 追いかけられなかったフレームの数
    std::array<double, 4> glideFrom {};  // 目印へ戻り始めたときの値
    std::array<bool, 4> prevTracked {};

    bool computeLpc (const float* x);
    void findRoots();
};

} // namespace vcab
