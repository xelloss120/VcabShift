// 2 のべき乗の大きさの複素 FFT（基数 2、その場で計算）。
// 回転因子の表は、使う一番大きい大きさの分を prepare で作り、小さい大きさはそれを間引いて使う。
#pragma once

#include "MathConstants.h"

#include <cmath>
#include <complex>
#include <utility>
#include <vector>

namespace vcab
{

class Fft
{
public:
    using Complex = std::complex<float>;

    // maxSize: 使う一番大きい大きさ（2 のべき乗）
    void prepare (int maxSize)
    {
        maxN = maxSize;
        twiddle.resize (static_cast<size_t> (maxN / 2));
        for (int k = 0; k < maxN / 2; ++k)
        {
            const double a = -2.0 * kPi * k / maxN;
            twiddle[static_cast<size_t> (k)] = Complex (static_cast<float> (std::cos (a)), static_cast<float> (std::sin (a)));
        }
    }

    int maxSize() const { return maxN; }

    // n 点の FFT（inverse で逆変換。逆変換は 1/n を掛けない）
    void transform (Complex* x, int n, bool inverse) const
    {
        // ビット反転の並べ替え
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j |= bit;
            if (i < j)
                std::swap (x[i], x[j]);
        }

        // 回転因子の虚部の符号（逆変換は共役）。複素数の掛け算は実部と虚部に分けて書く（std::complex と同じ計算の順）
        const float sign = inverse ? -1.0f : 1.0f;
        float* xf = reinterpret_cast<float*> (x);
        const float* tw = reinterpret_cast<const float*> (twiddle.data());
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int step = maxN / len;
            for (int i = 0; i < n; i += len)
            {
                float* a = xf + 2 * i;
                float* b = xf + 2 * (i + half);
                for (int k = 0; k < half; ++k)
                {
                    const float wr = tw[2 * k * step], wi = sign * tw[2 * k * step + 1];
                    const float br = b[2 * k], bi = b[2 * k + 1];
                    const float vr = br * wr - bi * wi, vi = br * wi + bi * wr;
                    const float ur = a[2 * k], ui = a[2 * k + 1];
                    a[2 * k] = ur + vr;
                    a[2 * k + 1] = ui + vi;
                    b[2 * k] = ur - vr;
                    b[2 * k + 1] = ui - vi;
                }
            }
        }
    }

    static int nextPow2 (int n)
    {
        int p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

private:
    int maxN = 0;
    std::vector<Complex> twiddle;
};

} // namespace vcab
