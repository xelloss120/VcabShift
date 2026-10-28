// 全体の処理: 入力チャンネルの選択 → 低い標本化周波数に落とす → 5ms ごとにピッチ検出と F1〜F4 の追いかけ
// → 印付け → TD-PSOLA（有声と無声の 2 つの流れを、10ms で等パワーにつなぐ）。
// 遅延は「探索下限の周期 × 2」。
#pragma once

#include "Params.h"
#include "Dsp/FormantTracker.h"
#include "Dsp/Grain.h"
#include "Dsp/PitchDetector.h"
#include "Dsp/WarpMap.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace vcab
{

// 画面に出す今の状態（音の処理のスレッドが書き、画面のスレッドが読む）
struct EngineStatus
{
    std::atomic<float> f0 { 0.0f };
    std::array<std::atomic<float>, 4> formant {};
    std::array<std::atomic<bool>, 4> tracked {};
    std::atomic<int> channel { 0 };
};

class Engine
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void setSettings (const Settings& s);

    // in: numIn チャンネル、out: 処理した音（1 チャンネル）。
    // delayedInput が nullptr でなければ、選んだ入力を遅延の分だけ遅らせたものを書く（画面のスペクトル用）
    void process (const float* const* in, int numIn, float* out, int n, float* delayedInput = nullptr);

    int latencySamples() const { return latency; }
    double sampleRate() const { return fs; }
    const EngineStatus& status() const { return stat; }

    // 一律の倍率の範囲（これを超える分は補正で動かす）
    static constexpr float kMinRatio = 0.25f, kMaxRatio = 4.0f;

private:
    struct Frame
    {
        double centre = 0.0;  // 窓の中心の時刻（入力の標本の番号）
        bool voiced = false;
        float period = 0.0f;  // 標本数（元の標本化周波数）
        FormantFrame fm;
    };
    struct Mark
    {
        int64_t t = 0;
        float period = 0.0f;
        bool voiced = false;
    };

    double fs = 48000.0;
    Settings settings;
    bool configured = false;

    // 長さ（標本数）
    int latency = 0, tMax = 0, hop = 0, unvoicedStep = 0, lifterUnvoiced = 0, lifterMax = 0, fadeSamples = 1;

    // 入力の輪
    std::vector<float> xRing;
    int64_t xMask = 0;
    int64_t inPos = 0;

    // 低い標本化周波数（フォルマントの追いかけとピッチ検出）
    int decim = 3;
    double fsd = 16000.0;
    std::vector<float> lowpass;
    int lpDelay = 0;
    std::vector<float> dRing;
    int64_t dMask = 0, dCount = 0;
    std::vector<float> scratch;

    PitchDetector detector;
    FormantTracker tracker;
    std::vector<Frame> frames;
    int64_t frameCount = 0;     // 分析したフレームの数
    int64_t nextFrameAt = 0;    // 次のフレームを分析する入力の時刻
    float levelPeak = -120.0f;

    std::vector<Mark> marks;
    int64_t markCount = 0;
    int64_t markCursor = 0;     // 有声の流れが今見ている印

    // 出力
    std::vector<float> accVoiced, accUnvoiced;
    int64_t accMask = 0;
    int64_t outPos = 0;
    double nextVoiced = 0.0, nextUnvoiced = 0.0;
    float theta = 0.0f;         // 有声の重み sin、無声の重み cos

    GrainMaker grain;
    WarpMap warp;
    // 最後に写像を作ったときの入力（同じなら作り直さない）
    bool warpValid = false;
    float warpFormant = 0.0f;
    std::array<float, 4> warpDelta {}, warpAnchors {};
    AboveF4 warpAbove = AboveF4::followF4;
    std::vector<float> seg, grainOut, corr, mono;

    // 入力チャンネルの自動の選択
    int channel = 0, channelTarget = 0;
    float chPower[2] { 0.0f, 0.0f };
    int otherLouderFor = 0;
    float chFade = 1.0f;        // 前のチャンネルから今のチャンネルへのつなぎ（1 で今だけ）
    int chPrev = 0;

    EngineStatus stat;

    void configure();
    void selectInput (const float* const* in, int numIn, int n, float* mono);
    void pushInput (float v);
    void analyzeFrame();
    void placeMarks();
    void synthesize (int64_t blockStart, int64_t blockEnd);
    void writeGrain (std::vector<float>& acc, double centreOut, int halfOut, int64_t from);
    bool makeGrain (int64_t centreIn, float halfIn, int halfOut, float ratio, bool correct, int lifter);
    const Frame& frameAt (double t) const;
    bool voicedThroughout (int64_t from, int64_t to) const;
    float ratioFor (const Frame& f);

    float x (int64_t t) const { return t < 0 ? 0.0f : xRing[static_cast<size_t> (t & xMask)]; }
};

} // namespace vcab
