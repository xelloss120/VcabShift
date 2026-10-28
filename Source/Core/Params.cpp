#include "Params.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace vcab
{

namespace
{

std::vector<ParamInfo> makeTable()
{
    using T = ParamType;
    std::vector<ParamInfo> t {
        { P::pitch, "pitch", "ピッチ", T::real, -24.0f, 24.0f, 12.0f, 0.1f, 0.0f, "半音", {},
          "声の高さを動かす量。+12 で 1 オクターブ上がります。" },
        { P::formant, "formant", "フォルマント", T::real, -24.0f, 24.0f, 6.0f, 0.1f, 0.0f, "半音", {},
          "声色（声道の響き）を動かす量。上げると小柄な声に、下げると大柄な声になります。" },
        { P::warpMode, "warp_mode", "動かし方", T::choice, 0.0f, 2.0f, 1.0f, 1.0f, 0.0f, "",
          { "一律", "フォルマント追従", "帯域固定" },
          "周波数ごとに、声色をどれだけ動かすか。\n"
          "一律: どの周波数も、フォルマントのつまみの分だけ動かします（F1〜F4 の点と F4以降は使いません）。\n"
          "フォルマント追従: 声から追いかけた F1〜F4 の位置ごとに、別の量だけ動かします。点は声に合わせて左右に動き、上下だけ操作できます。\n"
          "帯域固定: 決まった位置ごとに、別の量だけ動かします。点は上下左右にドラッグして動かせます。" },
        { P::f1Delta, "f1_delta", "F1 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F1 を、フォルマントのつまみからさらに動かす量。" },
        { P::f2Delta, "f2_delta", "F2 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F2 を、フォルマントのつまみからさらに動かす量。" },
        { P::f3Delta, "f3_delta", "F3 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F3 を、フォルマントのつまみからさらに動かす量。" },
        { P::f4Delta, "f4_delta", "F4 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F4 を、フォルマントのつまみからさらに動かす量。" },
        { P::f1Freq, "f1_freq", "F1 の位置", T::real, kFormantRanges[0].lo, kFormantRanges[0].hi, 500.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F1 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f2Freq, "f2_freq", "F2 の位置", T::real, kFormantRanges[1].lo, kFormantRanges[1].hi, 1400.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F2 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f3Freq, "f3_freq", "F3 の位置", T::real, kFormantRanges[2].lo, kFormantRanges[2].hi, 2700.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F3 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f4Freq, "f4_freq", "F4 の位置", T::real, kFormantRanges[3].lo, kFormantRanges[3].hi, 3500.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F4 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::aboveF4, "above_f4", "F4以降", T::choice, 0.0f, 2.0f, 1.0f, 1.0f, 0.0f, "",
          { "F4合わせ", "設定合わせ", "地声合わせ" },
          "F4 より上の周波数を、どれだけ動かすか。\n"
          "F4合わせ: F4 と同じだけ。\n"
          "設定合わせ: フォルマントのつまみの分だけ。\n"
          "地声合わせ: 動かさない。" },
        { P::pitchMin, "pitch_min", "探索下限", T::real, 40.0f, 200.0f, 80.0f, 1.0f, 0.0f, "Hz", {},
          "声の高さを探す範囲の下限。遅延はこの値で決まります（周期の 2 倍。80Hz で 25ms）。声の低さに合わせて、なるべく高くすると遅延が減ります。" },
        { P::pitchMax, "pitch_max", "探索上限", T::real, 150.0f, 1000.0f, 400.0f, 1.0f, 0.0f, "Hz", {},
          "声の高さを探す範囲の上限。遅延には関係しません。" },
        { P::inputChannel, "input_channel", "入力チャンネル", T::choice, 0.0f, 3.0f, 0.0f, 1.0f, 0.0f, "",
          { "自動", "1ch", "2ch", "平均" },
          "マイクの入力チャンネル。自動は、1ch と 2ch の大きいほうを使います（もう片方が 2 秒続けて 6dB 以上大きいと切り替え）。" },
    };

    // 表の並びと P の並びがそろっていることを確かめる（書き漏れ、並べ違いの防止）
    assert (static_cast<int> (t.size()) == kNumParams);
    for (int i = 0; i < kNumParams; ++i)
        assert (static_cast<int> (t[static_cast<size_t> (i)].id) == i);
    return t;
}

} // namespace

const std::vector<ParamInfo>& paramTable()
{
    static const std::vector<ParamInfo> table = makeTable();
    return table;
}

const ParamInfo& paramInfo (P id)
{
    return paramTable()[static_cast<size_t> (id)];
}

std::array<float, kNumParams> defaultValues()
{
    std::array<float, kNumParams> v {};
    for (const auto& p : paramTable())
        v[static_cast<size_t> (p.id)] = p.defaultValue;
    return v;
}

Settings Settings::fromValues (const std::array<float, kNumParams>& v)
{
    auto get = [&v] (P id) { return v[static_cast<size_t> (id)]; };
    auto getInt = [&] (P id) { return static_cast<int> (std::lround (get (id))); };

    Settings s;
    s.pitch = get (P::pitch);
    s.formant = get (P::formant);
    s.warpMode = static_cast<WarpMode> (std::clamp (getInt (P::warpMode), 0, 2));
    s.delta = { get (P::f1Delta), get (P::f2Delta), get (P::f3Delta), get (P::f4Delta) };
    s.anchor = { get (P::f1Freq), get (P::f2Freq), get (P::f3Freq), get (P::f4Freq) };
    s.aboveF4 = static_cast<AboveF4> (std::clamp (getInt (P::aboveF4), 0, 2));
    if (s.warpMode == WarpMode::uniform)
    {
        // 一律: F1〜F4 の差と F4以降は使わない（どの周波数もつまみの分だけ）
        s.delta = { 0.0f, 0.0f, 0.0f, 0.0f };
        s.aboveF4 = AboveF4::followUniform;
    }
    s.pitchMin = get (P::pitchMin);
    // 上限が下限の 1.5 倍より狭いときは、下限の 1.5 倍として扱う（つまみの値は書き換えない）
    s.pitchMax = std::max (get (P::pitchMax), s.pitchMin * 1.5f);
    s.inputChannel = static_cast<InputChannel> (std::clamp (getInt (P::inputChannel), 0, 3));
    return s;
}

} // namespace vcab
