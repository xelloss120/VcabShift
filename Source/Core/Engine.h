// 全体の処理: 入力チャンネルの選択 → 低い標本化周波数に落とす → 5ms ごとにピッチ検出と F1〜F4 の追いかけ
// → 印付け → TD-PSOLA（有声と無声の 2 つの流れを、10ms で等パワーにつなぐ）。
// 遅延は「探索下限の周期 × 2 ＋ ピッチ検出の先読み（2 フレーム、10ms）」。
#pragma once

#include "Params.h"
#include "Dsp/Fft.h"
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

    // 有声・無声の履歴（フレームごと。番号 % kVoicingHistory の位置に書く）。voicingCount はこれまでのフレームの数
    static constexpr int kVoicingHistory = 1024;
    std::array<std::atomic<bool>, kVoicingHistory> voicing {};
    std::atomic<int64_t> voicingCount { 0 };
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

    // 開発用の調整（prepare の前に呼ぶ。パラメータではない）
    struct Tuning
    {
        int pitchLookahead = 2;     // ピッチ検出の先読みのフレームの数（1 フレーム 5ms）
        float octaveJump = 0.001f;
        bool voicingCues = true;    // 有声・無声の判定に、周期性以外の手がかり（帯域の比、音量）を使う  // ピッチ検出の HMM の、1 フレームで 1 オクターブ跳ぶ確率（0.01 では速い抑揚の途中で 1 フレームだけ倍の高さへ跳んだ）
    };
    void setTuning (const Tuning& t) { tuning = t; }

    // 開発用: 決まったフレームごとに { 窓の中心の時刻（秒）, 有声なら 1, 高さ（Hz） } を足す（音の処理のスレッドでは使わない）
    void setFrameLog (std::vector<std::array<float, 3>>* log) { frameLog = log; }
    // 開発用: 置いた印ごとに { 時刻（標本）, 周期（標本）, 有声なら 1, 相互相関の値（有声の始まりと無声は -2） } を足す
    void setMarkLog (std::vector<std::array<double, 4>>* log) { markLog = log; }
    // 開発用: 有声の断片ごとに { 出力の印の時刻, 使った元の印の時刻, 元の印の周期, 0 } を足す
    void setGrainLog (std::vector<std::array<double, 4>>* log) { grainLog = log; }

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
        int64_t ready = 0;    // この印が決まる入力の時刻（これより前の時点では使わない。ブロックの大きさで結果が変わらないように）
    };

    double fs = 48000.0;
    Settings settings;
    Tuning tuning;
    std::vector<std::array<float, 3>>* frameLog = nullptr;
    std::vector<std::array<double, 4>>* markLog = nullptr;
    std::vector<std::array<double, 4>>* grainLog = nullptr;
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
    int64_t frameCount = 0;     // 決まったフレームの数（ピッチ検出の先読みの分だけ、分析したフレームより少ない）
    int64_t rawCount = 0;       // 分析した（窓をピッチ検出に渡した）フレームの数
    int64_t nextFrameAt = 0;    // 次のフレームを分析する入力の時刻
    int lag = 0;                // ピッチ検出の先読みのフレームの数
    static constexpr int kPendingRing = 64;
    std::array<float, kPendingRing> pendingLevel {}; // 分析して、まだ決まっていないフレームの音量（dB）
    float levelPeak = -120.0f;
    float rawPeak = -120.0f;    // 分析した時点での、最近の一番大きい音（dB。有声・無声の手がかり用）
    Fft cueFft;
    std::vector<Fft::Complex> cueBuf;
    std::vector<float> cueWindow;
    int cueSize = 0;            // FFT の大きさ（ピッチ検出の窓の長さ以上の 2 のべき乗）

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
    // 写像（0 = 有声、1 = 無声。無声はフォルマントのつまみの代わりに無声フォルマントを使う）。
    // 最後に作ったときの入力を持ち、同じなら作り直さない
    struct WarpCache
    {
        WarpMap map;
        bool valid = false;
        float formant = 0.0f;
        std::array<float, 4> delta {}, anchors {};
        AboveF4 above = AboveF4::followF4;
    };
    std::array<WarpCache, 2> warps;
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
    void finalizeFrame (const PitchResult& pitch);
    void voicingCue (int windowLength, float levelDb, float& up, float& down);
    void placeMarks();
    void synthesize (int64_t blockStart, int64_t blockEnd);
    void writeGrain (std::vector<float>& acc, double centreOut, int halfOut, int64_t from);
    bool makeGrain (int64_t centreIn, float halfIn, int halfOut, float ratio, const WarpMap* correction, int lifter, int stream, bool powerWindow);
    // t に一番近いフレーム。availInPos を渡すと、入力がその時刻まで届いた時点で分析済みのフレームに限る
    const Frame& frameAt (double t, int64_t availInPos = INT64_MAX) const;
    double frameCentre (int64_t j) const;      // j 番目のフレームの窓の中心の時刻
    int64_t nearestFrame (double t) const;     // 時刻 t に一番近いフレームの番号（frameAt が使うもの）
    int64_t inPosWhenKnown (double t) const;   // 時刻 t のフレームが決まる入力の時刻
    bool voicedThroughout (int64_t from, int64_t to, int64_t availInPos) const;
    float ratioFor (const Frame& f, int stream); // stream: 0 = 有声、1 = 無声
    const WarpMap* correctionFor (int stream) const { return warps[static_cast<size_t> (stream)].map.isUniform() ? nullptr : &warps[static_cast<size_t> (stream)].map; }

    float x (int64_t t) const { return t < 0 ? 0.0f : xRing[static_cast<size_t> (t & xMask)]; }
};

} // namespace vcab
