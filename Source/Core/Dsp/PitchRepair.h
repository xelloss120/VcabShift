// ピッチ検出で決まった高さの手直し（フレームごと。Engine から呼ぶ）。順番は 1 → 2:
//   1. 「んー」の対策（基音が弱く 2 倍音が強い声は、半分の周期でも波形がよく重なり、2 倍の高さを取る）:
//      ふだんの高さの約 1 オクターブ上を取っていて、倍の周期のほうがはっきりよく重なり、上の奇数倍が弱いなら、半分の高さにする。
//   2. ガラガラ声の手当て: 高さが取れなくなった（または急に高く跳んだ）とき、直前の周期で窓の波形がよく重なれば、その高さを保つ。
//      直前の有声がなければ（発話の始まり）、ふだんの高さの近くでよく重なる周期から始める。
// 「ふだんの高さ」は、有声のフレームの高さ（log2）の平均（重みは 1000 フレームでならす）。
// 窓は、そのフレームでピッチ検出に渡したもの（低い標本化周波数）。
#pragma once

#include "PitchDetector.h"

#include <array>
#include <vector>

namespace vcab
{

class PitchRepair
{
public:
    // 開発用の調整（初期値が製品の値）
    struct Tuning
    {
        // 2: 0 で使わない、1 で直前の高さを保つだけ、2 でふだんの高さから始めることもする
        int creakHold = 2;
        int holdGap = 8;            // 有声（保ったものを含む）からこのフレーム数以内なら、保ち直せる
        float holdRange = 0.15f;    // 重なりを見るずらす量の幅（±。0.06 より大きければ、一番よく重なった周期について行く）
        float holdStep = 0.15f;     // 前のフレームから続けて保つときの、重なりを見る幅（±。少しずつずれて隣の倍数の周期に乗り移らないように）
        float startRange = 0.3f;    // ふだんの高さから始めるときの、重なりを見る幅（±）
        int startFrames = 2;        // ふだんの高さから始めるまでに続くフレームの数
        float startJump = 15.0f;    // 発話の始まりで、ふだんの高さよりこれ（半音）より上の検出を、取れなかったとみなす（0 で使わない）
        float holdCorr = 0.85f;     // 直前の周期（T か 2T）で窓の波形がこれ以上重なるときだけ保つ
        // 1: ふだんの高さの humLow〜humHigh オクターブ上を取ったとき、倍の周期のほうが窓の波形がこれだけ（相関の差）よく重なれば、
        // 半分の高さにする。直している間は humStay で続ける。humEnter が 0 以下で使わない
        float humEnter = 0.05f;
        float humStay = 0.0f;
        float humLow = 0.6f, humHigh = 1.4f;
        float humCorr = 0.95f;      // 倍の周期の重なりがこれ以上のときだけ（「んー」はほぼ 1。高い声やかすれた声は 0.7〜0.85）
        float humSpread = -4.0f;    // 直し始めるのは、subharmonicSpread がこれ（dB）以下のときだけ（「んー」は -9〜-12、ガラガラ声は -3 より上）
    };

    // lowRate: 窓の標本化周波数。maxWindow: 窓の長さの上限
    void prepare (double lowRate, int maxWindow);
    void setTuning (const Tuning& t) { tuning = t; }
    bool enabled() const { return tuning.creakHold > 0 || tuning.humEnter > 0.0f; }
    void reset();

    // 分析したフレームごと（高さが決まる前。先読みの分だけ先に呼ばれる）: 背景の音の大きさを、一番小さい音を 1 秒に 3dB ずつ
    // 上げながら追う。hopSeconds: フレームの間隔
    void observeLevel (float levelDb, float hopSeconds);

    // 決まったフレームの高さを手直しする。window: そのフレームの窓（length 個。もう残っていなければ nullptr）。
    // levelDb: そのフレームの音量、levelPeak: 最近の一番大きい音（どちらも dB）、tilt: 帯域の比の手がかり（-1〜1。正が声らしい）、
    // pitchMin: 探索下限（Hz）、timeSec: 窓の中心の時刻（開発用の記録だけに使う）
    PitchResult adjust (const PitchResult& p, const float* window, int length, float levelDb, float levelPeak, float tilt,
                        float pitchMin, double timeSec);

    // 開発用: 判定ごとに { 時刻（秒）, 理由, 高さ, 有声からのフレーム数 } を足す
    // （理由: 9 = 取れている、1 = 直前の高さを保った、2 = ふだんの高さから始めた、3 = 声らしくない、
    //   4 = 続いているが重ならない、5 = ふだんの高さをまだ知らない、6 = ふだんの高さで重ならない、7 = 続くのを待っている、8 = 続いていない）
    void setLog (std::vector<std::array<float, 4>>* log) { holdLog = log; }

private:
    Tuning tuning;
    double fsd = 16000.0;
    std::vector<float> humWin;
    std::vector<std::array<float, 4>>* holdLog = nullptr;

    float floorDb = -200.0f;
    float lastVoicedF0 = 0.0f;
    int sinceVoiced = 1 << 20, holdFrames = 0, creakRun = 0;
    double typicalLog = 0.0, typicalWeight = 0.0;
    bool humFixing = false; // 前のフレームで「んー」の対策をかけた

    // 窓で、ずらす量 lag（窓の標本）× (1 ± range) の正規化相互相関の最大。bestLag にそのずらす量。窓がなければ 0
    static float windowCorr (const float* w, int wp, float lag, float range, float* bestLag = nullptr);
    // 窓を、半分の高さ f0 / 2 の 1〜8 倍の正弦波で最小二乗に当てはめ、
    // 「3・5・7 倍（上の奇数）の強さ」−「1 倍（f0 / 2）の強さ」（どちらも f0 の倍音に対する dB）を返す。
    // 「んー」は半分の高さの成分が一番下にだけあるので負、ガラガラ声は上の倍音の間にもあるので 0 前後より上。窓がなければ 0
    float subharmonicSpread (const float* w, int wp, float f0);
};

} // namespace vcab
