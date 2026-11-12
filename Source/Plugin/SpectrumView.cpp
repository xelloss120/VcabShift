#include "SpectrumView.h"
#include "EditorStyle.h"

#include <cmath>

using namespace editor;

namespace
{
constexpr float kHitRadius = 11.0f;
constexpr float kPointRadius = 6.5f;
constexpr float kUnvoicedOffset = 13.0f; // 無声の点を、有声の点から右へずらして描く量（画素）
constexpr float kSmooth = 0.6f; // 前のフレームの重み（パワー）
} // namespace

SpectrumView::SpectrumView (VcabShiftProcessor& p) : proc (p)
{
    fft.prepare (kFftSize);
    window.resize (kFftSize);
    float sum = 0.0f;
    for (int i = 0; i < kFftSize; ++i)
    {
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / kFftSize);
        sum += window[static_cast<size_t> (i)];
    }
    for (auto& w : window)
        w *= 2.0f / sum; // 振幅 1 の正弦波が 0dB になるように
    inRing.assign (kFftSize, 0.0f);
    outRing.assign (kFftSize, 0.0f);
    inPow.assign (kFftSize / 2 + 1, 0.0f);
    outPow.assign (kFftSize / 2 + 1, 0.0f);
    inDb.assign (kFftSize / 2 + 1, kMinDb);
    outDb.assign (kFftSize / 2 + 1, kMinDb);
    pullBuf.assign (SpectrumFifo::kSize, 0.0f);
    work.assign (kFftSize, {});

    setTooltip (utf8 ("青: 入力、橙: 出力のスペクトル（左の目盛り）。\n"
                      "桃の線と丸: 有声音で、周波数ごとに有声フォルマントのつまみからさらに動かす量（右の目盛り。0 はつまみのとおり）。\n"
                      "緑の線とひし形: 無声音（さ行・は行などの息の音）の動く量。目盛りは桃の線と同じく、有声フォルマントのつまみからの差です\n"
                      "（無声フォルマントが有声フォルマントより 3 半音小さければ、差 0 のひし形は -3 の高さに来ます。ひし形の数字は、無声フォルマントからの差）。\n"
                      "F1〜F4 の丸を上下にドラッグ（左右は声に合わせて動きます）。動かし方が「帯域固定」のときは、上下左右にドラッグ。\n"
                      "ひし形は上下だけ（位置は丸と同じで、重ならないよう少し右に描いています）。\n"
                      "ホイールで 0.5 半音ずつ。ダブルクリックで 0 に戻します。\n"
                      "薄い青の範囲: 声の高さを探す範囲（探索下限〜上限）。\n"
                      "下端の帯: 直近 3 秒の有声（青）と無声（灰）。右端が今で、左へ流れます（周波数の目盛りとは関係ありません）。"));
    updateWarp();
    startTimerHz (30);
}

SpectrumView::~SpectrumView()
{
    stopTimer();
}

void SpectrumView::timerCallback()
{
    auto drain = [this] (SpectrumFifo& fifo, std::vector<float>& ring, int pos)
    {
        const int n = fifo.pull (pullBuf.data(), static_cast<int> (pullBuf.size()));
        for (int i = 0; i < n; ++i)
            ring[static_cast<size_t> ((pos + i) % kFftSize)] = pullBuf[static_cast<size_t> (i)];
        return n;
    };
    const int nIn = drain (proc.inputFifo, inRing, ringPos);
    const int nOut = drain (proc.outputFifo, outRing, ringPos);
    ringPos = (ringPos + std::max (nIn, nOut)) % kFftSize;

    computeSpectrum (inRing, inPow, inDb);
    computeSpectrum (outRing, outPow, outDb);
    updateWarp();
    repaint();
}

