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
        { P::formant, "formant", "有声フォルマント", T::real, -24.0f, 24.0f, 4.0f, 0.1f, 0.0f, "半音", {},
          "有声音（母音など、声帯が震える音）の声色（声道の響き）を動かす量。上げると小柄な声に、下げると大柄な声になります。\n"
          "無声音（さ行・は行などの息の音）には使わず、「無声フォルマント」を使います。" },
        { P::unvoicedFormant, "unvoiced_formant", "無声フォルマント", T::real, -24.0f, 24.0f, 4.0f, 0.1f, 0.0f, "半音", {},
          "無声音（さ行・は行などの息の音）の声色を動かす量。有声フォルマントと別に決められます。\n"
          "F1〜F4 の位置は有声音と同じで、差（グラフの緑のひし形）と F4 より上の扱い（無声F4以降）は、無声音で別に決められます。\n"
          "上げすぎると、さ行がチリチリ、シャリシャリと鋭くなります。" },
        { P::warpMode, "warp_mode", "動かし方", T::choice, 0.0f, 2.0f, 1.0f, 1.0f, 0.0f, "",
          { "一律", "フォルマント追従", "帯域固定" },
          "周波数ごとに、声色をどれだけ動かすか。\n"
          "一律: どの周波数も、有声フォルマントのつまみの分だけ動かします（F1〜F4 の点、有声F4以降、無声F4以降は使いません）。\n"
          "フォルマント追従: 声から追いかけた F1〜F4 の位置ごとに、別の量だけ動かします。点は声に合わせて左右に動き、上下だけ操作できます。\n"
          "帯域固定: 決まった位置ごとに、別の量だけ動かします。点は上下左右にドラッグして動かせます。" },
        { P::f1Delta, "f1_delta", "F1 の差", T::real, -12.0f, 12.0f, 2.0f, 0.1f, 0.0f, "半音", {},
          "F1 を、有声フォルマントのつまみからさらに動かす量。" },
        { P::f2Delta, "f2_delta", "F2 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F2 を、有声フォルマントのつまみからさらに動かす量。" },
        { P::f3Delta, "f3_delta", "F3 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "F3 を、有声フォルマントのつまみからさらに動かす量。" },
        { P::f4Delta, "f4_delta", "F4 の差", T::real, -12.0f, 12.0f, -1.0f, 0.1f, 0.0f, "半音", {},
          "F4 を、有声フォルマントのつまみからさらに動かす量。" },
        { P::f1Freq, "f1_freq", "F1 の位置", T::real, kFormantRanges[0].lo, kFormantRanges[0].hi, 500.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F1 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f2Freq, "f2_freq", "F2 の位置", T::real, kFormantRanges[1].lo, kFormantRanges[1].hi, 1400.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F2 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f3Freq, "f3_freq", "F3 の位置", T::real, kFormantRanges[2].lo, kFormantRanges[2].hi, 2700.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F3 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::f4Freq, "f4_freq", "F4 の位置", T::real, kFormantRanges[3].lo, kFormantRanges[3].hi, 3500.0f, 1.0f, 0.0f, "Hz", {},
          "動かし方が「帯域固定」のときの F4 の位置。「フォルマント追従」でも、声から追いかけられないと、この位置に戻ります。" },
        { P::aboveF4, "above_f4", "有声F4以降", T::choice, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, "",
          { "F4合わせ", "設定合わせ", "地声合わせ" },
          "F4 より上の周波数を、どれだけ動かすか。\n"
          "F4合わせ: F4 と同じだけ。\n"
          "設定合わせ: 有声フォルマントのつまみの分だけ。\n"
          "地声合わせ: 動かさない。" },
        { P::uf1Delta, "uf1_delta", "無声 F1 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "無声音の F1 を、無声フォルマントのつまみからさらに動かす量（位置は有声音と同じ）。" },
        { P::uf2Delta, "uf2_delta", "無声 F2 の差", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "半音", {},
          "無声音の F2 を、無声フォルマントのつまみからさらに動かす量（位置は有声音と同じ）。" },
        { P::uf3Delta, "uf3_delta", "無声 F3 の差", T::real, -12.0f, 12.0f, -2.0f, 0.1f, 0.0f, "半音", {},
          "無声音の F3 を、無声フォルマントのつまみからさらに動かす量（位置は有声音と同じ）。" },
        { P::uf4Delta, "uf4_delta", "無声 F4 の差", T::real, -12.0f, 12.0f, -2.0f, 0.1f, 0.0f, "半音", {},
          "無声音の F4 を、無声フォルマントのつまみからさらに動かす量（位置は有声音と同じ）。" },
        { P::unvoicedAboveF4, "unvoiced_above_f4", "無声F4以降", T::choice, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, "",
          { "F4合わせ", "設定合わせ", "地声合わせ" },
          "無声音で、F4 より上の周波数をどれだけ動かすか。さ行・は行の主な成分（4〜8kHz）は、ほとんど F4 より上にあります。\n"
          "F4合わせ: 無声の F4 と同じだけ。\n"
          "設定合わせ: 無声フォルマントのつまみの分だけ。\n"
          "地声合わせ: 動かさない。" },
        { P::eqF0, "eq_f0", "EQ 基音", T::real, -12.0f, 12.0f, 3.0f, 0.1f, 0.0f, "dB", {},
          "出力の声の高さ（基音）の所を、持ち上げる（下げる）量。中心は声の高さに合わせて動きます（幅 2/3 オクターブ）。" },
        { P::eqF1, "eq_f1", "EQ F1", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "dB", {},
          "有声音で、動かした後の F1 の山を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqF2, "eq_f2", "EQ F2", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "dB", {},
          "有声音で、動かした後の F2 の山を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqF3, "eq_f3", "EQ F3", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "dB", {},
          "有声音で、動かした後の F3 の山を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqF4, "eq_f4", "EQ F4", T::real, -12.0f, 12.0f, -3.0f, 0.1f, 0.0f, "dB", {},
          "有声音で、動かした後の F4 の山を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqUf1, "eq_uf1", "無声 EQ F1", T::real, -12.0f, 12.0f, -3.0f, 0.1f, 0.0f, "dB", {},
          "無声音で、動かした後の F1 の所を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqUf2, "eq_uf2", "無声 EQ F2", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "dB", {},
          "無声音で、動かした後の F2 の所を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqUf3, "eq_uf3", "無声 EQ F3", T::real, -12.0f, 12.0f, 0.0f, 0.1f, 0.0f, "dB", {},
          "無声音で、動かした後の F3 の所を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::eqUf4, "eq_uf4", "無声 EQ F4", T::real, -12.0f, 12.0f, -3.0f, 0.1f, 0.0f, "dB", {},
          "無声音で、動かした後の F4 の所を、持ち上げる（下げる）量（幅 1 オクターブ）。" },
        { P::pitchMin, "pitch_min", "探索下限", T::real, 40.0f, 200.0f, 80.0f, 1.0f, 0.0f, "Hz", {},
          "声の高さを探す範囲の下限。遅延はこの値で決まります（周期の 2 倍 ＋ 10ms。80Hz で 35ms）。声の低さに合わせて、なるべく高くすると遅延が減ります。" },
        { P::pitchMax, "pitch_max", "探索上限", T::real, 150.0f, 1000.0f, 600.0f, 1.0f, 0.0f, "Hz", {},
          "声の高さを探す範囲の上限。遅延には関係しません。笑い声や叫び声で高くなる分まで入るよう、広めにしておくのがおすすめです。" },
        { P::inputChannel, "input_channel", "入力チャンネル", T::choice, 0.0f, 3.0f, 0.0f, 1.0f, 0.0f, "",
          { "自動", "1ch", "2ch", "平均" },
          "マイクの入力チャンネル。自動は、1ch と 2ch の大きいほうを使います（もう片方が 2 秒続けて 6dB 以上大きいと切り替え）。" },
    };

    // 表の並びと P の並びがそろっていることを確かめる（書き漏れ、並べ違いの防止）
    assert (static_cast<int> (t.size()) == kNumParams);
    for (int i = 0; i < kNumParams; ++i)
        assert (static_cast<int> (t[static_cast<size_t> (i)].id) == i);

    // Settings の仮の初期値が、表の初期値と同じことを確かめる（表だけ変えて、仮の値が古いまま残らないように）
    std::array<float, kNumParams> v {};
    for (const auto& p : t)
        v[static_cast<size_t> (p.id)] = p.defaultValue;
    const Settings a {};
    const Settings b = Settings::fromValues (v);
    assert (a.pitch == b.pitch && a.formant == b.formant && a.unvoicedFormant == b.unvoicedFormant && a.warpMode == b.warpMode
            && a.delta == b.delta && a.anchor == b.anchor && a.aboveF4 == b.aboveF4
            && a.unvoicedDelta == b.unvoicedDelta && a.unvoicedAboveF4 == b.unvoicedAboveF4
            && a.eq == b.eq && a.unvoicedEq == b.unvoicedEq
            && a.pitchMin == b.pitchMin && a.pitchMax == b.pitchMax && a.inputChannel == b.inputChannel);
    (void) a;
    (void) b;
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
    s.unvoicedFormant = get (P::unvoicedFormant);
    s.warpMode = static_cast<WarpMode> (std::clamp (getInt (P::warpMode), 0, 2));
    s.delta = { get (P::f1Delta), get (P::f2Delta), get (P::f3Delta), get (P::f4Delta) };
    s.anchor = { get (P::f1Freq), get (P::f2Freq), get (P::f3Freq), get (P::f4Freq) };
    s.aboveF4 = static_cast<AboveF4> (std::clamp (getInt (P::aboveF4), 0, 2));
    s.unvoicedDelta = { get (P::uf1Delta), get (P::uf2Delta), get (P::uf3Delta), get (P::uf4Delta) };
    s.unvoicedAboveF4 = static_cast<AboveF4> (std::clamp (getInt (P::unvoicedAboveF4), 0, 2));
    if (s.warpMode == WarpMode::uniform)
    {
        // 一律: F1〜F4 の差と F4以降は使わない（どの周波数もつまみの分だけ）
        s.delta = { 0.0f, 0.0f, 0.0f, 0.0f };
        s.aboveF4 = AboveF4::followUniform;
        s.unvoicedDelta = { 0.0f, 0.0f, 0.0f, 0.0f };
        s.unvoicedAboveF4 = AboveF4::followUniform;
    }
    // EQ は動かし方によらず使う（一律でも、追いかけた F1〜F4 に追従する）
    s.eq = { get (P::eqF0), get (P::eqF1), get (P::eqF2), get (P::eqF3), get (P::eqF4) };
    s.unvoicedEq = { get (P::eqUf1), get (P::eqUf2), get (P::eqUf3), get (P::eqUf4) };
    s.pitchMin = get (P::pitchMin);
    // 上限が下限の 1.5 倍より狭いときは、下限の 1.5 倍として扱う（つまみの値は書き換えない）
    s.pitchMax = std::max (get (P::pitchMax), s.pitchMin * 1.5f);
    s.inputChannel = static_cast<InputChannel> (std::clamp (getInt (P::inputChannel), 0, 3));
    return s;
}

} // namespace vcab
