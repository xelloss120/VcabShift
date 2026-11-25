#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String u8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout VcabShiftProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& p : vcab::paramTable())
    {
        const juce::ParameterID id { p.key, 1 };
        switch (p.type)
        {
            case vcab::ParamType::real:
            {
                juce::NormalisableRange<float> range (p.minValue, p.maxValue, p.step);
                if (p.skewCentre > 0.0f)
                    range.setSkewForCentre (p.skewCentre);
                const auto unit = u8 (p.unit);
                const int decimals = p.step >= 1.0f ? 0 : 1;
                // ピッチとフォルマントは、半音に倍率（%）を併記する（+12 半音 = 200%）。打ち込んだ文字は先頭の数を半音として読む
                const bool ratio = p.id == vcab::P::pitch || p.id == vcab::P::formant || p.id == vcab::P::unvoicedFormant;
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    id, u8 (p.name), range, p.defaultValue,
                    juce::AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (
                        [decimals, unit, ratio] (float v, int)
                        {
                            auto s = juce::String (v, decimals) + " " + unit;
                            if (ratio)
                                s << u8 ("（") << juce::roundToInt (100.0 * std::exp2 (v / 12.0)) << u8 ("%）");
                            return s;
                        })));
                break;
            }
            case vcab::ParamType::choice:
            {
                juce::StringArray choices;
                for (const auto* c : p.choices)
                    choices.add (u8 (c));
                layout.add (std::make_unique<juce::AudioParameterChoice> (id, u8 (p.name), choices, static_cast<int> (p.defaultValue)));
                break;
            }
        }
    }
    return layout;
}

VcabShiftProcessor::VcabShiftProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VcabShift", createLayout())
{
    for (const auto& p : vcab::paramTable())
        params[static_cast<size_t> (p.id)] = apvts.getParameter (p.key);
    startTimerHz (20);
}

VcabShiftProcessor::~VcabShiftProcessor()
{
    stopTimer();
}

void VcabShiftProcessor::timerCallback()
{
    // 探索下限を変えると遅延が変わる。ホストへの報告は、音の処理のスレッドからではなく、ここで行う
    const int latency = engineLatency.load();
    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

bool VcabShiftProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s) { return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo(); };
    return ok (in) && ok (out);
}

vcab::Settings VcabShiftProcessor::currentSettings() const
{
    std::array<float, vcab::kNumParams> v {};
    for (int i = 0; i < vcab::kNumParams; ++i)
        v[static_cast<size_t> (i)] = value (static_cast<vcab::P> (i));
    return vcab::Settings::fromValues (v);
}

void VcabShiftProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (samplesPerBlock, 16));
    engine.setSettings (currentSettings());
    monoOut.assign (static_cast<size_t> (std::max (samplesPerBlock, 16)), 0.0f);
    delayedIn.assign (monoOut.size(), 0.0f);
    // 遅延は一番長くて 60ms（探索下限 40Hz）。余裕を見て 0.25 秒とブロックの分
    const int ring = vcab::Fft::nextPow2 (static_cast<int> (sampleRate * 0.25) + static_cast<int> (monoOut.size()));
    for (auto& r : dryRing)
        r.assign (static_cast<size_t> (ring), 0.0f);
    dryMask = ring - 1;
    dryPos = 0;
    engineLatency.store (engine.latencySamples());
    setLatencySamples (engine.latencySamples());
}

void VcabShiftProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

void VcabShiftProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, true);
}

// バイパス中も処理は続ける（止めると、戻したときに溜まっていた古い音が出るため）。出す音だけを、遅らせた入力に替える
void VcabShiftProcessor::run (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    engine.setSettings (currentSettings());
    const int latency = engine.latencySamples();
    engineLatency.store (latency);

    const float* in[2] = { buffer.getReadPointer (0), numIn > 1 ? buffer.getReadPointer (1) : nullptr };
    for (int done = 0; done < n;)
    {
        const int len = std::min (n - done, static_cast<int> (monoOut.size()));
        const float* chunk[2] = { in[0] + done, in[1] != nullptr ? in[1] + done : nullptr };
        // 入力を溜める（出力と同じバッファなので、書き込む前に。入力が 1 チャンネルなら、両方に同じ音）
        for (size_t c = 0; c < dryRing.size(); ++c)
        {
            const float* src = chunk[c] != nullptr ? chunk[c] : chunk[0];
            for (int i = 0; i < len; ++i)
                dryRing[c][static_cast<size_t> ((dryPos + i) & dryMask)] = src[i];
        }
        engine.process (chunk, std::min (numIn, 2), monoOut.data(), len, delayedIn.data());
        inputFifo.push (delayedIn.data(), len);
        for (int c = 0; c < numOut; ++c)
        {
            float* dst = buffer.getWritePointer (c) + done;
            if (! bypassed)
            {
                std::copy (monoOut.begin(), monoOut.begin() + len, dst);
                continue;
            }
            const auto& dry = dryRing[static_cast<size_t> (std::min (c, 1))];
            for (int i = 0; i < len; ++i)
                dst[i] = dry[static_cast<size_t> ((dryPos + i - latency) & dryMask)];
        }
        // 画面の出力のスペクトルは、実際に出した音（バイパス中は遅らせた入力）
        outputFifo.push (bypassed && numOut > 0 ? buffer.getReadPointer (0) + done : monoOut.data(), len);
        dryPos += len;
        done += len;
    }
}

juce::AudioProcessorEditor* VcabShiftProcessor::createEditor()
{
    return new VcabShiftEditor (*this);
}

void VcabShiftProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VcabShiftProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VcabShiftProcessor();
}
