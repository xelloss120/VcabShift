#include "PitchRepair.h"

#include "MathConstants.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr float kSilentDb = -60.0f;
} // namespace

void PitchRepair::prepare (double lowRate, int maxWindow)
{
    fsd = lowRate;
    humWin.assign (static_cast<size_t> (std::max (1, maxWindow)), 0.0f);
    reset();
}

void PitchRepair::reset()
{
    floorDb = -200.0f;
    lastVoicedF0 = 0.0f;
    sinceVoiced = 1 << 20;
    holdFrames = creakRun = 0;
    typicalLog = typicalWeight = 0.0;
    humFixing = false;
}

void PitchRepair::observeLevel (float levelDb, float hopSeconds)
{
    floorDb = floorDb < -150.0f ? levelDb : std::min (levelDb, floorDb + 3.0f * hopSeconds);
}

float PitchRepair::windowCorr (const float* w, int wp, float lagD, float range, float* bestLag)
{
    if (w == nullptr)
        return 0.0f;
    const int lo = std::max (2, static_cast<int> (std::floor (lagD * (1.0f - range))));
    const int hi = std::min (wp - 40, static_cast<int> (std::ceil (lagD * (1.0f + range))));
    float best = -1.0f;
    for (int l = lo; l <= hi; ++l)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (int i = 0; i < wp - l; ++i)
        {
            const double a = w[i];
            const double b = w[i + l];
            ab += a * b; aa += a * a; bb += b * b;
        }
        const float v = static_cast<float> (ab / std::sqrt (aa * bb + 1.0e-30));
        if (v > best)
        {
            best = v;
            if (bestLag != nullptr)
                *bestLag = static_cast<float> (l);
        }
    }
    return best;
}

float PitchRepair::subharmonicSpread (const float* w, int wp, float f0)
{
    constexpr int kParts = 8, kCols = 2 * kParts;
    if (w == nullptr || wp > static_cast<int> (humWin.size()))
        return 0.0f;
    std::copy (w, w + wp, humWin.begin());

    // ハン窓をかけた最小二乗（正規方程式）
    std::array<double, kCols * kCols> ata {};
    std::array<double, kCols> atb {}, col {};
    const double w0 = 2.0 * kPi * 0.5 * f0 / fsd;
    for (int i = 0; i < wp; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / wp);
        const double c1 = std::cos (w0 * i), s1 = std::sin (w0 * i);
        double c = c1, s = s1;
        for (int k = 0; k < kParts; ++k)
        {
            col[static_cast<size_t> (2 * k)] = c * win;
            col[static_cast<size_t> (2 * k + 1)] = s * win;
            const double cn = c * c1 - s * s1;
            s = s * c1 + c * s1;
            c = cn;
        }
        const double y = humWin[static_cast<size_t> (i)] * win;
        for (int a = 0; a < kCols; ++a)
        {
            atb[static_cast<size_t> (a)] += col[static_cast<size_t> (a)] * y;
            for (int b = a; b < kCols; ++b)
                ata[static_cast<size_t> (a * kCols + b)] += col[static_cast<size_t> (a)] * col[static_cast<size_t> (b)];
        }
    }
    double trace = 0.0;
    for (int a = 0; a < kCols; ++a)
    {
        trace += ata[static_cast<size_t> (a * kCols + a)];
        for (int b = 0; b < a; ++b)
            ata[static_cast<size_t> (a * kCols + b)] = ata[static_cast<size_t> (b * kCols + a)];
    }
    for (int a = 0; a < kCols; ++a)
        ata[static_cast<size_t> (a * kCols + a)] += 1.0e-9 * trace / kCols + 1.0e-30;
    // ガウスの消去法（対称正定値なので、ピボットの入れ替えなし）
    for (int p = 0; p < kCols; ++p)
    {
        const double d = ata[static_cast<size_t> (p * kCols + p)];
        for (int a = p + 1; a < kCols; ++a)
        {
            const double m = ata[static_cast<size_t> (a * kCols + p)] / d;
            for (int b = p; b < kCols; ++b)
                ata[static_cast<size_t> (a * kCols + b)] -= m * ata[static_cast<size_t> (p * kCols + b)];
            atb[static_cast<size_t> (a)] -= m * atb[static_cast<size_t> (p)];
        }
    }
    std::array<double, kCols> x {};
    for (int a = kCols - 1; a >= 0; --a)
    {
        double s = atb[static_cast<size_t> (a)];
        for (int b = a + 1; b < kCols; ++b)
            s -= ata[static_cast<size_t> (a * kCols + b)] * x[static_cast<size_t> (b)];
        x[static_cast<size_t> (a)] = s / ata[static_cast<size_t> (a * kCols + a)];
    }
    // k 倍（k = 1〜8）の強さの 2 乗
    const auto power = [&x] (int k) { return x[static_cast<size_t> (2 * k - 2)] * x[static_cast<size_t> (2 * k - 2)] + x[static_cast<size_t> (2 * k - 1)] * x[static_cast<size_t> (2 * k - 1)]; };
    const double odd = power (3) + power (5) + power (7);
    const double even = power (2) + power (4) + power (6) + power (8);
    const double first = power (1), second = power (2);
    const double oddDb = 10.0 * std::log10 (odd / (even + 1.0e-30) + 1.0e-30);
    const double firstDb = 10.0 * std::log10 (first / (second + 1.0e-30) + 1.0e-30);
    return static_cast<float> (oddDb - firstDb);
}

