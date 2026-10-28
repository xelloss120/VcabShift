// 画面の色と大きさ（VcabDenoise と同じ値）
#pragma once

#include "LookAndFeel.h"

namespace editor
{

inline juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

// 地の色
inline const juce::Colour kBackground { 0xff17202e };
inline const juce::Colour kPanel { 0xff1e2a3c };
inline const juce::Colour kText { 0xffe2e8f2 };
inline const juce::Colour kGraph { 0xff0f1724 }; // スペクトルの下地
inline const juce::Colour kGrid { 0x22ffffff };
inline const juce::Colour kDim { 0xff7a8494 };

// 線の色
inline const juce::Colour kInputColour { 0xff4fa3e0 };  // 入力: 青
inline const juce::Colour kOutputColour { 0xfff0a040 }; // 出力: 橙
inline const juce::Colour kWarpColour { 0xffe870a8 };   // フォルマントの動く量: 桃（アイコンの帯も同じ色）

inline constexpr float kFont = VCLookAndFeel::kMinFontSize;
inline constexpr int kMargin = 8;

} // namespace editor
