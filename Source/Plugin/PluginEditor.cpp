#include "PluginEditor.h"
#include "EditorStyle.h"

using namespace editor;

namespace
{
constexpr int kHeader = 44;      // 上の段: 名前、入力チャンネル、（右から）設定値初期化、オーディオ設定
constexpr int kStripRow = 34;    // 操作の帯の 1 行
constexpr int kButtonWidth = 120;
constexpr int kInputComboWidth = 176; // VcabDenoise の入力チャンネルの選択欄と同じ幅
constexpr int kComboExtra = 46;       // 選択欄の、文字の幅に足す分（左右の余白と、右の矢印）
constexpr int kLabelInset = 5;        // Label の文字の左の余白（JUCE の標準）
constexpr int kLabelGap = 12;         // 名前と部品の隙間

// 画面に出すパラメータ（F1〜F4 の差と位置は、グラフの点で操作する）
const vcab::P kRows[] = { vcab::P::pitch, vcab::P::formant, vcab::P::warpMode, vcab::P::aboveF4,
                          vcab::P::pitchMin, vcab::P::pitchMax, vcab::P::inputChannel };
} // namespace

VcabShiftEditor::VcabShiftEditor (VcabShiftProcessor& p)
    : AudioProcessorEditor (p), proc (p), spectrum (p)
{
    setLookAndFeel (&lookAndFeel);
    auto& apvts = proc.state();

    for (auto id : kRows)
    {
        const auto& info = vcab::paramInfo (id);
        auto r = std::make_unique<Row>();
        r->id = id;
        r->label.setText (utf8 (info.name), juce::dontSendNotification);
        r->label.setColour (juce::Label::textColourId, kText);
        r->label.setTooltip (utf8 (info.help));
        addAndMakeVisible (r->label);
        if (info.type == vcab::ParamType::toggle)
        {
            // チェックボックスは、名前をボタンの横に書く（名前の欄は使わない）
            r->label.setVisible (false);
            r->toggle = std::make_unique<juce::ToggleButton> (utf8 (info.name));
            r->toggle->setTooltip (utf8 (info.help));
            addAndMakeVisible (*r->toggle);
            r->toggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, info.key, *r->toggle);
        }
        else if (info.type == vcab::ParamType::choice)
        {
            r->combo = std::make_unique<juce::ComboBox>();
            for (size_t i = 0; i < info.choices.size(); ++i)
                r->combo->addItem (utf8 (info.choices[i]), static_cast<int> (i) + 1);
            r->combo->setTooltip (utf8 (info.help));
            addAndMakeVisible (*r->combo);
            if (id == vcab::P::inputChannel)
            {
                // 「自動」の項目の文字が変わるので、パラメータとは自前でつなぐ（timerCallback で選択をそろえる）
                r->combo->onChange = [this, combo = r->combo.get()]
                {
                    const int index = combo->getSelectedItemIndex();
                    auto* param = proc.param (vcab::P::inputChannel);
                    if (index < 0 || index == static_cast<int> (std::lround (proc.value (vcab::P::inputChannel))))
                        return;
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (index)));
                    param->endChangeGesture();
                };
            }
            else
                r->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, info.key, *r->combo);
        }
        else
        {
            r->slider = std::make_unique<juce::Slider> (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
            r->slider->setTooltip (utf8 (info.help));
            addAndMakeVisible (*r->slider);
            r->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, info.key, *r->slider);
        }
        rows.push_back (std::move (r));
    }

    resetButton.setButtonText (utf8 ("設定値初期化"));
    resetButton.setTooltip (utf8 ("すべての設定を初期値に戻します（入力チャンネルは戻しません）。押すと確認が出ます。"));
    resetButton.onClick = [this]
    {
        const auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::QuestionIcon, utf8 ("設定値初期化"),
            utf8 ("すべての設定を初期値に戻します。よろしいですか？（入力チャンネルとオーディオデバイスの設定は変わりません）"),
            utf8 ("初期化"), utf8 ("キャンセル"), this);
        juce::Component::SafePointer<VcabShiftEditor> safe (this);
        juce::AlertWindow::showAsync (options, [safe] (int result)
        {
            if (safe == nullptr || result != 1)
                return;
            for (const auto& info : vcab::paramTable())
            {
                if (info.id == vcab::P::inputChannel)
                    continue;
                auto* param = safe->proc.param (info.id);
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->getDefaultValue());
                param->endChangeGesture();
            }
        });
    };
    addAndMakeVisible (resetButton);

    // EXE 版だけ、オーディオの設定画面を開くボタン（「オプション」の中と同じ画面）
    audioSettingsButton.setButtonText (utf8 ("オーディオ設定"));
    audioSettingsButton.setTooltip (utf8 ("オーディオの設定画面（入出力の機器、サンプルレート、バッファの大きさ）を開きます。左上の「オプション」の中と同じ画面です。"));
    audioSettingsButton.onClick = [this]
    {
        if (proc.openAudioSettings)
            proc.openAudioSettings();
    };
    if (proc.openAudioSettings)
        addAndMakeVisible (audioSettingsButton);
    addAndMakeVisible (spectrum);

    setSize (kWidth, kHeight);
    timerCallback();
    startTimerHz (10);
}

VcabShiftEditor::~VcabShiftEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

VcabShiftEditor::Row* VcabShiftEditor::row (vcab::P id)
{
    for (auto& r : rows)
        if (r->id == id)
            return r.get();
    return nullptr;
}

void VcabShiftEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    g.setColour (kText);
    g.setFont (juce::FontOptions (kFont + 3.0f, juce::Font::bold));
    g.drawText ("VcabShift", juce::Rectangle<int> (kMargin, 0, 130, kHeader), juce::Justification::centredLeft);
    g.setColour (kPanel);
    g.fillRoundedRectangle (stripArea.toFloat(), 4.0f);
}

void VcabShiftEditor::resized()
{
    auto r = getLocalBounds().reduced (kMargin, 0);

    const juce::Font font { juce::FontOptions (kFont) };
    auto textWidth = [&font] (const juce::String& t) { return juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, t)); };
    // 名前の欄の幅: 文字の幅＋Label の左の余白＋名前と部品の隙間
    auto labelWidth = [&] (std::initializer_list<vcab::P> ids)
    {
        int w = 0;
        for (auto id : ids)
            w = std::max (w, textWidth (row (id)->label.getText()));
        return w + kLabelInset + kLabelGap;
    };

    // 上の段: 名前（左）。右端に寄せて、入力チャンネル（VcabDenoise と同じ幅）、オーディオ設定、設定値初期化（ボタンは同じ幅）
    {
        auto h = r.removeFromTop (kHeader).reduced (0, 5);
        resetButton.setBounds (h.removeFromRight (kButtonWidth));
        h.removeFromRight (6);
        if (audioSettingsButton.isVisible())
        {
            audioSettingsButton.setBounds (h.removeFromRight (kButtonWidth));
            h.removeFromRight (10);
        }
        else
            h.removeFromRight (4);
        auto* input = row (vcab::P::inputChannel);
        input->combo->setBounds (h.removeFromRight (kInputComboWidth));
        input->label.setBounds (h.removeFromRight (labelWidth ({ vcab::P::inputChannel })));
    }

    // 操作の帯（2 行 3 列）: ピッチ・フォルマント ｜ 探索下限・探索上限 ｜ 動かし方・F4以降
    // 右の列の選択欄は、中の文字が切れない幅（2 つのうち広いほう）にそろえ、残りを左の 2 列で分ける（バーの長さは同じ）
    stripArea = r.removeFromTop (kStripRow * 2 + 14);
    {
        auto s = stripArea.reduced (10, 7);
        auto comboWidth = [&] (vcab::P id)
        {
            int w = 0;
            for (const auto* c : vcab::paramInfo (id).choices)
                w = std::max (w, textWidth (utf8 (c)));
            return w + kComboExtra;
        };
        const int label1 = labelWidth ({ vcab::P::pitch, vcab::P::formant });
        const int label2 = labelWidth ({ vcab::P::pitchMin, vcab::P::pitchMax });
        const int label3 = labelWidth ({ vcab::P::warpMode, vcab::P::aboveF4 });
        const int combo3 = std::max (comboWidth (vcab::P::warpMode), comboWidth (vcab::P::aboveF4));
        const int gap = 18;
        auto col3 = s.removeFromRight (label3 + combo3);
        s.removeFromRight (gap);
        const int bar = (s.getWidth() - gap - label1 - label2) / 2;
        auto col1 = s.removeFromLeft (label1 + bar);
        s.removeFromLeft (gap);
        auto col2 = s.removeFromLeft (label2 + bar);

        auto place = [this] (vcab::P id, juce::Rectangle<int> line, int labelW, int controlW)
        {
            auto* rw = row (id);
            line = line.reduced (0, 3);
            rw->label.setBounds (line.removeFromLeft (labelW));
            line = line.removeFromLeft (controlW);
            if (rw->slider != nullptr)
                rw->slider->setBounds (line);
            else
                rw->combo->setBounds (line);
        };
        place (vcab::P::pitch, col1.removeFromTop (kStripRow), label1, bar);
        place (vcab::P::formant, col1.removeFromTop (kStripRow), label1, bar);
        place (vcab::P::pitchMin, col2.removeFromTop (kStripRow), label2, bar);
        place (vcab::P::pitchMax, col2.removeFromTop (kStripRow), label2, bar);
        place (vcab::P::warpMode, col3.removeFromTop (kStripRow), label3, combo3);
        place (vcab::P::aboveF4, col3.removeFromTop (kStripRow), label3, combo3);
    }

    // スペクトル（残り全部）
    r.removeFromTop (6);
    spectrum.setBounds (r.removeFromTop (r.getHeight() - kMargin));
}

void VcabShiftEditor::timerCallback()
{
    // 一律のときは F4以降を使わないので、灰色にする
    if (auto* r = row (vcab::P::aboveF4))
    {
        const bool on = proc.currentSettings().warpMode != vcab::WarpMode::uniform;
        r->label.setAlpha (on ? 1.0f : 0.45f);
        r->combo->setEnabled (on);
        r->combo->setAlpha (on ? 1.0f : 0.45f);
    }

    // 入力チャンネルの「自動」に、いま使っているチャンネルを添える
    if (auto* r = row (vcab::P::inputChannel))
    {
        const auto text = utf8 ("自動（") + (proc.status().channel.load() == 1 ? "2" : "1") + utf8 ("ch を使用中）");
        const auto& info = vcab::paramInfo (vcab::P::inputChannel);
        auto* combo = r->combo.get();
        if (combo->getItemText (0) != text && ! combo->isPopupActive())
        {
            combo->clear (juce::dontSendNotification);
            combo->addItem (text, 1);
            for (size_t i = 1; i < info.choices.size(); ++i)
                combo->addItem (utf8 (info.choices[i]), static_cast<int> (i) + 1);
        }
        const int index = static_cast<int> (std::lround (proc.value (vcab::P::inputChannel)));
        if (combo->getSelectedItemIndex() != index)
            combo->setSelectedItemIndex (index, juce::dontSendNotification);
    }
}