PitchResult PitchRepair::adjust (const PitchResult& p, const float* w, int wp, float levelDb, float levelPeak, float tilt,
                                 float pitchMin, double timeSec)
{
    PitchResult r = p;
    bool unreliable = false;
    const bool voiceLike = levelDb > kSilentDb && levelDb > floorDb + 20.0f && levelDb > levelPeak - 25.0f && tilt > 0.3f;
    bool held = false;

    // 1. 「んー」の対策（ガラガラ声は、倍の周期がふだんの高さの 1 オクターブ下に来るので当たらない）
    {
        bool fixing = false;
        if (tuning.humEnter > 0.0f && r.voiced && typicalWeight > 40.0 && 0.5f * r.f0 >= pitchMin)
        {
            const double above = std::log2 (static_cast<double> (r.f0)) - typicalLog;
            if (above >= tuning.humLow && above <= tuning.humHigh)
            {
                const float lagD = static_cast<float> (fsd / r.f0);
                const float c1 = windowCorr (w, wp, lagD, 0.03f);
                const float c2 = windowCorr (w, wp, 2.0f * lagD, 0.03f);
                if (c2 >= tuning.humCorr && c2 >= c1 + (humFixing ? tuning.humStay : tuning.humEnter)
                    && (humFixing || subharmonicSpread (w, wp, r.f0) <= tuning.humSpread))
                {
                    r.f0 *= 0.5f;
                    fixing = true;
                }
            }
        }
        humFixing = fixing;
    }

    // 2. ガラガラ声の手当て
    if (tuning.creakHold > 0)
    {
        // その高さの周期か 2 倍の周期で、窓の波形がよく重なるか（息やささやきは重ならない）
        // 重なれば、一番よく重なった周期からの高さ（2 倍の周期なら半分にする）、重ならなければ 0
        const auto periodic = [this, w, wp] (float f0, float range)
        {
            const float lagD = static_cast<float> (fsd / f0);
            float l1 = lagD, l2 = 2.0f * lagD;
            const float c1 = windowCorr (w, wp, lagD, range, &l1);
            const float c2 = windowCorr (w, wp, 2.0f * lagD, range, &l2);
            if (std::max (c1, c2) < tuning.holdCorr)
                return 0.0f;
            return static_cast<float> (c2 >= c1 ? 2.0 * fsd / l2 : fsd / l1);
        };
        const bool continuing = lastVoicedF0 > 0.0f && sinceVoiced <= tuning.holdGap && holdFrames < 400;
        // 取れなかった、または直前の高さから 7 半音より大きく上へ跳んだ。
        // 続いていなければ（発話の始まり）、ふだんの高さから startJump 半音より上（響きの周期を取った）
        const bool learned = typicalWeight > 40.0;
        const float typical = learned ? static_cast<float> (std::exp2 (typicalLog)) : 0.0f;
        // 検出した高さの周期でも窓がよく重なるなら、検出を信じる（本当に跳んだ声を、低いまま保たないように）
        const auto supported = [this, w, wp, &r] { return windowCorr (w, wp, static_cast<float> (fsd / r.f0), 0.03f) >= tuning.holdCorr; };
        bool jumped = false;
        if (r.voiced && continuing)
            jumped = std::log2 (r.f0 / lastVoicedF0) > 7.0f / 12.0f && ! supported();
        else if (r.voiced && learned && tuning.startJump > 0.0f)
        {
            jumped = std::log2 (r.f0 / typical) > tuning.startJump / 12.0f && ! supported();
            unreliable = jumped; // 発話の始まりの、響きの周期を取ったものは、直前の高さとして覚えない
        }
        const bool lost = ! r.voiced || jumped;
        float g = 0.0f;
        int why = lost ? 0 : 9;
        if (lost && ! voiceLike)
            why = 3;
        else if (lost && continuing && (g = periodic (lastVoicedF0, sinceVoiced <= 1 ? tuning.holdStep : tuning.holdRange)) > 0.0f)
        {
            r.voiced = true;
            r.f0 = tuning.holdRange > 0.06f ? g : lastVoicedF0;
            lastVoicedF0 = r.f0; // 保っている間も、重なりの周期について行く
            held = true;
            why = 1;
        }
        else if (lost)
        {
            why = continuing ? 4 : 8;
            if (tuning.creakHold >= 2 && sinceVoiced > 1)
            {
                if (! learned)
                    why = 5;
                else if ((g = periodic (typical, tuning.startRange)) <= 0.0f)
                    why = 6;
                else if (++creakRun < tuning.startFrames)
                    why = 7;
                else
                {
                    // 直前の有声がない: ふだんの高さの近くから始める（startFrames フレーム続いてから）
                    r.voiced = true;
                    r.f0 = g;
                    lastVoicedF0 = r.f0;
                    held = true;
                    why = 2;
                }
            }
        }
        if (holdLog != nullptr)
            holdLog->push_back ({ static_cast<float> (timeSec), static_cast<float> (why), r.voiced ? r.f0 : 0.0f, static_cast<float> (sinceVoiced) });
        if (! voiceLike)
            creakRun = 0;
    }

    // 直前の高さと、ふだんの高さを覚える
    if (r.voiced)
    {
        if (held)
            ++holdFrames;
        else if (! unreliable)
        {
            holdFrames = 0;
            lastVoicedF0 = r.f0;
            // ふだんの高さ: 有声のフレームの log2 の高さの平均（重みは 1000 フレーム = 5 秒でならす）
            typicalWeight = std::min (typicalWeight + 1.0, 1000.0);
            typicalLog += (std::log2 (static_cast<double> (r.f0)) - typicalLog) / typicalWeight;
        }
        if (held || ! unreliable)
            sinceVoiced = 0;
        else
            sinceVoiced = std::min (sinceVoiced + 1, 1 << 20);
    }
    else
    {
        sinceVoiced = std::min (sinceVoiced + 1, 1 << 20);
        holdFrames = 0;
    }
    return r;
}

} // namespace vcab
