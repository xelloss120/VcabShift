// 入出力のスペクトルと、フォルマントの動く量の線を重ねたグラフ。
// 縦軸は左がスペクトルの大きさ（dB）、右がフォルマントのつまみからの差（半音）。F1〜F4 の点は可変の EQ のようにドラッグで動かす。
#pragma once

#include "PluginProcessor.h"
#include "Dsp/Fft.h"
#include "Dsp/WarpMap.h"

#include <juce_gui_basics/juce_gui_basics.h>

class SpectrumView : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::Timer
{
public:
    explicit SpectrumView (VcabShiftProcessor&);
    ~SpectrumView() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr int kFftSize = 4096;
    static constexpr float kMinDb = -96.0f, kMaxDb = 0.0f;
    static constexpr float kShiftRange = 12.0f; // 右の縦軸（つまみからの差）±12 半音
    static constexpr float kMinHz = 50.0f;

    VcabShiftProcessor& proc;
    vcab::Fft fft;
    std::vector<float> window, inRing, outRing, inPow, outPow, inDb, outDb, pullBuf;
    int ringPos = 0;
    std::vector<vcab::Fft::Complex> work;

    vcab::WarpMap warp;
    vcab::Settings settings;
    std::array<bool, 4> tracked {};

    enum class Drag { none, point };
    Drag drag = Drag::none;
    int dragPoint = -1;
    int hoverPoint = -1;

    void timerCallback() override;
    void updateWarp();
    void computeSpectrum (const std::vector<float>& ring, std::vector<float>& power, std::vector<float>& db);

    juce::Rectangle<float> plotArea() const;
    float maxHz() const;
    float xForHz (float hz) const;
    float hzForX (float x) const;
    float yForDb (float db) const;
    float yForShift (float st) const;
    float shiftForY (float y) const;

    bool pointsVisible() const;
    juce::Point<float> pointPos (int i) const;
    int hitPoint (juce::Point<float> p) const;

    void setParam (vcab::P id, float v);
    void begin (vcab::P id);
    void end (vcab::P id);
    static vcab::P deltaId (int i) { return static_cast<vcab::P> (static_cast<int> (vcab::P::f1Delta) + i); }
    static vcab::P freqId (int i) { return static_cast<vcab::P> (static_cast<int> (vcab::P::f1Freq) + i); }
};
