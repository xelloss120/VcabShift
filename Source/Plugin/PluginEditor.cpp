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
constexpr int kTimerHz = 30;          // 画面の更新（「声に合わせる」は、この回数ごとに声の高さを 1 つ集める）

// 「声に合わせる」
constexpr double kFitSettleMs = 300.0;     // 探索下限を下げた直後は、処理の作り直しを待って集めない
constexpr double kFitTimeoutMs = 15000.0;  // この時間で声が足りなければやめる
constexpr double kFitMessageMs = 3000.0;   // 結果をボタンに出しておく時間
constexpr int kFitNeeded = 3 * kTimerHz;   // 有声の値を 3 秒分集めたら終わる
constexpr int kFitMinimum = 1 * kTimerHz;  // 時間切れのとき、これだけあれば使う
constexpr float kFitLowPct = 0.05f;        // 測った声の低い側（5% 点）から
constexpr float kFitMargin = 5.0f;         // この半音だけ下を探索下限にする

// 画面に出すパラメータ（F1〜F4 の差と位置は、グラフの点で操作する）
const vcab::P kRows[] = { vcab::P::pitch, vcab::P::formant, vcab::P::unvoicedFormant, vcab::P::warpMode, vcab::P::aboveF4, vcab::P::unvoicedAboveF4,
                          vcab::P::pitchMin, vcab::P::pitchMax, vcab::P::inputChannel };
} // namespace

VcabShiftEditor::VcabShiftEditor (VcabShiftProcessor& p)
    : AudioProcessorEditor (p), proc (p), spectrum (p)
{
    setLookAndFeel (&lookAndFeel);
    lookAndFeel.setTooltipAvoid (&spectrum, this); // グラフの説明は、グラフの外に出す
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
            if (safe->measuring)
                safe->finishMeasure (false);
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
    fitButton.setButtonText (utf8 ("声に合わせる"));
    fitButton.setTooltip (utf8 ("押してから数秒、ふだんの声でしゃべると、探索下限をその声の低さに合わせます"
                                "（測った声の低い側から 5 半音下）。声が高い人ほど下限が上がり、遅延が減ります。探索上限は変えません。\n"
                                "測っている間は探索下限を 40Hz に下げるので、押したときと終わったときに音が一瞬途切れ、遅延も一時的に増えます。\n"
                                "測っている間にもう一度押すと、取りやめて元の値に戻します。"));
    fitButton.onClick = [this]
    {
        if (measuring)
            finishMeasure (false);
        else
            startMeasure();
    };
    addAndMakeVisible (fitButton);
    addAndMakeVisible (spectrum);

    setSize (kWidth, kHeight);
    timerCallback();
    startTimerHz (kTimerHz);
}

VcabShiftEditor::~VcabShiftEditor()
{
    stopTimer();
    if (measuring)
        finishMeasure (false); // 画面を閉じたら、測る前の範囲に戻す
    setLookAndFeel (nullptr);
}

void VcabShiftEditor::setParamValue (vcab::P id, float v)
{
    auto* p = proc.param (id);
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (v));
    p->endChangeGesture();
}

void VcabShiftEditor::startMeasure()
{
    savedMin = proc.value (vcab::P::pitchMin);
    setParamValue (vcab::P::pitchMin, vcab::paramInfo (vcab::P::pitchMin).minValue);
    f0Samples.clear();
    measuring = true;
    measureStart = juce::Time::getMillisecondCounterHiRes();
    messageUntil = 0.0;
}

void VcabShiftEditor::finishMeasure (bool apply)
{
    measuring = false;
    const int n = static_cast<int> (f0Samples.size());
    if (apply && n >= kFitMinimum)
    {
        std::sort (f0Samples.begin(), f0Samples.end());
        auto at = [&] (float q) { return f0Samples[static_cast<size_t> (juce::jlimit (0, n - 1, juce::roundToInt (q * static_cast<float> (n - 1))))]; };
        const auto& minInfo = vcab::paramInfo (vcab::P::pitchMin);
        const float lo = juce::jlimit (minInfo.minValue, minInfo.maxValue, std::round (at (kFitLowPct) * std::exp2 (-kFitMargin / 12.0f)));
        setParamValue (vcab::P::pitchMin, lo);
        fitButton.setButtonText (utf8 ("探索下限を ") + juce::String (juce::roundToInt (lo)) + utf8 (" Hz にしました"));
    }
    else
    {
        setParamValue (vcab::P::pitchMin, savedMin);
        fitButton.setButtonText (apply ? utf8 ("声を拾えませんでした") : utf8 ("取りやめました"));
    }
    messageUntil = juce::Time::getMillisecondCounterHiRes() + kFitMessageMs;
}

