#pragma once

#include "Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>

// 画面のスペクトル用に、音の処理のスレッドから音を渡す輪（書き手 1、読み手 1。ロックしない）
class SpectrumFifo
{
public:
    static constexpr int kSize = 1 << 15;

    void push (const float* x, int n)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (x, x + n1, buffer.begin() + s1);
        std::copy (x + n1, x + n1 + n2, buffer.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }

    // 読めるだけ読む。返り値は読んだ数
    int pull (float* dst, int maxN)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (maxN, s1, n1, s2, n2);
        std::copy (buffer.begin() + s1, buffer.begin() + s1 + n1, dst);
        std::copy (buffer.begin() + s2, buffer.begin() + s2 + n2, dst + n1);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

private:
    juce::AbstractFifo fifo { kSize };
    std::array<float, kSize> buffer {};
};

class VcabShiftProcessor : public juce::AudioProcessor
{
public:
    VcabShiftProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& state() { return apvts; }
    juce::RangedAudioParameter* param (vcab::P id) const { return params[static_cast<size_t> (id)]; }
    float value (vcab::P id) const { return params[static_cast<size_t> (id)]->convertFrom0to1 (params[static_cast<size_t> (id)]->getValue()); }
    vcab::Settings currentSettings() const;

    const vcab::EngineStatus& status() const { return engine.status(); }
    double sampleRate() const { return engine.sampleRate(); }
    int latency() const { return getLatencySamples(); }

    SpectrumFifo inputFifo, outputFifo;

    // EXE 版だけ、外枠が入れる。音声の設定画面を開く（VST3 では空のまま）
    std::function<void()> openAudioSettings;

private:
    juce::AudioProcessorValueTreeState apvts;
    std::array<juce::RangedAudioParameter*, vcab::kNumParams> params {};
    vcab::Engine engine;
    std::vector<float> monoOut, delayedIn;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VcabShiftProcessor)
};
