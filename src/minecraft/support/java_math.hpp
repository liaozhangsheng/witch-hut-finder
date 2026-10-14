#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

// Java math whose results feed integer coordinates. java_sin is fdlibm, bit for bit StrictMath.sin; it fills Mth's
// sine table, which vanilla builds with Math.sin. HotSpot may intrinsify Math.sin: on an AArch64 JVM about 7%
// of inputs differ from StrictMath by one ulp. The integer results built from them have agreed wherever compared,
// but a position at an exact rounding boundary can differ between JVMs, as it can in vanilla itself.
namespace minecraft::support {
namespace fdlibm {

inline std::int32_t high_word(double x) {
    return static_cast<std::int32_t>(std::bit_cast<std::uint64_t>(x) >> 32U);
}

inline double with_high_word(std::int32_t high) {
    return std::bit_cast<double>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32U);
}

inline double kernel_sin(double x, double y, int iy) {
    constexpr double half = 5.00000000000000000000e-01, s1 = -1.66666666666666324348e-01,
                     s2 = 8.33333333332248946124e-03, s3 = -1.98412698298579493134e-04,
                     s4 = 2.75573137070700676789e-06, s5 = -2.50507602534068634195e-08,
                     s6 = 1.58969099521155010221e-10;
    const auto ix = high_word(x) & 0x7fffffff;
    if (ix < 0x3e400000 && static_cast<int>(x) == 0)
        return x;
    const double z = x * x, v = z * x, r = s2 + z * (s3 + z * (s4 + z * (s5 + z * s6)));
    if (iy == 0)
        return x + v * (s1 + z * r);
    return x - ((z * (half * y - v * r) - y) - v * s1);
}

inline double kernel_cos(double x, double y) {
    constexpr double one = 1.0, c1 = 4.16666666666666019037e-02, c2 = -1.38888888888741095749e-03,
                     c3 = 2.48015872894767294178e-05, c4 = -2.75573143513906633035e-07,
                     c5 = 2.08757232129817482790e-09, c6 = -1.13596475577881948265e-11;
    const auto ix = high_word(x) & 0x7fffffff;
    if (ix < 0x3e400000 && static_cast<int>(x) == 0)
        return one;
    const double z = x * x, r = z * (c1 + z * (c2 + z * (c3 + z * (c4 + z * (c5 + z * c6)))));
    if (ix < 0x3FD33333)
        return one - (0.5 * z - (z * r - x * y));
    const double qx = ix > 0x3fe90000 ? 0.28125 : with_high_word(ix - 0x00200000);
    const double hz = 0.5 * z - qx, a = one - qx;
    return a - (hz - (z * r - x * y));
}

// Arguments up to 2^19 * pi/2. Larger ones never reach world generation and fall back to the platform library.
inline bool rem_pio2(double x, std::array<double, 2>& y, int& n) {
    constexpr std::array<std::int32_t, 32> npio2_hw{
        0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB, 0x402921FB,
        0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB,
        0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
        0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB};
    constexpr double half = 5.00000000000000000000e-01, invpio2 = 6.36619772367581382433e-01,
                     pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
                     pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
                     pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;
    const auto hx = high_word(x);
    const auto ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        y = {x, 0.0};
        n = 0;
        return true;
    }
    if (ix < 0x4002d97c) {
        if (hx > 0) {
            double z = x - pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            n = 1;
        } else {
            double z = x + pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z + pio2_1t;
                y[1] = (z - y[0]) + pio2_1t;
            } else {
                z += pio2_2;
                y[0] = z + pio2_2t;
                y[1] = (z - y[0]) + pio2_2t;
            }
            n = -1;
        }
        return true;
    }
    if (ix > 0x413921fb)
        return false;
    double t = std::fabs(x);
    const int quadrant = static_cast<int>(t * invpio2 + half);
    const double fn = quadrant;
    double r = t - fn * pio2_1, w = fn * pio2_1t;
    if (quadrant < 32 && ix != npio2_hw[static_cast<std::size_t>(quadrant - 1)]) {
        y[0] = r - w;
    } else {
        const int j = ix >> 20;
        y[0] = r - w;
        int i = j - ((high_word(y[0]) >> 20) & 0x7ff);
        if (i > 16) {
            t = r;
            w = fn * pio2_2;
            r = t - w;
            w = fn * pio2_2t - ((t - r) - w);
            y[0] = r - w;
            i = j - ((high_word(y[0]) >> 20) & 0x7ff);
            if (i > 49) {
                t = r;
                w = fn * pio2_3;
                r = t - w;
                w = fn * pio2_3t - ((t - r) - w);
                y[0] = r - w;
            }
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
        y = {-y[0], -y[1]};
        n = -quadrant;
    } else {
        n = quadrant;
    }
    return true;
}

}  // namespace fdlibm

inline double java_sin(double x) {
    const auto ix = fdlibm::high_word(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb)
        return fdlibm::kernel_sin(x, 0.0, 0);
    if (ix >= 0x7ff00000)
        return x - x;
    std::array<double, 2> y{};
    int n = 0;
    if (!fdlibm::rem_pio2(x, y, n))
        return std::sin(x);
    switch (n & 3) {
        case 0: return fdlibm::kernel_sin(y[0], y[1], 1);
        case 1: return fdlibm::kernel_cos(y[0], y[1]);
        case 2: return -fdlibm::kernel_sin(y[0], y[1], 1);
        default: return -fdlibm::kernel_cos(y[0], y[1]);
    }
}

// Java's (int) and (long) casts from double: NaN maps to zero and out-of-range values saturate.
inline std::int32_t java_d2i(double value) {
    if (std::isnan(value))
        return 0;
    if (value >= 2147483647.0)
        return 2147483647;
    if (value <= -2147483648.0)
        return -2147483647 - 1;
    return static_cast<std::int32_t>(value);
}

inline std::int64_t java_d2l(double value) {
    if (std::isnan(value))
        return 0;
    if (value >= 0x1p63)
        return INT64_MAX;
    if (value <= -0x1p63)
        return INT64_MIN;
    return static_cast<std::int64_t>(value);
}

inline std::int32_t java_floor(double value) {
    return java_d2i(std::floor(value));
}

// Mth.sin and Mth.cos read a 65536-entry float table.
class MthTrig {
public:
    static float sin(double value) {
        return table()[static_cast<std::size_t>(java_d2l(value * 10430.378350470453) & 65535)];
    }
    static float cos(double value) {
        return table()[static_cast<std::size_t>(java_d2l(value * 10430.378350470453 + 16384.0) & 65535)];
    }

private:
    static const std::array<float, 65536>& table() {
        static const auto values = [] {
            std::array<float, 65536> result{};
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] = static_cast<float>(java_sin(static_cast<double>(i) / 10430.378350470453));
            return result;
        }();
        return values;
    }
};

}  // namespace minecraft::support
