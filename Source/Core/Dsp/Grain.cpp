#include "Grain.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kMaxCorrection = 2.0723266f; // ±18dB（自然対数の振幅）
constexpr int kMinEnvelopeFft = 512;
} // namespace

void GrainMaker::prepare (double sampleRate, int maxHalfIn, int maxHalfOut, int maxLifter)
{
    fs = sampleRate;
    const int maxIn = 2 * maxHalfIn + 1;
    const int maxOut = 2 * maxHalfOut + 1 + 2 * maxLifter;
    const int n = std::max ({ Fft::nextPow2 (maxIn), Fft::nextPow2 (maxOut), kMinEnvelopeFft });
    fft.prepare (n);
    spec.assign (static_cast<size_t> (n), {});
    spec2.assign (static_cast<size_t> (n), {});
    envelope.assign (static_cast<size_t> (n / 2 + 1), 0.0f);
    inverseBins.assign (static_cast<size_t> (n / 2 + 1), 0.0f);
    inverseWarp = nullptr;
    // 伸縮の補間は、元の断片の両側に補間の幅（倍率 4 で 32 標本）が要る
    windowed.assign (static_cast<size_t> (maxIn + 2 * (kZeroCrossings * 4 + 2)), 0.0f);

    kernel.resize (static_cast<size_t> (kZeroCrossings * kTableRes + 2));
    for (size_t i = 0; i < kernel.size(); ++i)
    {
        const double x = static_cast<double> (i) / kTableRes;
        if (x >= kZeroCrossings)
        {
            kernel[i] = 0.0f;
            continue;
        }
        const double s = x < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
        const double t = x / kZeroCrossings; // ブラックマン窓（右半分）
        const double w = 0.42 + 0.5 * std::cos (kPi * t) + 0.08 * std::cos (2.0 * kPi * t);
        kernel[i] = static_cast<float> (s * w);
    }
}

int GrainMaker::readMargin (float halfIn, float ratio)
{
    return static_cast<int> (std::ceil (halfIn)) + static_cast<int> (std::ceil (kZeroCrossings * std::max (1.0f, ratio))) + 2;
}

float GrainMaker::kernelAt (float x) const
{
    const float a = std::abs (x) * kTableRes;
    const int i = static_cast<int> (a);
    if (i >= kZeroCrossings * kTableRes)
        return 0.0f;
    const float t = a - static_cast<float> (i);
    return kernel[static_cast<size_t> (i)] + t * (kernel[static_cast<size_t> (i + 1)] - kernel[static_cast<size_t> (i)]);
}

// 窓をかけた断片の包絡（ケプストラムで、lifter 標本より先を落とす）。m に FFT の大きさを返す
void GrainMaker::computeEnvelope (const float* w, int n, int lifter, int& m)
{
    m = std::max (Fft::nextPow2 (n), kMinEnvelopeFft);
    std::fill (spec.begin(), spec.begin() + m, Fft::Complex {});
    for (int i = 0; i < n; ++i)
        spec[static_cast<size_t> (i)] = w[i];
    fft.transform (spec.data(), m, false);

    float peak = 0.0f;
    for (int b = 0; b <= m / 2; ++b)
        peak = std::max (peak, std::abs (spec[static_cast<size_t> (b)]));
    const float floor = std::max (peak * 1.0e-5f, 1.0e-12f); // 100dB 下で止める
    for (int b = 0; b < m; ++b)
        spec[static_cast<size_t> (b)] = std::log (std::max (std::abs (spec[static_cast<size_t> (b)]), floor));

    fft.transform (spec.data(), m, true); // ケプストラム（1/m は後でまとめて掛ける）
    const int q = std::clamp (lifter, 2, m / 2 - 1);
    const int taper = std::max (1, q / 4);
    for (int i = 0; i < m; ++i)
    {
        const int k = i <= m / 2 ? i : m - i;
        float g = 0.0f;
        if (k <= q - taper)
            g = 1.0f;
        else if (k < q)
            g = 0.5f * (1.0f + std::cos (static_cast<float> (kPi) * static_cast<float> (k - (q - taper)) / static_cast<float> (taper)));
        spec[static_cast<size_t> (i)] *= g / static_cast<float> (m);
    }
    fft.transform (spec.data(), m, false);
    for (int b = 0; b <= m / 2; ++b)
        envelope[static_cast<size_t> (b)] = spec[static_cast<size_t> (b)].real();
}