void VcabShiftEditor::updateMeasure()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! measuring)
    {
        // 結果の文字を出し終えたら、ボタンの名前に戻す
        if (messageUntil > 0.0 && now >= messageUntil)
        {
            messageUntil = 0.0;
            fitButton.setButtonText (utf8 ("声に合わせる"));
        }
        return;
    }

    // 音の処理が進んでいるとき（フレームが増えたとき）だけ集める（止まっていると、最後の値が残るため）
    const double elapsed = now - measureStart;
    const auto& st = proc.status();
    const int64_t frames = st.voicingCount.load();
    const float f0 = st.f0.load();
    if (elapsed >= kFitSettleMs && frames != lastFrameCount && f0 > 0.0f)
        f0Samples.push_back (f0);
    lastFrameCount = frames;

    const int n = static_cast<int> (f0Samples.size());
    if (n >= kFitNeeded || elapsed >= kFitTimeoutMs)
        finishMeasure (true);
    else
        fitButton.setButtonText (utf8 ("しゃべってください ") + juce::String (static_cast<float> (n) / kTimerHz, 1)
                                 + " / " + juce::String (kFitNeeded / kTimerHz) + utf8 (" 秒"));
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

    // 操作の帯（3 行 3 列）: 声に合わせる・探索下限・探索上限 ｜ ピッチ・有声フォルマント・無声フォルマント ｜ 動かし方・有声F4以降・無声F4以降
    // 右の列の選択欄は、中の文字が切れない幅（2 つのうち広いほう）にそろえ、残りを左の 2 列で分ける（バーの長さは同じ）
    stripArea = r.removeFromTop (kStripRow * 3 + 14);
    {
        auto s = stripArea.reduced (10, 7);
        auto comboWidth = [&] (vcab::P id)
        {
            int w = 0;
            for (const auto* c : vcab::paramInfo (id).choices)
                w = std::max (w, textWidth (utf8 (c)));
            return w + kComboExtra;
        };
        const int label1 = labelWidth ({ vcab::P::pitchMin, vcab::P::pitchMax });
        const int label2 = labelWidth ({ vcab::P::pitch, vcab::P::formant, vcab::P::unvoicedFormant });
        const int label3 = labelWidth ({ vcab::P::warpMode, vcab::P::aboveF4, vcab::P::unvoicedAboveF4 });
        const int combo3 = std::max ({ comboWidth (vcab::P::warpMode), comboWidth (vcab::P::aboveF4), comboWidth (vcab::P::unvoicedAboveF4) });
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
        fitButton.setBounds (col1.removeFromTop (kStripRow).reduced (0, 3));
        place (vcab::P::pitchMin, col1.removeFromTop (kStripRow), label1, bar);
        place (vcab::P::pitchMax, col1.removeFromTop (kStripRow), label1, bar);
        place (vcab::P::pitch, col2.removeFromTop (kStripRow), label2, bar);
        place (vcab::P::formant, col2.removeFromTop (kStripRow), label2, bar);
        place (vcab::P::unvoicedFormant, col2.removeFromTop (kStripRow), label2, bar);
        place (vcab::P::warpMode, col3.removeFromTop (kStripRow), label3, combo3);
        place (vcab::P::aboveF4, col3.removeFromTop (kStripRow), label3, combo3);
        place (vcab::P::unvoicedAboveF4, col3.removeFromTop (kStripRow), label3, combo3);
    }

    // スペクトル（残り全部）
    r.removeFromTop (6);
    spectrum.setBounds (r.removeFromTop (r.getHeight() - kMargin));
}

void VcabShiftEditor::timerCallback()
{
    updateMeasure();

    // 一律のときは有声F4以降と無声F4以降を使わないので、灰色にする
    for (auto id : { vcab::P::aboveF4, vcab::P::unvoicedAboveF4 })
        if (auto* r = row (id))
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
