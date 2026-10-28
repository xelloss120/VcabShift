// 2 のべき乗の大きさの複素 FFT（基数 2、その場で計算）。
// 回転因子とビット反転の表は、使う一番大きい大きさの分を prepare で作り、小さい大きさはそれを間引いて使う。
#pragma once

#include <cmath>
#include <complex>
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
            const double a = -2.0 * 3.14159265358979323846 * k / maxN;
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

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int step = maxN / len;
            for (int i = 0; i < n; i += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    Complex w = twiddle[static_cast<size_t> (k * step)];
                    if (inverse)
                        w = std::conj (w);
                    const Complex u = x[i + k];
                    const Complex v = x[i + k + half] * w;
                    x[i + k] = u + v;
                    x[i + k + half] = u - v;
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
