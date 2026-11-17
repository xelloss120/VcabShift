// 入出力のスペクトルと、フォルマントの動く量の線（または声に追従する EQ の線）を重ねたグラフ。
// 右上の切り替えで、出す点を選ぶ。
// フォルマント: 縦軸は左がスペクトルの大きさ（dB）、右がフォルマントのつまみからの差（半音）。F1〜F4 の点は可変の EQ のようにドラッグで動かす。
//   有声（桃の線と丸）と無声（緑の線とひし形）を重ねて出す。無声の点は位置が有声と同じなので、重ならないよう少し右にずらして描き、上下だけ動かす。
// EQ: 右の縦軸は EQ の量（dB）。有声は 基音・F1〜F4 の丸、無声は F1〜F4 のひし形。位置は動かした後（出力の山の所）で、上下だけ動かす。
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
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr int kFftSize = 4096;
    static constexpr float kMinDb = -96.0f, kMaxDb = 0.0f;
    static constexpr float kShiftRange = 12.0f; // 右の縦軸（つまみからの差 ±12 半音、EQ では ±12dB）
    static constexpr float kMinHz = 50.0f;

    VcabShiftProcessor& proc;
    vcab::Fft fft;
    std::vector<float> window, inRing, outRing, inPow, outPow, inDb, outDb, pullBuf;
    int inRingPos = 0, outRingPos = 0; // 輪の、次に書く位置（一番古い標本の位置でもある）
    std::vector<vcab::Fft::Complex> work;

    vcab::WarpMap warp, warpU; // 有声、無声
    vcab::Settings settings;
    std::array<bool, 4> tracked {};
    std::array<float, 4> eqCentreV {}, eqCentreU {}; // EQ の F1〜F4 の位置（動かした後）
    float lastF0 = 0.0f;  // 最後に有声だったときの入力の高さ（無声の間も、EQ の基音の点はそこに置く）
    bool voicedNow = false;

    juce::TextButton formantTab, eqTab;

    enum class Drag { none, point };
    Drag drag = Drag::none;
    int dragPoint = -1;   // フォルマント: 0〜3 が有声の F1〜F4、4〜7 が無声の F1〜F4。EQ: 0 が基音、1〜4 が有声の F1〜F4、5〜8 が無声の F1〜F4
    int hoverPoint = -1;

    void timerCallback() override;
    void updateWarp();
    void setEqView (bool on);
    void computeSpectrum (const std::vector<float>& ring, int ringPos, std::vector<float>& power, std::vector<float>& db);

    juce::Rectangle<float> plotArea() const;
    float maxHz() const;
    float xForHz (float hz) const;
    float hzForX (float x) const;
    float yForDb (float db) const;
    float yForShift (float st) const;
    float shiftForY (float y) const;

    bool eqView() const { return proc.eqView; }
    bool pointsVisible() const;
    int numPoints() const { return eqView() ? 9 : 8; }
    bool unvoicedPoint (int i) const { return i >= (eqView() ? 5 : 4); }
    // 左右にも動かせる点（帯域固定のフォルマントの有声の点）
    bool movesSideways (int i) const { return ! eqView() && settings.warpMode == vcab::WarpMode::bands && i < 4; }
    float pointHz (int i) const;
    juce::Point<float> pointPos (int i) const;
    int hitPoint (juce::Point<float> p) const;

    void setParam (vcab::P id, float v);
    void begin (vcab::P id);
    void end (vcab::P id);
    static vcab::P freqId (int i) { return static_cast<vcab::P> (static_cast<int> (vcab::P::f1Freq) + i); }
    // 点の番号から、操作するパラメータと、その今の値
    vcab::P pointParam (int i) const;
    float pointValue (int i) const;
    // 右の目盛りの上で、値 0 の点が来る高さ（フォルマントの無声は 無声フォルマント − フォルマント、ほかは 0）
    float pointOffset (int i) const { return ! eqView() && i >= 4 ? settings.unvoicedFormant - settings.formant : 0.0f; }
};
