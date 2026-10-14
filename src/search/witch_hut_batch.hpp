#pragma once

#include "minecraft/worldgen/noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define WITCH_HUT_BATCH_NEON 1
#endif

namespace witch_hut {

// Estimates a NormalNoise at sixteen positions at a time: permutation hashes go through NEON table lookups and the
// lattice arithmetic runs in 16-bit fixed point. Each estimate stays within max_error of the exact value, per unit
// of factor, so it can screen ahead of exact evaluation.
class NoiseBatch {
public:
    static constexpr std::size_t lanes = 16;
    // The fixed-point rounding stays below 0.001 per unit of noise, well inside this margin.
    static constexpr double max_error = 0.005;

    // estimate() approximates noise.sample(x, y, z) * factor.
    NoiseBatch(const minecraft::worldgen::NormalNoise& noise, double factor) : noise_(&noise), factor_(factor) {
        const auto add = [&](const minecraft::worldgen::PerlinNoise& perlin, double input) {
            for (const auto& term : perlin.terms()) {
                Octave octave;
                octave.input = input * term.input_factor;
                octave.origin = term.noise->origin();
                // With y fixed at 0 its lattice cell and fraction are constants.
                const double cell_y = std::floor(octave.origin[1]);
                octave.cell_y = static_cast<std::uint8_t>(static_cast<std::int64_t>(cell_y) & 255);
                octave.local_y = to_fixed(octave.origin[1] - cell_y);
                octave.fade_y = to_weight(fade(octave.origin[1] - cell_y));
                octave.permutation = term.noise->permutation();
                octave.weight = static_cast<float>(term.weight * noise.value_factor() * factor);
                octaves_.push_back(octave);
            }
        };
        add(noise.first(), 1.0);
        add(noise.second(), minecraft::worldgen::NormalNoise::second_input_factor);
    }

    // out[i] estimates the noise at (x[i], y ? y[i] : 0, z[i]) for i < count, a multiple of lanes. The positions
    // must stay within a few million so that a float carries the result.
    void estimate(const double* x, const double* y, const double* z, std::size_t count, float* out) const {
        for (std::size_t first = 0; first < count; first += lanes)
            estimate16(x + first, y ? y + first : nullptr, z + first, out + first);
    }

private:
    static constexpr int fraction_bits = 12;  // lattice values in [-8, 8)
    struct Octave {
        double input;
        std::array<double, 3> origin;
        std::uint8_t cell_y;
        std::int16_t local_y, fade_y;
        float weight;
        std::array<std::uint8_t, 256> permutation;
    };
    const minecraft::worldgen::NormalNoise* noise_;
    double factor_;
    std::vector<Octave> octaves_;

    static double fade(double t) {
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }
    static std::int16_t to_fixed(double value) {
        return static_cast<std::int16_t>(std::lround(value * (1 << fraction_bits)));
    }
    // Lerp weights in Q15; 1 itself is never needed since fractions stay below 1.
    static std::int16_t to_weight(double value) {
        return static_cast<std::int16_t>(std::min(32767L, std::lround(value * 32768.0)));
    }

#ifdef WITCH_HUT_BATCH_NEON
    // One axis of sixteen lanes: lattice cells (low byte), fractions in fixed point and their fade weights in Q15.
    struct Axis {
        uint8x16_t cell;
        std::array<int16x8_t, 2> local, weight;
    };