void SpectrumView::computeSpectrum (const std::vector<float>& ring, std::vector<float>& power, std::vector<float>& db)
{
    for (int i = 0; i < kFftSize; ++i)
        work[static_cast<size_t> (i)] = ring[static_cast<size_t> ((ringPos + i) % kFftSize)] * window[static_cast<size_t> (i)];
    fft.transform (work.data(), kFftSize, false);
    // パワーのままならし、描く値（dB）はそこから求める
    for (int b = 0; b <= kFftSize / 2; ++b)
    {
        auto& p = power[static_cast<size_t> (b)];
        p = kSmooth * p + (1.0f - kSmooth) * std::norm (work[static_cast<size_t> (b)]);
        db[static_cast<size_t> (b)] = juce::jlimit (kMinDb - 20.0f, kMaxDb + 20.0f, 10.0f * std::log10 (p + 1.0e-14f));
    }
}

void SpectrumView::updateWarp()
{
    settings = proc.currentSettings();
    std::array<float, 4> anchors = settings.anchor;
    const auto& st = proc.status();
    for (int k = 0; k < 4; ++k)
        tracked[static_cast<size_t> (k)] = st.tracked[static_cast<size_t> (k)].load();
    if (settings.warpMode == vcab::WarpMode::formants)
        for (int k = 0; k < 4; ++k)
            anchors[static_cast<size_t> (k)] = st.formant[static_cast<size_t> (k)].load();
    warp.build (settings.formant, settings.delta, anchors, settings.aboveF4);
    warpU.build (settings.unvoicedFormant, settings.unvoicedDelta, anchors, settings.unvoicedAboveF4);
}

juce::Rectangle<float> SpectrumView::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (46.0f).withTrimmedRight (52.0f).withTrimmedTop (30.0f).withTrimmedBottom (26.0f);
}

float SpectrumView::maxHz() const
{
    const double fs = proc.sampleRate();
    return static_cast<float> (std::min (20000.0, fs > 0.0 ? fs * 0.5 : 24000.0));
}

float SpectrumView::xForHz (float hz) const
{
    const auto a = plotArea();
    const float t = std::log (hz / kMinHz) / std::log (maxHz() / kMinHz);
    return a.getX() + t * a.getWidth();
}

float SpectrumView::hzForX (float x) const
{
    const auto a = plotArea();
    const float t = (x - a.getX()) / a.getWidth();
    return kMinHz * std::pow (maxHz() / kMinHz, t);
}

float SpectrumView::yForDb (float db) const
{
    const auto a = plotArea();
    return a.getY() + (kMaxDb - db) / (kMaxDb - kMinDb) * a.getHeight();
}

float SpectrumView::yForShift (float st) const
{
    const auto a = plotArea();
    return a.getCentreY() - st / kShiftRange * a.getHeight() * 0.5f;
}

float SpectrumView::shiftForY (float y) const
{
    const auto a = plotArea();
    return (a.getCentreY() - y) / (a.getHeight() * 0.5f) * kShiftRange;
}

bool SpectrumView::pointsVisible() const
{
    return settings.warpMode != vcab::WarpMode::uniform;
}

juce::Point<float> SpectrumView::pointPos (int i) const
{
    const float f = warp.anchorPositions()[static_cast<size_t> (i % 4)];
    // 縦は、有声・無声とも、フォルマントのつまみからの差（無声は無声フォルマントとの違いの分だけずれる）。目盛りの外は端に置く
    return { xForHz (f) + (i < 4 ? 0.0f : kUnvoicedOffset),
             yForShift (juce::jlimit (-kShiftRange, kShiftRange, pointOffset (i) + pointDelta (i))) };
}

