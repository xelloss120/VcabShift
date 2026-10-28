#pragma once

#include "LookAndFeel.h"
#include "PluginProcessor.h"
#include "SpectrumView.h"

#include <juce_audio_processors/juce_audio_processors.h>

class VcabShiftEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit VcabShiftEditor (VcabShiftProcessor&);
    ~VcabShiftEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kWidth = 800, kHeight = 520;

private:
    struct Row
    {
        vcab::P id;
        juce::Label label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttachment;
    };

    VcabShiftProcessor& proc;
    VCLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltip { this, 600 };
    std::vector<std::unique_ptr<Row>> rows;
    SpectrumView spectrum;
    juce::TextButton resetButton, audioSettingsButton;
    juce::Rectangle<int> stripArea; // 操作の帯（paint で下地を描く）

    void timerCallback() override;
    Row* row (vcab::P id);
};
