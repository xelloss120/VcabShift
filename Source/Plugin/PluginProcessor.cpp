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
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    id, u8 (p.name), range, p.defaultValue,
                    juce::AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (
                        [decimals, unit] (float v, int) { return juce::String (v, decimals) + " " + unit; })));
                break;
            }
            case vcab::ParamType::integer:
                layout.add (std::make_unique<juce::AudioParameterInt> (id, u8 (p.name), static_cast<int> (p.minValue), static_cast<int> (p.maxValue),
                                                                       static_cast<int> (p.defaultValue),
                                                                       juce::AudioParameterIntAttributes().withLabel (u8 (p.unit))));
                break;
            case vcab::ParamType::toggle:
                layout.add (std::make_unique<juce::AudioParameterBool> (id, u8 (p.name), p.defaultValue >= 0.5f));
                break;
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
    setLatencySamples (engine.latencySamples());
}

void VcabShiftProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    engine.setSettings (currentSettings());
    if (engine.latencySamples() != getLatencySamples())
        setLatencySamples (engine.latencySamples());

    const float* in[2] = { buffer.getReadPointer (0), numIn > 1 ? buffer.getReadPointer (1) : nullptr };
    for (int done = 0; done < n;)
    {
        const int len = std::min (n - done, static_cast<int> (monoOut.size()));
        const float* chunk[2] = { in[0] + done, in[1] != nullptr ? in[1] + done : nullptr };
        engine.process (chunk, std::min (numIn, 2), monoOut.data() + 0, len, delayedIn.data());
        inputFifo.push (delayedIn.data(), len);
        outputFifo.push (monoOut.data(), len);
        for (int c = 0; c < numOut; ++c)
            std::copy (monoOut.begin(), monoOut.begin() + len, buffer.getWritePointer (c) + done);
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