int SpectrumView::hitPoint (juce::Point<float> p) const
{
    if (! pointsVisible())
        return -1;
    int best = -1;
    float bestD = kHitRadius;
    for (int i = 0; i < 8; ++i)
    {
        const float d = pointPos (i).getDistanceFrom (p);
        if (d <= bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void SpectrumView::paint (juce::Graphics& g)
{
    g.fillAll (kGraph);
    const auto a = plotArea();
    g.setFont (juce::FontOptions (kFont - 3.0f));

    // 目盛り: 周波数
    const float freqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float f : freqs)
    {
        if (f > maxHz())
            continue;
        const float x = xForHz (f);
        g.setColour (kGrid);
        g.drawVerticalLine (juce::roundToInt (x), a.getY(), a.getBottom());
        g.setColour (kDim);
        const auto label = f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String (f, 0);
        g.drawText (label, juce::Rectangle<float> (x - 30.0f, a.getBottom() + 3.0f, 60.0f, 18.0f), juce::Justification::centred);
    }
    // 目盛り: 左 dB
    for (float db = kMinDb; db <= kMaxDb; db += 24.0f)
    {
        const float y = yForDb (db);
        g.setColour (kGrid);
        g.drawHorizontalLine (juce::roundToInt (y), a.getX(), a.getRight());
        g.setColour (kDim);
        g.drawText (juce::String (db, 0), juce::Rectangle<float> (0.0f, y - 9.0f, a.getX() - 6.0f, 18.0f), juce::Justification::centredRight);
    }
    // 目盛り: 右 半音
    for (float st = -kShiftRange; st <= kShiftRange; st += 3.0f)
    {
        const float y = yForShift (st);
        g.setColour (kWarpColour.withAlpha (st == 0.0f ? 0.35f : 0.12f));
        g.drawHorizontalLine (juce::roundToInt (y), a.getX(), a.getRight());
        g.setColour (kWarpColour.withAlpha (0.8f));
        g.drawText ((st > 0 ? "+" : "") + juce::String (st, 0), juce::Rectangle<float> (a.getRight() + 6.0f, y - 9.0f, 46.0f, 18.0f),
                    juce::Justification::centredLeft);
    }
    g.setColour (kDim);
    g.drawText ("dB", juce::Rectangle<float> (0.0f, 4.0f, a.getX() - 6.0f, 20.0f), juce::Justification::centredRight);
    g.setColour (kWarpColour.withAlpha (0.8f));
    g.drawText (utf8 ("差"), juce::Rectangle<float> (a.getRight() + 6.0f, 4.0f, 46.0f, 20.0f), juce::Justification::centredLeft);

    // 凡例
    {
        float x = a.getX() + 4.0f;
        auto legend = [&] (juce::Colour c, const char* text)
        {
            g.setColour (c);
            g.fillRect (x, 13.0f, 16.0f, 3.0f);
            g.setColour (kText);
            const auto s = utf8 (text);
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), s) + 4.0f;
            g.drawText (s, juce::Rectangle<float> (x + 20.0f, 4.0f, w, 20.0f), juce::Justification::centredLeft);
            x += 26.0f + w;
        };
        legend (kInputColour, "入力");
        legend (kOutputColour, "出力");
        legend (kWarpColour, "有声の動く量（有声フォルマントからの差）");
        legend (kUnvoicedWarpColour, "無声の動く量");
    }

    // 声の高さと遅延（グラフの中の右上）
    {
        const float f0 = proc.status().f0.load();
        const double fs = proc.sampleRate();
        juce::String text = utf8 ("声の高さ ");
        if (f0 > 0.0f)
            text << juce::roundToInt (f0) << " → " << juce::roundToInt (f0 * std::exp2 (settings.pitch / 12.0f)) << " Hz";
        else
            text << utf8 ("（無声）");
        if (fs > 0.0)
            text << utf8 ("　遅延 ") << juce::String (proc.latency() * 1000.0 / fs, 1) << " ms";
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 12.0f;
        const auto box = juce::Rectangle<float> (a.getRight() - w - 4.0f, a.getY() + 4.0f, w, 20.0f);
        g.setColour (kGraph.withAlpha (0.8f));
        g.fillRect (box);
        g.setColour (kText);
        g.drawText (text, box, juce::Justification::centred);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // 声の高さを探す範囲（探索下限〜上限。上限は下限の 1.5 倍より狭くしない、処理と同じ値）を薄く塗る
    {
        const float x0 = xForHz (settings.pitchMin), x1 = xForHz (settings.pitchMax);
        g.setColour (kInputColour.withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float> (x0, a.getY(), x1 - x0, a.getHeight()));
        g.setColour (kInputColour.withAlpha (0.3f));
        g.drawVerticalLine (juce::roundToInt (x0), a.getY(), a.getBottom());
        g.drawVerticalLine (juce::roundToInt (x1), a.getY(), a.getBottom());
    }

    // スペクトル
    const double fs = proc.sampleRate() > 0.0 ? proc.sampleRate() : 48000.0;
    auto spectrumPath = [&] (const std::vector<float>& db)
    {
        juce::Path path;
        bool started = false;
        for (float x = a.getX(); x <= a.getRight(); x += 1.0f)
        {
            // 画素の幅の中の一番大きい区画（低い側は補間）
            const float f0 = hzForX (x), f1 = hzForX (x + 1.0f);
            const float b0 = static_cast<float> (f0 * kFftSize / fs), b1 = static_cast<float> (f1 * kFftSize / fs);
            float v;
            if (b1 - b0 < 1.0f)
            {
                const int i = juce::jlimit (0, kFftSize / 2 - 1, static_cast<int> (b0));
                const float t = b0 - static_cast<float> (i);
                v = db[static_cast<size_t> (i)] + t * (db[static_cast<size_t> (i + 1)] - db[static_cast<size_t> (i)]);
            }
            else
            {
                v = kMinDb - 20.0f;
                for (int i = static_cast<int> (b0); i <= std::min (static_cast<int> (b1), kFftSize / 2); ++i)
                    v = std::max (v, db[static_cast<size_t> (i)]);
            }
            const float y = yForDb (v);
            if (! started)
            {
                path.startNewSubPath (x, y);
                started = true;
            }
            else
                path.lineTo (x, y);
        }
        return path;
    };
    // 声の高さ（入力: 青、ピッチを変えた後: 橙の点線）
    const float f0 = proc.status().f0.load();
    if (f0 > 0.0f)
    {
        const float dash[] = { 4.0f, 4.0f };
        const float outF0 = f0 * std::exp2 (settings.pitch / 12.0f);
        for (auto [hz, colour] : { std::pair<float, juce::Colour> { f0, kInputColour }, { outF0, kOutputColour } })
        {
            if (hz < kMinHz || hz > maxHz())
                continue;
            const float x = xForHz (hz);
            juce::Path line;
            line.startNewSubPath (x, a.getY());
            line.lineTo (x, a.getBottom());
            juce::Path dashed;
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, line, dash, 2);
            g.setColour (colour.withAlpha (0.7f));
            g.fillPath (dashed);
        }
    }

    g.setColour (kInputColour.withAlpha (0.85f));
    g.strokePath (spectrumPath (inDb), juce::PathStrokeType (1.3f));
    g.setColour (kOutputColour.withAlpha (0.9f));
    g.strokePath (spectrumPath (outDb), juce::PathStrokeType (1.3f));

    // 動く量の線（無声を先に描き、有声を手前に）
    auto warpPath = [&] (const vcab::WarpMap& w, float knob)
    {
        juce::Path path;
        for (float x = a.getX(); x <= a.getRight(); x += 2.0f)
        {
            const float y = yForShift (juce::jlimit (-kShiftRange, kShiftRange, w.shiftAt (hzForX (x)) - knob));
            if (x == a.getX())
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }
        return path;
    };
    g.setColour (kUnvoicedWarpColour);
    g.strokePath (warpPath (warpU, settings.formant), juce::PathStrokeType (1.8f)); // 無声も、フォルマントのつまみからの差で描く
    g.setColour (kWarpColour);
    g.strokePath (warpPath (warp, settings.formant), juce::PathStrokeType (2.2f));

    // 有声・無声の履歴（下端の帯。右端が今。スペクトルの線に隠れないよう、線より手前に描く）
    {
        constexpr int kFrames = 600; // 5ms ごとに 3 秒
        const auto strip = a.withTop (a.getBottom() - 5.0f);
        g.setColour (kUnvoicedColour);
        g.fillRect (strip);
        g.setColour (kInputColour);
        const auto& st = proc.status();
        const int64_t count = st.voicingCount.load();
        const float frameWidth = strip.getWidth() / kFrames;
        const int frames = static_cast<int> (std::min<int64_t> (kFrames, count));
        auto voicedAt = [&] (int i) { return st.voicing[static_cast<size_t> ((count - 1 - i) % vcab::EngineStatus::kVoicingHistory)].load(); };
        // 続いている有声を 1 つの四角で描く（フレームごとに描くと、継ぎ目がまだらになる）
        for (int i = 0; i < frames; ++i)
        {
            if (! voicedAt (i))
                continue;
            int end = i + 1;
            while (end < frames && voicedAt (end))
                ++end;
            const float x0 = strip.getRight() - frameWidth * static_cast<float> (end);
            g.fillRect (juce::Rectangle<float> (x0, strip.getY(), frameWidth * static_cast<float> (end - i), strip.getHeight()));
            i = end;
        }
    }
    g.restoreState();

    // F1〜F4 の点と数字（数字はほかの点と前の数字を避けて置き、点は数字より手前に描く）。0〜3 が有声の丸、4〜7 が無声のひし形
    if (pointsVisible())
    {
        const bool formants = settings.warpMode == vcab::WarpMode::formants;
        constexpr int kPoints = 8;
        juce::Rectangle<float> circles[kPoints], labels[kPoints];
        for (int i = 0; i < kPoints; ++i)
        {
            const float r = (i == hoverPoint || i == dragPoint) ? kPointRadius + 2.0f : kPointRadius;
            circles[i] = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pointPos (i));
        }
        for (int i = 0; i < kPoints; ++i)
        {
            const auto p = pointPos (i);
            const float f = warp.anchorPositions()[static_cast<size_t> (i % 4)];
            const float d = pointDelta (i);
            const auto diff = utf8 ("差") + (d >= 0.0f ? "+" : "") + juce::String (d, 1);
            const auto text = i < 4 ? "F" + juce::String (i + 1) + " " + juce::String (juce::roundToInt (f)) + "Hz " + diff
                                    : utf8 ("無声") + " " + diff;
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 6.0f;
            // 点の上下（近い順）、次に左右へずらした所を試し、重ならない最初の所に置く。どこも重なるなら、重なりが一番小さい所
            const float offsets[] = { -30.0f, 12.0f, -52.0f, 34.0f, -74.0f, 56.0f, -96.0f, 78.0f };
            const float sides[] = { 0.0f, -(w * 0.5f + 10.0f), w * 0.5f + 10.0f };
            juce::Rectangle<float> box;
            float bestArea = -1.0f;
            for (float side : sides)
            {
                for (float off : offsets)
                {
                    const auto cand = juce::Rectangle<float> (p.x - w * 0.5f + side, p.y + off, w, 18.0f).constrainedWithin (a);
                    float area = 0.0f;
                    for (int j = 0; j < kPoints; ++j)
                    {
                        area += cand.getIntersection (circles[j].expanded (2.0f)).getWidth() * cand.getIntersection (circles[j].expanded (2.0f)).getHeight();
                        if (j < i)
                            area += cand.getIntersection (labels[j]).getWidth() * cand.getIntersection (labels[j]).getHeight();
                    }
                    if (bestArea < 0.0f || area < bestArea)
                    {
                        bestArea = area;
                        box = cand;
                    }
                    if (area <= 0.0f)
                        break;
                }
                if (bestArea <= 0.0f)
                    break;
            }
            labels[i] = box;
            g.setColour (kGraph.withAlpha (0.7f));
            g.fillRect (box);
            g.setColour (i < 4 ? kText : kUnvoicedWarpColour);
            g.drawText (text, box, juce::Justification::centred);
        }
        // 無声のひし形を先に、有声の丸を手前に
        for (int i = kPoints - 1; i >= 0; --i)
        {
            const auto colour = i < 4 ? kWarpColour : kUnvoicedWarpColour;
            juce::Path shape;
            if (i < 4)
                shape.addEllipse (circles[i]);
            else
            {
                const auto c = circles[i];
                shape.startNewSubPath (c.getCentreX(), c.getY());
                shape.lineTo (c.getRight(), c.getCentreY());
                shape.lineTo (c.getCentreX(), c.getBottom());
                shape.lineTo (c.getX(), c.getCentreY());
                shape.closeSubPath();
            }
            if (! formants || tracked[static_cast<size_t> (i % 4)])
            {
                g.setColour (colour);
                g.fillPath (shape);
            }
            else
            {
                g.setColour (kGraph);
                g.fillPath (shape);
                g.setColour (colour);
                g.strokePath (shape, juce::PathStrokeType (2.0f));
            }
        }
    }
}

