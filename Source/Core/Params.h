// パラメータの唯一の定義。プラグインの設定、処理ツール、画面は、どれもこの表から作る。
#pragma once

#include <array>
#include <string>
#include <vector>

namespace vcab
{

enum class P : int
{
    pitch,
    formant,
    warpMode,
    f1Delta, f2Delta, f3Delta, f4Delta,
    f1Freq, f2Freq, f3Freq, f4Freq,
    aboveF4,
    pitchMin,
    pitchMax,
    inputChannel,
    count
};

constexpr int kNumParams = static_cast<int> (P::count);

enum class ParamType { real, integer, choice, toggle };

struct ParamInfo
{
    P id;
    const char* key;      // 保存とホストの識別に使う名前（変えない）
    const char* name;     // 画面に出す名前
    ParamType type;
    float minValue, maxValue, defaultValue;
    float step;           // 0 なら連続
    float skewCentre;     // 0 なら直線。そうでなければ、つまみの真ん中に来る値
    const char* unit;
    std::vector<const char*> choices;
    const char* help;     // ツールチップの説明
};

// 動かし方（一律 / F1〜F4 の点を声から追いかけた位置に置く / 決まった位置に置く）
enum class WarpMode : int { uniform, formants, bands };
// F4 より上の扱い
enum class AboveF4 : int { followF4, followUniform, original };
// 入力チャンネル
enum class InputChannel : int { automatic, ch1, ch2, average };

const std::vector<ParamInfo>& paramTable();
const ParamInfo& paramInfo (P id);

// F1〜F4 の目印と、追いかけの範囲（Hz）
struct FormantRange { float lo, hi, target; };
constexpr std::array<FormantRange, 4> kFormantRanges { { { 180.0f, 1100.0f, 500.0f },
                                                         { 550.0f, 3000.0f, 1500.0f },
                                                         { 1500.0f, 4000.0f, 2500.0f },
                                                         { 2500.0f, 5000.0f, 3500.0f } } };
constexpr float kMinAnchorRatio = 1.15f; // 目印どうしを離す倍率

// 処理に渡す設定（パラメータの値を型の付いた形にしたもの）
struct Settings
{
    float pitch = 12.0f;   // 半音
    float formant = 6.0f;  // 半音
    WarpMode warpMode = WarpMode::formants;
    std::array<float, 4> delta { 0.0f, 0.0f, 0.0f, 0.0f };          // 半音
    std::array<float, 4> anchor { 500.0f, 1400.0f, 2700.0f, 3500.0f }; // Hz
    AboveF4 aboveF4 = AboveF4::followUniform;
    float pitchMin = 80.0f; // Hz
    float pitchMax = 400.0f;
    InputChannel inputChannel = InputChannel::automatic;

    static Settings fromValues (const std::array<float, kNumParams>& v);
};

std::array<float, kNumParams> defaultValues();

} // namespace vcab
