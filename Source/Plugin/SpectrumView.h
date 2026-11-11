// 入出力のスペクトルと、フォルマントの動く量の線を重ねたグラフ。
// 縦軸は左がスペクトルの大きさ（dB）、右がフォルマントのつまみからの差（半音）。F1〜F4 の点は可変の EQ のようにドラッグで動かす。
// 有声（桃の線と丸）と無声（緑の線とひし形）を重ねて出す。無声の点は位置が有声と同じなので、重ならないよう少し右にずらして描き、上下だけ動かす。
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

    vcab::WarpMap warp, warpU; // 有声、無声
    vcab::Settings settings;
    std::array<bool, 4> tracked {};

    enum class Drag { none, point };
    Drag drag = Drag::none;
    int dragPoint = -1;   // 0〜3 が有声の F1〜F4、4〜7 が無声の F1〜F4
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
    static vcab::P unvoicedDeltaId (int i) { return static_cast<vcab::P> (static_cast<int> (vcab::P::uf1Delta) + i); }
    // 点の番号（0〜7）から、差のパラメータと、その今の値
    static vcab::P pointDeltaId (int i) { return i < 4 ? deltaId (i) : unvoicedDeltaId (i - 4); }
    float pointDelta (int i) const { return i < 4 ? settings.delta[static_cast<size_t> (i)] : settings.unvoicedDelta[static_cast<size_t> (i - 4)]; }
    // 右の目盛り（フォルマントのつまみからの差）の上で、差 0 の点が来る高さ（有声は 0、無声は 無声フォルマント − フォルマント）
    float pointOffset (int i) const { return i < 4 ? 0.0f : settings.unvoicedFormant - settings.formant; }
};
