#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 見た目の設定（VcabDenoise と同じ見た目）。文字の大きさの下限を kMinFontSize にそろえる
// （JUCE の標準は小さくて読みにくいため）。EXE 版の外枠にも使う。
class VCLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr float kMinFontSize = 17.0f;

    VCLookAndFeel();

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;

    // マウスが avoid の上にあるときは、ツールチップを横に広げて背を低くし、avoid の上に出す（グラフを隠さないため）。
    // avoid の上に入らなければ、窓の一番上から出す（重なるのは avoid の上端だけ）。
    // parent: ツールチップの窓の親（座標をそろえるため。親がなければ nullptr）
    void setTooltipAvoid (juce::Component* avoid, juce::Component* parent) { tooltipAvoid = avoid; tooltipParent = parent; }
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
    juce::Font getAlertWindowTitleFont() override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

private:
    juce::Component::SafePointer<juce::Component> tooltipAvoid, tooltipParent;

    juce::TextLayout tooltipLayout (const juce::String& text, juce::Colour colour, float width) const;
};