void GrainMaker::make (const float* seg, int centre, float halfIn, int halfOut, float ratio,
                       const WarpMap* warp, int lifter, float* out, int64_t key)
{
    // 窓をかけた元の断片（補間の幅の分だけ、両側を 0 で埋める）
    const int pad = static_cast<int> (std::ceil (kZeroCrossings * std::max (1.0f, ratio))) + 2;
    const int hi = static_cast<int> (std::ceil (halfIn));
    const int n = 2 * hi + 1;
    float* w = windowed.data();
    std::fill (w, w + n + 2 * pad, 0.0f);
    for (int j = -hi; j <= hi; ++j)
    {
        const float u = static_cast<float> (j) / halfIn;
        if (std::abs (u) < 1.0f)
            w[pad + hi + j] = seg[centre + j] * 0.5f * (1.0f + std::cos (static_cast<float> (kPi) * u));
    }

    // 伸縮（縮めるときは、帯域を 1/ratio に絞った sinc で補間する）
    const float scale = std::max (1.0f, ratio);
    const float gain = 1.0f / scale;
    const int width = static_cast<int> (std::ceil (kZeroCrossings * scale));
    const int no = 2 * halfOut + 1;
    for (int k = -halfOut; k <= halfOut; ++k)
    {
        const float u = static_cast<float> (k) * ratio;
        const int base = static_cast<int> (std::floor (u));
        float acc = 0.0f;
        for (int j = base - width + 1; j <= base + width; ++j)
        {
            const int idx = pad + hi + j;
            if (idx < 0 || idx >= n + 2 * pad)
                continue;
            acc += w[idx] * kernelAt ((u - static_cast<float> (j)) / scale);
        }
        out[k + halfOut] = acc * gain;
    }

    if (warp == nullptr)
        return;

    // 補正: 狙いの包絡（写像で動かしたもの）と、一律に伸縮した包絡の差をかける
    if (! envValid || key != envKey || n != envN || lifter != envLifter)
    {
        computeEnvelope (w + pad, n, lifter, envM);
        envValid = true;
        envKey = key;
        envN = n;
        envLifter = lifter;
    }
    const int m = envM;
    const int lpad = std::max (lifter, 1);
    const int mo = Fft::nextPow2 (no + 2 * lpad);
    std::fill (spec2.begin(), spec2.begin() + mo, Fft::Complex {});
    for (int i = 0; i < no; ++i)
        spec2[static_cast<size_t> (lpad + i)] = out[i];
    fft.transform (spec2.data(), mo, false);

    const float nyq = static_cast<float> (fs * 0.5);
    auto envAt = [&] (float f)
    {
        const float pos = std::clamp (f, 0.0f, nyq) * static_cast<float> (m) / static_cast<float> (fs);
        const int i = std::min (static_cast<int> (pos), m / 2 - 1);
        const float t = pos - static_cast<float> (i);
        return envelope[static_cast<size_t> (i)] + t * (envelope[static_cast<size_t> (i + 1)] - envelope[static_cast<size_t> (i)]);
    };
    if (warp != inverseWarp || warp->version() != inverseVersion || mo != inverseSize)
    {
        for (int b = 0; b <= mo / 2; ++b)
            inverseBins[static_cast<size_t> (b)] = warp->inverse (static_cast<float> (b) * static_cast<float> (fs) / static_cast<float> (mo));
        inverseWarp = warp;
        inverseVersion = warp->version();
        inverseSize = mo;
    }
    for (int b = 0; b <= mo / 2; ++b)
    {
        const float f = static_cast<float> (b) * static_cast<float> (fs) / static_cast<float> (mo);
        const float c = std::clamp (envAt (inverseBins[static_cast<size_t> (b)]) - envAt (f / ratio), -kMaxCorrection, kMaxCorrection);
        const float g = std::exp (c);
        spec2[static_cast<size_t> (b)] *= g;
        if (b > 0 && b < mo / 2)
            spec2[static_cast<size_t> (mo - b)] *= g;
    }
    fft.transform (spec2.data(), mo, true);
    // 断片の外へ出る裾は捨てる（遅延を増やさないため）
    for (int i = 0; i < no; ++i)
        out[i] = spec2[static_cast<size_t> (lpad + i)].real() / static_cast<float> (mo);
}

} // namespace vcab