void SpectrumView::setParam (vcab::P id, float v)
{
    auto* p = proc.param (id);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

void SpectrumView::begin (vcab::P id) { proc.param (id)->beginChangeGesture(); }
void SpectrumView::end (vcab::P id) { proc.param (id)->endChangeGesture(); }

void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    const int before = hoverPoint;
    hoverPoint = hitPoint (e.position);
    setMouseCursor (hoverPoint >= 0 ? (settings.warpMode == vcab::WarpMode::bands && hoverPoint < 4 ? juce::MouseCursor::UpDownLeftRightResizeCursor
                                                                                  : juce::MouseCursor::UpDownResizeCursor)
                                    : juce::MouseCursor::NormalCursor);
    if (before != hoverPoint)
        repaint();
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    updateWarp();
    dragPoint = hitPoint (e.position);
    if (dragPoint >= 0)
    {
        drag = Drag::point;
        begin (pointDeltaId (dragPoint));
        if (settings.warpMode == vcab::WarpMode::bands && dragPoint < 4)
            begin (freqId (dragPoint));
    }
    else
        drag = Drag::none;
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::point)
    {
        const int i = dragPoint;
        const float delta = juce::jlimit (-12.0f, 12.0f, shiftForY (e.position.y) - pointOffset (i));
        setParam (pointDeltaId (i), delta);
        if (settings.warpMode == vcab::WarpMode::bands && i < 4)
        {
            // 範囲と、隣の点との間隔（1.15 倍）の中に収める
            const auto& r = vcab::kFormantRanges[static_cast<size_t> (i)];
            float lo = r.lo, hi = r.hi;
            if (i > 0)
                lo = std::max (lo, settings.anchor[static_cast<size_t> (i - 1)] * vcab::kMinAnchorRatio);
            if (i < 3)
                hi = std::min (hi, settings.anchor[static_cast<size_t> (i + 1)] / vcab::kMinAnchorRatio);
            if (lo <= hi)
                setParam (freqId (i), juce::jlimit (lo, hi, hzForX (e.position.x)));
        }
    }
    updateWarp();
    repaint();
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::point)
    {
        end (pointDeltaId (dragPoint));
        if (settings.warpMode == vcab::WarpMode::bands && dragPoint < 4)
            end (freqId (dragPoint));
    }
    drag = Drag::none;
    dragPoint = -1;
    repaint();
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = hitPoint (e.position);
    if (i < 0)
        return;
    begin (pointDeltaId (i));
    setParam (pointDeltaId (i), 0.0f);
    end (pointDeltaId (i));
    if (settings.warpMode == vcab::WarpMode::bands && i < 4)
    {
        begin (freqId (i));
        setParam (freqId (i), vcab::paramInfo (freqId (i)).defaultValue);
        end (freqId (i));
    }
    updateWarp();
    repaint();
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float step = (w.deltaY > 0.0f ? 0.5f : -0.5f);
    if (w.deltaY == 0.0f)
        return;
    const int i = hitPoint (e.position);
    if (i >= 0)
    {
        begin (pointDeltaId (i));
        setParam (pointDeltaId (i), juce::jlimit (-12.0f, 12.0f, pointDelta (i) + step));
        end (pointDeltaId (i));
    }
    updateWarp();
    repaint();
}
