// 声の高さの検出（pYIN。先読みは数フレーム）。
// 低い標本化周波数に落とした音の、決まった長さの窓を 1 フレームずつ受け取る。
// フレームごとに YIN の谷から高さの候補とその確率を求め、HMM（有声 × 高さの区画、無声 × 高さの区画）をビタビでたどる。
// 先読みの分（lookahead フレーム）だけさかのぼって、そのフレームの高さを決める。
// 本来の pYIN は全体を見てから決めるので、さかのぼれない分を補うため、1 オクターブ跳ぶつなぎを足している。
#pragma once

#include <cstdint>
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
    // fs: 受け取る音の標本化周波数。lowestMin / highestMax: 探す範囲のいちばん広い値（その分のメモリを確保する）。
    // lookahead: 先読みのフレームの数（何フレームさかのぼって決めるか）。octaveJump: 1 フレームで 1 オクターブ跳ぶ確率
    void prepare (float fs, float lowestMin, float highestMax, int lookahead, float octaveJump);

    // 探す範囲を変える（それまでの記憶も消える）
    void configure (float fmin, float fmax);

    // 1 フレームの窓の長さ（標本数）。窓の最後が、今のいちばん新しい標本
    int windowLength() const { return 2 * tauMax + 2; }
    int lookahead() const { return lag; }

    // x: windowLength() 個の標本。周期性以外の手がかりから求めた、有声の観測に掛ける倍率（log10）:
    // cueUp は有声に寄せる分（0 以上。周期性がある程度あるフレームでだけ効かせる）、cueDown は無声に寄せる分（0 以下。いつでも効かせる）。
    // lookahead() フレーム前のフレームの結果が決まったら decided に書いて true を返す（最初の lookahead() フレームは false）
    bool analyze (const float* x, float cueUp, float cueDown, PitchResult& decided);

private:
    static constexpr int kThresholds = 100;
    static constexpr int kBinsPerSemitone = 10;
    static constexpr int kMaxCandidates = 32;

    struct Candidate
    {
        float f0 = 0.0f;
        int bin = 0;
    };

    float fs = 16000.0f;
    int lag = 2;
    float octaveJump = 0.01f;
    int tauMin = 0, tauMax = 0;
    float binBase = 80.0f;   // 0 番の区画の高さ（Hz）
    int numBins = 0, maxBins = 0;
    std::vector<float> diff, cmnd, tauProb, thresholdWeight, nearWeight, octaveWeight;
    std::vector<float> delta, deltaNew, obs;
    // ビタビの途中の値（区画ごと）: 有声から近い区画へ（A）、無声から近い区画へ（B）、有声からオクターブ跳び（O）の、一番大きい値と元の区画
    std::vector<float> bestA, bestB, bestO;
    std::vector<int> argA, argB, argO, live;
    std::vector<int16_t> back;           // (lag + 1) フレーム × 2 × maxBins。前のフレームのどの状態から来たか
    std::vector<Candidate> cand;         // (lag + 1) フレーム × kMaxCandidates
    std::vector<int> candCount;
    int slot = 0;                        // いまのフレームを書く位置
    int64_t framesSeen = 0;

    float refineTau (int tau) const;
    int binOf (float f0) const;
    void observe (const float* x, float cueUp, float cueDown, Candidate* c, int& count);
};

} // namespace vcab
