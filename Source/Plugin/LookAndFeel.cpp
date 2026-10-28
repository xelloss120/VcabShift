#include "LookAndFeel.h"

namespace
{
juce::Font notSmaller (juce::Font f)
{
    return f.getHeight() < VCLookAndFeel::kMinFontSize ? f.withHeight (VCLookAndFeel::kMinFontSize) : f;
}
constexpr float kTooltipWidth = 420.0f;
} // namespace

VCLookAndFeel::VCLookAndFeel()
{
    setDefaultSansSerifTypefaceName ("Yu Gothic UI");

    // 紺系の配色（JUCE の暗い配色を青寄りにしたもの）
    setColourScheme ({ 0xff1e2a3c,    // ウィンドウの背景
                       0xff121b2a,    // 部品の背景（選択欄、値の欄）
                       0xff1e2a3c,    // メニューの背景
                       0xff4a5d7c,    // 枠線
                       0xffffffff,    // 文字
                       0xff4a90e0,    // 塗り（スライダー、チェック）
                       0xffffffff,    // 選んだ項目の文字
                       0xff2e4f80,    // 選んだ項目の背景
                       0xffffffff }); // メニューの文字
}

juce::TextLayout VCLookAndFeel::tooltipLayout (const juce::String& text, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.append (text, juce::FontOptions (kMinFontSize), colour);
    juce::TextLayout tl;
    tl.createLayout (s, kTooltipWidth);
    return tl;
}

juce::Rectangle<int> VCLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto tl = tooltipLayout (text, juce::Colours::black);
    const int w = static_cast<int> (tl.getWidth() + 22.0f);
    const int h = static_cast<int> (tl.getHeight() + 16.0f);
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

void VCLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height));
    g.setColour (juce::Colour (0xff0f1724));
    g.fillRect (bounds);
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawRect (bounds, 1.0f);
    tooltipLayout (text, juce::Colour (0xfff0f0f0)).draw (g, bounds.reduced (10.0f, 7.0f));
}

juce::Font VCLookAndFeel::getLabelFont (juce::Label& l) { return notSmaller (LookAndFeel_V4::getLabelFont (l)); }
juce::Font VCLookAndFeel::getComboBoxFont (juce::ComboBox&) { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getPopupMenuFont() { return juce::FontOptions (kMinFontSize); }

void VCLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight)
{
    LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
    if (! isSeparator)
        idealHeight = juce::jmax (idealHeight, juce::roundToInt (kMinFontSize * 1.3f) + 6);
}

juce::Font VCLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowMessageFont() { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowFont() { return juce::FontOptions (kMinFontSize); }
juce::Font VCLookAndFeel::getAlertWindowTitleFont() { return juce::FontOptions (kMinFontSize + 3.0f, juce::Font::bold); }

// チェックボックスの文字も、ほかの文字と同じ大きさにする
void VCLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    const float box = juce::jmin (kMinFontSize * 1.1f, static_cast<float> (button.getHeight()) - 2.0f);
    drawTickBox (g, button, 4.0f, (static_cast<float> (button.getHeight()) - box) * 0.5f, box, box,
                 button.getToggleState(), button.isEnabled(), highlighted, down);
    g.setColour (button.findColour (juce::ToggleButton::textColourId));
    g.setFont (juce::FontOptions (kMinFontSize));
    if (! button.isEnabled())
        g.setOpacity (0.5f);
    const int textX = juce::roundToInt (box) + 10;
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (textX).withTrimmedRight (2),
                      juce::Justification::centredLeft, 1);
}
