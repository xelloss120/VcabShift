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

    static constexpr int kWidth = 800, kHeight = 600;

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

    // 「声に合わせる」: 探索下限を一時的に一番下にして声の高さを集め、低い側に余裕を足して探索下限にする（上限は変えない）
    juce::TextButton fitButton;
    bool measuring = false;
    double measureStart = 0.0;     // 測り始めた時刻（ms）
    double messageUntil = 0.0;     // ボタンに結果の文字を出しておく時刻（ms）
    float savedMin = 0.0f; // 測る前の探索下限（取りやめたときに戻す）
    std::vector<float> f0Samples;
    int64_t lastFrameCount = -1;

    void startMeasure();
    void finishMeasure (bool apply);
    void updateMeasure(); // タイマーごと: 声の高さを集め、足りたら（または時間切れで）終える
    void setParamValue (vcab::P id, float v);

    void timerCallback() override;
    Row* row (vcab::P id);
};