    static Axis prepare(const double* in, double input, double origin) {
        // Cells and fractions need double precision; the fraction's fade and fixed-point forms do not.
        const float64x2_t scale = vdupq_n_f64(input), offset = vdupq_n_f64(origin);
        std::array<int64x2_t, 8> cells;
        std::array<float32x4_t, 4> fractions;
        for (std::size_t pair = 0; pair < 8; ++pair) {
            const float64x2_t position = vaddq_f64(vmulq_f64(vld1q_f64(in + pair * 2), scale), offset);
            const float64x2_t floor = vrndmq_f64(position);
            cells[pair] = vcvtq_s64_f64(floor);
            const float32x2_t t = vcvt_f32_f64(vsubq_f64(position, floor));
            fractions[pair / 2] = pair % 2 ? vcombine_f32(vget_low_f32(fractions[pair / 2]), t)
                                           : vcombine_f32(t, t);
        }
        std::array<int32x4_t, 4> locals, weights;
        for (std::size_t q = 0; q < 4; ++q) {
            const float32x4_t t = fractions[q];
            const float32x4_t faded = vmulq_f32(
                vmulq_f32(vmulq_f32(t, t), t),
                vaddq_f32(vmulq_f32(t, vsubq_f32(vmulq_f32(t, vdupq_n_f32(6.0F)), vdupq_n_f32(15.0F))),
                          vdupq_n_f32(10.0F)));
            locals[q] = vcvtnq_s32_f32(vmulq_f32(t, vdupq_n_f32(1 << fraction_bits)));
            weights[q] = vcvtnq_s32_f32(vminq_f32(vmulq_f32(faded, vdupq_n_f32(32768.0F)), vdupq_n_f32(32767.0F)));
        }
        const auto quad = [&](std::size_t a) {
            return vcombine_s32(vmovn_s64(cells[a]), vmovn_s64(cells[a + 1]));
        };
        const auto narrow = [](int32x4_t low, int32x4_t high) {
            return vcombine_s16(vmovn_s32(low), vmovn_s32(high));
        };
        Axis axis;
        axis.cell = vcombine_u8(vmovn_u16(vreinterpretq_u16_s16(narrow(quad(0), quad(2)))),
                                vmovn_u16(vreinterpretq_u16_s16(narrow(quad(4), quad(6)))));
        axis.local = {narrow(locals[0], locals[1]), narrow(locals[2], locals[3])};
        axis.weight = {narrow(weights[0], weights[1]), narrow(weights[2], weights[3])};
        return axis;
    }

    void estimate16(const double* xs, const double* ys, const double* zs, float* out) const {
        float32x4_t sum[4] = {vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0)};
        static constexpr std::array<std::int8_t, 16> gx{1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0, 1, 0, -1, 0};
        static constexpr std::array<std::int8_t, 16> gy{1, 1, -1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1};
        static constexpr std::array<std::int8_t, 16> gz{0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 1, 0, -1};
        const int8x16_t grad_x = vld1q_s8(gx.data()), grad_y = vld1q_s8(gy.data()), grad_z = vld1q_s8(gz.data());
        const uint8x16_t low_nibble = vdupq_n_u8(15), one = vdupq_n_u8(1);
        const int16x8_t unit = vdupq_n_s16(1 << fraction_bits);
        for (const auto& octave : octaves_) {
            const auto ax = prepare(xs, octave.input, octave.origin[0]);
            const auto az = prepare(zs, octave.input, octave.origin[2]);
            Axis ay;
            if (ys) {
                ay = prepare(ys, octave.input, octave.origin[1]);
            } else {
                ay.cell = vdupq_n_u8(octave.cell_y);
                ay.local = {vdupq_n_s16(octave.local_y), vdupq_n_s16(octave.local_y)};
                ay.weight = {vdupq_n_s16(octave.fade_y), vdupq_n_s16(octave.fade_y)};
            }
            const auto* p = octave.permutation.data();
            const uint8x16x4_t t0 = vld1q_u8_x4(p), t1 = vld1q_u8_x4(p + 64), t2 = vld1q_u8_x4(p + 128),
                               t3 = vld1q_u8_x4(p + 192);
            // permutation[index & 255] per lane: byte arithmetic wraps exactly as the scalar & 255 does.
            const auto lookup = [&](uint8x16_t index) {
                auto r = vqtbl4q_u8(t0, index);
                r = vqtbx4q_u8(r, t1, vsubq_u8(index, vdupq_n_u8(64)));
                r = vqtbx4q_u8(r, t2, vsubq_u8(index, vdupq_n_u8(128)));
                return vqtbx4q_u8(r, t3, vsubq_u8(index, vdupq_n_u8(192)));
            };
            const uint8x16_t cx = ax.cell, cy = ay.cell, cz = az.cell;
            const uint8x16_t cz1 = vaddq_u8(cz, one);
            const auto i = lookup(cx), j = lookup(vaddq_u8(cx, one));
            const auto k = lookup(vaddq_u8(i, cy)), l = lookup(vaddq_u8(vaddq_u8(i, cy), one));
            const auto i1 = lookup(vaddq_u8(j, cy)), j1 = lookup(vaddq_u8(vaddq_u8(j, cy), one));
            const std::array<uint8x16_t, 8> hash{
                vandq_u8(lookup(vaddq_u8(k, cz)), low_nibble),  vandq_u8(lookup(vaddq_u8(i1, cz)), low_nibble),
                vandq_u8(lookup(vaddq_u8(l, cz)), low_nibble),  vandq_u8(lookup(vaddq_u8(j1, cz)), low_nibble),
                vandq_u8(lookup(vaddq_u8(k, cz1)), low_nibble), vandq_u8(lookup(vaddq_u8(i1, cz1)), low_nibble),
                vandq_u8(lookup(vaddq_u8(l, cz1)), low_nibble), vandq_u8(lookup(vaddq_u8(j1, cz1)), low_nibble)};
            std::array<int8x16_t, 8> hx, hy, hz;
            for (std::size_t c = 0; c < 8; ++c) {
                hx[c] = vqtbl1q_s8(grad_x, hash[c]);
                hy[c] = vqtbl1q_s8(grad_y, hash[c]);
                hz[c] = vqtbl1q_s8(grad_z, hash[c]);
            }
            const float32x4_t weight = vdupq_n_f32(octave.weight);
            for (std::size_t half = 0; half < 2; ++half) {
                const auto widen = [half](int8x16_t v) {
                    return half ? vmovl_high_s8(v) : vmovl_s8(vget_low_s8(v));
                };
                const int16x8_t lx0 = ax.local[half], lx1 = vsubq_s16(lx0, unit);
                const int16x8_t ly0 = ay.local[half], ly1 = vsubq_s16(ly0, unit);
                const int16x8_t lz0 = az.local[half], lz1 = vsubq_s16(lz0, unit);
                const int16x8_t wx = ax.weight[half], wy = ay.weight[half], wz = az.weight[half];
                const auto dot = [&](std::size_t c, int16x8_t x, int16x8_t y, int16x8_t z) {
                    auto d = vmulq_s16(widen(hx[c]), x);
                    d = vmlaq_s16(d, widen(hy[c]), y);
                    return vmlaq_s16(d, widen(hz[c]), z);
                };
                const auto lerp = [](int16x8_t t, int16x8_t a, int16x8_t b) {
                    return vaddq_s16(a, vqrdmulhq_s16(vsubq_s16(b, a), t));
                };
                const auto e0 = lerp(wx, dot(0, lx0, ly0, lz0), dot(1, lx1, ly0, lz0));
                const auto e1 = lerp(wx, dot(2, lx0, ly1, lz0), dot(3, lx1, ly1, lz0));
                const auto e2 = lerp(wx, dot(4, lx0, ly0, lz1), dot(5, lx1, ly0, lz1));
                const auto e3 = lerp(wx, dot(6, lx0, ly1, lz1), dot(7, lx1, ly1, lz1));
                const auto value = lerp(wz, lerp(wy, e0, e1), lerp(wy, e2, e3));
                const auto low = vcvtq_n_f32_s32(vmovl_s16(vget_low_s16(value)), fraction_bits);
                const auto high = vcvtq_n_f32_s32(vmovl_high_s16(value), fraction_bits);
                sum[half * 2] = vmlaq_f32(sum[half * 2], low, weight);
                sum[half * 2 + 1] = vmlaq_f32(sum[half * 2 + 1], high, weight);
            }
        }
        for (std::size_t q = 0; q < 4; ++q)
            vst1q_f32(out + q * 4, sum[q]);
    }
#else
    void estimate16(const double* xs, const double* ys, const double* zs, float* out) const {
        for (std::size_t i = 0; i < lanes; ++i)
            out[i] = static_cast<float>(noise_->sample(xs[i], ys ? ys[i] : 0.0, zs[i]) * factor_);
    }
#endif
};

}  // namespace witch_hut
