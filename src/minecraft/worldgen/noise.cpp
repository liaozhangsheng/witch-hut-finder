#include "noise.hpp"

#include <minecraft/random.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// Octave loops call the ImprovedNoise sample path per octave; inlining it lets the core overlap octaves.
#if defined(__GNUC__) || defined(__clang__)
#define MINECRAFT_INLINE [[gnu::always_inline]]
#else
#define MINECRAFT_INLINE
#endif

namespace minecraft::worldgen {
namespace {

std::int32_t java_floor_int(const double value) noexcept {
    // NaN fails the range test, so only finite in-range values take the fast path.
    if (value > -0x1p30 && value < 0x1p30) {
        return static_cast<std::int32_t>(std::floor(value));
    }
    const auto floored = std::floor(value);
    if (std::isnan(floored)) {
        return 0;
    }
    if (floored >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (floored <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(floored);
}

std::int64_t java_floor_long(const double value) noexcept {
    const auto floored = std::floor(value);
    if (std::isnan(floored)) {
        return 0;
    }
    if (floored >= 0x1.0p63) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (floored <= -0x1.0p63) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(floored);
}

std::int32_t java_add_int(const std::int32_t left, const std::int32_t right) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(left) + static_cast<std::uint32_t>(right));
}

double smoothstep(const double value) noexcept {
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

double lerp(const double alpha, const double from, const double to) noexcept {
    return from + alpha * (to - from);
}

double lerp2(const double alpha1, const double alpha2, const double v00, const double v10, const double v01,
             const double v11) noexcept {
    return lerp(alpha2, lerp(alpha1, v00, v10), lerp(alpha1, v01, v11));
}

[[maybe_unused]] double lerp3(const double alpha1, const double alpha2, const double alpha3, const double v000,
                               const double v100, const double v010, const double v110, const double v001,
                               const double v101, const double v011, const double v111) noexcept {
    return lerp(alpha3, lerp2(alpha1, alpha2, v000, v100, v010, v110),
                lerp2(alpha1, alpha2, v001, v101, v011, v111));
}

double clamped_lerp(const double alpha, const double from, const double to) noexcept {
    if (alpha < 0.0) {
        return from;
    }
    if (alpha > 1.0) {
        return to;
    }
    return lerp(alpha, from, to);
}

// Stored as doubles: converting Java's int gradients is exact, so the products are unchanged.
constexpr std::array<std::array<double, 3>, 16> gradients{{
    {{1, 1, 0}},  {{-1, 1, 0}}, {{1, -1, 0}}, {{-1, -1, 0}}, {{1, 0, 1}},  {{-1, 0, 1}},
    {{1, 0, -1}}, {{-1, 0, -1}}, {{0, 1, 1}}, {{0, -1, 1}}, {{0, 1, -1}}, {{0, -1, -1}},
    {{1, 1, 0}},  {{0, -1, 1}}, {{-1, 1, 0}}, {{0, -1, -1}},
}};

double gradient_dot(const std::uint8_t hash, const double x, const double y, const double z) noexcept {
    const auto& gradient = gradients[hash & 15U];
    return gradient[0] * x + gradient[1] * y + gradient[2] * z;
}

double corner_noise(const std::uint8_t index, const double x, const double y, const double z,
                    const double base) noexcept {
    auto attenuation = base - x * x - y * y - z * z;
    if (attenuation < 0.0) {
        return 0.0;
    }
    attenuation *= attenuation;
    return attenuation * attenuation * gradient_dot(index, x, y, z);
}

NoiseParameters all_octaves(const int first_octave, const std::size_t count) {
    return NoiseParameters{first_octave, std::vector<double>(count, 1.0)};
}

}  // namespace

ImprovedNoise::ImprovedNoise(random::RandomSource& rng) {
    xo_ = rng.next_double() * 256.0;
    yo_ = rng.next_double() * 256.0;
    zo_ = rng.next_double() * 256.0;
    for (std::size_t i = 0; i < permutation_.size(); ++i) {
        permutation_[i] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t k = 0; k < permutation_.size(); ++k) {
        const auto j = static_cast<std::size_t>(rng.next_int(static_cast<std::int32_t>(256U - k)));
        std::swap(permutation_[k], permutation_[k + j]);
    }
}

MINECRAFT_INLINE double ImprovedNoise::sample(const double x, const double y, const double z) const {
    const auto shifted_x = x + xo_;
    const auto shifted_y = y + yo_;
    const auto shifted_z = z + zo_;
    const auto cell_x = java_floor_int(shifted_x);
    const auto cell_y = java_floor_int(shifted_y);
    const auto cell_z = java_floor_int(shifted_z);
    const auto local_y = shifted_y - cell_y;
    // With no vertical fudge vanilla subtracts a zero shift, which leaves local_y unchanged.
    return sample_cell(cell_x, cell_y, cell_z, shifted_x - cell_x, local_y, shifted_z - cell_z, local_y);
}

MINECRAFT_INLINE double ImprovedNoise::sample(const double x, const double y, const double z, const double y_scale,
                                            const double y_max) const {
    if (y_scale == 0.0) {
        return sample(x, y, z);
    }
    const auto shifted_x = x + xo_;
    const auto shifted_y = y + yo_;
    const auto shifted_z = z + zo_;
    const auto cell_x = java_floor_int(shifted_x);
    const auto cell_y = java_floor_int(shifted_y);
    const auto cell_z = java_floor_int(shifted_z);
    const auto local_y = shifted_y - cell_y;
    const auto fudge = y_max >= 0.0 && y_max < local_y ? y_max : local_y;
    const auto vertical_shift = java_floor_int(fudge / y_scale + static_cast<double>(1.0e-7F)) * y_scale;
    return sample_cell(cell_x, cell_y, cell_z, shifted_x - cell_x, local_y, shifted_z - cell_z,
                       local_y - vertical_shift);
}

#if defined(__GNUC__) || defined(__clang__)
namespace {

using double2 = double __attribute__((vector_size(16)));

// The gradients of two corners side by side: x, y and z components, each a lane pair. Indexed by
// (first hash & 15) * 16 + (second hash & 15).
struct GradientPairs {
    alignas(16) std::array<std::array<double, 6>, 256> rows{};
    constexpr GradientPairs() {
        for (std::size_t a = 0; a < 16; ++a)
            for (std::size_t b = 0; b < 16; ++b)
                rows[a * 16 + b] = {gradients[a][0], gradients[b][0], gradients[a][1],
                                    gradients[b][1], gradients[a][2], gradients[b][2]};
    }
};
constexpr GradientPairs gradient_pairs;

[[gnu::always_inline]] inline double2 load2(const double* values) noexcept {
    double2 result;
    __builtin_memcpy(&result, values, sizeof(result));
    return result;
}

}  // namespace
#endif

MINECRAFT_INLINE double ImprovedNoise::sample_cell(const std::int32_t cell_x, const std::int32_t cell_y,
                                                 const std::int32_t cell_z, const double local_x, const double local_y,
                                                 const double local_z, const double gradient_y) const {
    // Java indexes with the low byte of an int sum; unsigned wrap-around keeps the same low byte.
    const auto p = [this](const std::uint32_t index) { return permutation_[index & 255U]; };
    const auto ux = static_cast<std::uint32_t>(cell_x);
    const auto uy = static_cast<std::uint32_t>(cell_y);
    const auto uz = static_cast<std::uint32_t>(cell_z);
    const auto i = p(ux);
    const auto j = p(ux + 1U);
    const auto k = p(i + uy);
    const auto l = p(i + uy + 1U);
    const auto i1 = p(j + uy);
    const auto j1 = p(j + uy + 1U);
#if defined(__GNUC__) || defined(__clang__)
    // The scalar formula below, with the corners paired along x: every lane performs the same operations in the same
    // order, and the library disables floating-point contraction, so each value rounds exactly as the scalar code.
    const auto pair = [&](const std::uint32_t low, const std::uint32_t high) {
        return gradient_pairs.rows[static_cast<std::size_t>(p(low) & 15U) * 16 + (p(high) & 15U)].data();
    };
    const auto* g01 = pair(k + uz, i1 + uz);
    const auto* g23 = pair(l + uz, j1 + uz);
    const auto* g45 = pair(k + uz + 1U, i1 + uz + 1U);
    const auto* g67 = pair(l + uz + 1U, j1 + uz + 1U);
    const double2 dx{local_x, local_x - 1.0};
    const double2 dy0{gradient_y, gradient_y}, dy1 = dy0 - 1.0;
    const double2 dz0{local_z, local_z}, dz1 = dz0 - 1.0;
    const auto dot = [&](const double* g, const double2 y, const double2 z) {
        return load2(g) * dx + load2(g + 2) * y + load2(g + 4) * z;
    };
    const auto d01 = dot(g01, dy0, dz0), d23 = dot(g23, dy1, dz0);
    const auto d45 = dot(g45, dy0, dz1), d67 = dot(g67, dy1, dz1);
    const double2 xy{local_x, local_y};
    const auto fade = xy * xy * xy * (xy * (xy * 6.0 - 15.0) + 10.0);
    const double2 alpha_x{fade[0], fade[0]}, alpha_y{fade[1], fade[1]};
    // lerp3: x lerps of (d0, d1), (d2, d3) | (d4, d5), (d6, d7), then y lerps, then the z lerp.
    const double2 from_a = __builtin_shufflevector(d01, d23, 0, 2), to_a = __builtin_shufflevector(d01, d23, 1, 3);
    const double2 from_b = __builtin_shufflevector(d45, d67, 0, 2), to_b = __builtin_shufflevector(d45, d67, 1, 3);
    const auto along_x_a = from_a + alpha_x * (to_a - from_a);
    const auto along_x_b = from_b + alpha_x * (to_b - from_b);
    const double2 from_y = __builtin_shufflevector(along_x_a, along_x_b, 0, 2);
    const double2 to_y = __builtin_shufflevector(along_x_a, along_x_b, 1, 3);
    const auto along_y = from_y + alpha_y * (to_y - from_y);
    return lerp(smoothstep(local_z), along_y[0], along_y[1]);
#else
    const auto d0 = gradient_dot(p(k + uz), local_x, gradient_y, local_z);
    const auto d1 = gradient_dot(p(i1 + uz), local_x - 1.0, gradient_y, local_z);
    const auto d2 = gradient_dot(p(l + uz), local_x, gradient_y - 1.0, local_z);
    const auto d3 = gradient_dot(p(j1 + uz), local_x - 1.0, gradient_y - 1.0, local_z);
    const auto d4 = gradient_dot(p(k + uz + 1U), local_x, gradient_y, local_z - 1.0);
    const auto d5 = gradient_dot(p(i1 + uz + 1U), local_x - 1.0, gradient_y, local_z - 1.0);
    const auto d6 = gradient_dot(p(l + uz + 1U), local_x, gradient_y - 1.0, local_z - 1.0);
    const auto d7 = gradient_dot(p(j1 + uz + 1U), local_x - 1.0, gradient_y - 1.0, local_z - 1.0);
    return lerp3(smoothstep(local_x), smoothstep(local_y), smoothstep(local_z), d0, d1, d2, d3, d4, d5, d6, d7);
#endif
}

#if defined(__GNUC__) || defined(__clang__)
#define MINECRAFT_PAIRED_OCTAVES 1

// Two independent ImprovedNoise samples, one per lane. sample_cell already fills the vector units within a sample,
// so the lanes are simply the scalar calls, inlined side by side for the out-of-order core to overlap.
struct PairKernel {
    // PerlinNoise::wrap per lane; both lanes usually take its identity range, tested at once.
    [[gnu::always_inline]] static double2 wrap(const double2 value) noexcept {
        const auto inside = (value > -0x1p23) & (value < 0x1p23);
        if (inside[0] && inside[1])
            return value;
        return double2{PerlinNoise::wrap(value[0]), PerlinNoise::wrap(value[1])};
    }

    // ImprovedNoise::sample(x, y, z, y_scale, y_max) per lane; a zero y_scale lane takes the unfudged path.
    [[gnu::always_inline]] static double2 sample(const ImprovedNoise& a, const ImprovedNoise& b, const double2 x,
                                                 const double2 y, const double2 z,
                                                 const double2 y_scale = double2{0.0, 0.0},
                                                 const double2 y_max = double2{0.0, 0.0}) {
        return double2{a.sample(x[0], y[0], z[0], y_scale[0], y_max[0]),
                       b.sample(x[1], y[1], z[1], y_scale[1], y_max[1])};
    }
};
#endif

PerlinNoise::PerlinNoise(random::RandomSource& rng, const NoiseParameters& parameters, const bool modern) {
    const auto& amplitudes = parameters.amplitudes;
    const auto first_octave = parameters.first_octave;
    const auto count = amplitudes.size();
    const auto first_index = -static_cast<std::int64_t>(first_octave);
    std::vector<std::optional<ImprovedNoise>> created(count);
    if (modern) {
        auto positional = rng.fork_positional();
        for (std::size_t i = 0; i < count; ++i) {
            if (amplitudes[i] != 0.0) {
                const auto octave = static_cast<std::int64_t>(first_octave) + static_cast<std::int64_t>(i);
                auto octave_rng = positional.from_hash_of("octave_" + std::to_string(octave));
                created[i].emplace(octave_rng);
            }
        }
    } else {
        auto initial = ImprovedNoise(rng);
        if (first_index >= 0 && static_cast<std::uint64_t>(first_index) < count &&
            amplitudes[static_cast<std::size_t>(first_index)] != 0.0) {
            created[static_cast<std::size_t>(first_index)].emplace(std::move(initial));
        }
        for (auto i = first_index - 1; i >= 0; --i) {
            if (static_cast<std::uint64_t>(i) < count && amplitudes[static_cast<std::size_t>(i)] != 0.0) {
                created[static_cast<std::size_t>(i)].emplace(rng);
            } else {
                rng.consume_count(262);
            }
        }

        const auto nonzero_amplitudes = static_cast<std::size_t>(
            std::count_if(amplitudes.begin(), amplitudes.end(), [](const double value) { return value != 0.0; }));
        const auto created_octaves = static_cast<std::size_t>(
            std::count_if(created.begin(), created.end(), [](const auto& octave) { return octave.has_value(); }));
        if (created_octaves != nonzero_amplitudes) {
            throw std::logic_error("failed to create the requested legacy octave levels");
        }
        if (first_index < static_cast<std::int64_t>(count) - 1) {
            throw std::invalid_argument("positive legacy octaves are not supported");
        }
    }
    const auto lowest_index = static_cast<double>(first_index);
    auto input_factor = std::pow(2.0, -lowest_index);
    auto value_factor =
        std::pow(2.0, static_cast<double>(count) - 1.0) / (std::pow(2.0, static_cast<double>(count)) - 1.0);
    slots_.assign(count, -1);
    for (std::size_t i = 0; i < count; ++i) {
        if (created[i]) {
            slots_[i] = static_cast<std::int32_t>(octaves_.size());
            octaves_.push_back({std::move(*created[i]), amplitudes[i], input_factor, value_factor});
        }
        input_factor *= 2.0;
        value_factor /= 2.0;
    }
}

double PerlinNoise::sample(const double x, const double y, const double z) const {
    return sum(x, y, z, octaves_.size());
}

MINECRAFT_INLINE double PerlinNoise::sum(const double x, const double y, const double z, const std::size_t end) const {
    double result = 0.0;
    std::size_t first = 0;
#ifdef MINECRAFT_PAIRED_OCTAVES
    // Octave samples are independent; only their sum is ordered, and it is still accumulated octave by octave.
    for (; first + 1 < end; first += 2) {
        const auto& a = octaves_[first];
        const auto& b = octaves_[first + 1];
        const double2 factor{a.input_factor, b.input_factor};
        const auto value = PairKernel::sample(a.noise, b.noise, PairKernel::wrap(x * factor),
                                              PairKernel::wrap(y * factor), PairKernel::wrap(z * factor));
        const double2 amplitude{a.amplitude, b.amplitude}, value_factor{a.value_factor, b.value_factor};
        const auto terms = amplitude * value * value_factor;
        result += terms[0];
        result += terms[1];
    }
#endif
    for (; first < end; ++first) {
        const auto& octave = octaves_[first];
        result += octave.amplitude *
                  octave.noise.sample(wrap(x * octave.input_factor), wrap(y * octave.input_factor),
                                      wrap(z * octave.input_factor)) *
                  octave.value_factor;
    }
    return result;
}

double PerlinNoise::sample(const double x, const double y, const double z, const double y_scale,
                           const double y_max) const {
    double result = 0.0;
    for (const auto& octave : octaves_) {
        result += octave.amplitude *
                  octave.noise.sample(wrap(x * octave.input_factor), wrap(y * octave.input_factor),
                                      wrap(z * octave.input_factor), y_scale * octave.input_factor,
                                      y_max * octave.input_factor) *
                  octave.value_factor;
    }
    return result;
}

const ImprovedNoise* PerlinNoise::octave(const std::size_t index) const {
    const auto slot = slots_[index];
    return slot < 0 ? nullptr : &octaves_[static_cast<std::size_t>(slot)].noise;
}

double PerlinNoise::wrap(const double value) noexcept {
    // Below 2^23 the rounded multiple is provably zero and vanilla returns value - 0.0, i.e. value itself.
    if (value > -0x1p23 && value < 0x1p23) {
        return value;
    }
    const auto multiple = java_floor_long(value / 3.3554432E7 + 0.5);
    return value - static_cast<double>(multiple) * 3.3554432E7;
}

NormalNoise::NormalNoise(random::RandomSource rng, const NoiseParameters& parameters)
    : first_(rng, parameters), second_(rng, parameters) {
    auto min_index = std::numeric_limits<std::int32_t>::max();
    auto max_index = std::numeric_limits<std::int32_t>::min();
    for (std::size_t i = 0; i < parameters.amplitudes.size(); ++i) {
        if (parameters.amplitudes[i] != 0.0) {
            min_index = std::min(min_index, static_cast<std::int32_t>(i));
            max_index = std::max(max_index, static_cast<std::int32_t>(i));
        }
    }
    const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(max_index) -
                                                static_cast<std::uint32_t>(min_index));
    value_factor_ = 0.16666666666666666 / (0.1 * (1.0 + 1.0 / (static_cast<double>(span) + 1.0)));
}

double NormalNoise::sample(const double x, const double y, const double z) const {
    const auto sx = x * second_input_factor, sy = y * second_input_factor, sz = z * second_input_factor;
#ifdef MINECRAFT_PAIRED_OCTAVES
    // Both halves share their parameters, so with an odd octave count each has one octave left over after pairing;
    // those two are paired with each other and added last to their own sums.
    const auto count = first_.octaves_.size();
    if (count % 2 == 1 && second_.octaves_.size() == count) {
        auto first = first_.sum(x, y, z, count - 1);
        auto second = second_.sum(sx, sy, sz, count - 1);
        const auto& a = first_.octaves_.back();
        const auto& b = second_.octaves_.back();
        const double2 factor{a.input_factor, b.input_factor};
        const auto value =
            PairKernel::sample(a.noise, b.noise, PairKernel::wrap(double2{x, sx} * factor),
                               PairKernel::wrap(double2{y, sy} * factor), PairKernel::wrap(double2{z, sz} * factor));
        first += a.amplitude * value[0] * a.value_factor;
        second += b.amplitude * value[1] * b.value_factor;
        return (first + second) * value_factor_;
    }
#endif
    return (first_.sample(x, y, z) + second_.sample(sx, sy, sz)) * value_factor_;
}

BlendedNoise::BlendedNoise(random::RandomSource& rng, const double xz_scale, const double y_scale,
                           const double xz_factor, const double y_factor, const double smear_scale)
    : xz_scale_(xz_scale), y_scale_(y_scale), xz_factor_(xz_factor), y_factor_(y_factor), smear_scale_(smear_scale),
      min_limit_(rng, all_octaves(-15, 16), false), max_limit_(rng, all_octaves(-15, 16), false),
      main_(rng, all_octaves(-7, 8), false) {
    // A legacy octave blends gradient dot products, each summing two unit components times offsets in [-1, 1]; the y
    // fudge moves the y offset past that range by at most 1e-7 of the octave's y scale, largest at frequency one.
    // sample() lerps between the two limit sums, which weigh octaves 2^0..2^15, and divides by 2^16, so it stays
    // below one octave's bound. The factor covers the rounding of the lerps and sums.
    const double y_scale_bound = 684.412 * y_scale * smear_scale;
    max_magnitude_ = (2.0 + 1.0e-7 * std::abs(y_scale_bound)) * 1.01;
}

double BlendedNoise::sample(const BlockPos position) const {
    const auto d0 = position.x * (684.412 * xz_scale_);
    const auto d1 = position.y * (684.412 * y_scale_);
    const auto d2 = position.z * (684.412 * xz_scale_);
    const auto d3 = d0 / xz_factor_;
    const auto d4 = d1 / y_factor_;
    const auto d5 = d2 / xz_factor_;
    const auto d6 = (684.412 * y_scale_) * smear_scale_;
    const auto d7 = d6 / y_factor_;
    double min_noise = 0.0;
    double max_noise = 0.0;
    double main_noise = 0.0;
    double frequency = 1.0;
    const auto main_sample = [&](const ImprovedNoise& octave, const double f) {
        return octave.sample(PerlinNoise::wrap(d3 * f), PerlinNoise::wrap(d4 * f), PerlinNoise::wrap(d5 * f), d7 * f,
                             d4 * f);
    };
    for (int i = 0; i < 8; ++i) {
        const auto index = main_.octave_count() - 1U - static_cast<std::size_t>(i);
        const auto* octave = main_.octave(index);
#ifdef MINECRAFT_PAIRED_OCTAVES
        // Pair this octave with the next one; the sum is still accumulated in octave order.
        const auto* next = i + 1 < 8 ? main_.octave(index - 1U) : nullptr;
        if (octave && next) {
            const auto f = frequency, g = frequency / 2.0;
            const auto value =
                PairKernel::sample(*octave, *next, {PerlinNoise::wrap(d3 * f), PerlinNoise::wrap(d3 * g)},
                                   {PerlinNoise::wrap(d4 * f), PerlinNoise::wrap(d4 * g)},
                                   {PerlinNoise::wrap(d5 * f), PerlinNoise::wrap(d5 * g)}, {d7 * f, d7 * g},
                                   {d4 * f, d4 * g});
            main_noise += value[0] / f;
            main_noise += value[1] / g;
            frequency = g / 2.0;
            ++i;
            continue;
        }
#endif
        if (octave) {
            main_noise += main_sample(*octave, frequency) / frequency;
        }
        frequency /= 2.0;
    }
    const auto blend = (main_noise / 10.0 + 1.0) / 2.0;
    const auto skip_min = blend >= 1.0;
    const auto skip_max = blend <= 0.0;
    frequency = 1.0;
    for (int i = 0; i < 16; ++i) {
        const auto wrapped_x = PerlinNoise::wrap(d0 * frequency);
        const auto wrapped_y = PerlinNoise::wrap(d1 * frequency);
        const auto wrapped_z = PerlinNoise::wrap(d2 * frequency);
        const auto y_scale = d6 * frequency;
        const auto y_max = d1 * frequency;
        const auto index = min_limit_.octave_count() - 1U - static_cast<std::size_t>(i);
        const auto* min_octave = skip_min ? nullptr : min_limit_.octave(index);
        const auto* max_octave = skip_max ? nullptr : max_limit_.octave(index);
#ifdef MINECRAFT_PAIRED_OCTAVES
        // Both limits read the same coordinates at a given octave.
        if (min_octave && max_octave) {
            const auto value = PairKernel::sample(*min_octave, *max_octave, {wrapped_x, wrapped_x},
                                                  {wrapped_y, wrapped_y}, {wrapped_z, wrapped_z}, {y_scale, y_scale},
                                                  {y_max, y_max});
            min_noise += value[0] / frequency;
            max_noise += value[1] / frequency;
            frequency /= 2.0;
            continue;
        }
#endif
        if (min_octave) {
            min_noise += min_octave->sample(wrapped_x, wrapped_y, wrapped_z, y_scale, y_max) / frequency;
        }
        if (max_octave) {
            max_noise += max_octave->sample(wrapped_x, wrapped_y, wrapped_z, y_scale, y_max) / frequency;
        }
        frequency /= 2.0;
    }
    return clamped_lerp(blend, min_noise / 512.0, max_noise / 512.0) / 128.0;
}

SimplexNoise::SimplexNoise(random::LegacyRandom& rng) {
    xo_ = rng.next_double() * 256.0;
    yo_ = rng.next_double() * 256.0;
    zo_ = rng.next_double() * 256.0;
    for (std::size_t i = 0; i < 256; ++i) {
        permutation_[i] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t i = 0; i < 256; ++i) {
        const auto j = i + static_cast<std::size_t>(rng.next_int(static_cast<std::int32_t>(256U - i)));
        std::swap(permutation_[i], permutation_[j]);
    }
}

double SimplexNoise::sample2(const double xin, const double yin) const {
    constexpr double sqrt_3 = 1.73205080756887729352744634150587236;
    constexpr double f2 = 0.5 * (sqrt_3 - 1.0);
    constexpr double g2 = (3.0 - sqrt_3) / 6.0;
    const auto skew = (xin + yin) * f2;
    const auto i = java_floor_int(xin + skew);
    const auto j = java_floor_int(yin + skew);
    const auto unskew = java_add_int(i, j) * g2;
    const auto x0 = xin - (i - unskew);
    const auto y0 = yin - (j - unskew);
    const auto i1 = x0 > y0 ? 1 : 0;
    const auto j1 = x0 > y0 ? 0 : 1;
    const auto x1 = x0 - i1 + g2;
    const auto y1 = y0 - j1 + g2;
    const auto x2 = x0 - 1.0 + 2.0 * g2;
    const auto y2 = y0 - 1.0 + 2.0 * g2;
    const auto ii = static_cast<std::uint8_t>(i);
    const auto jj = static_cast<std::uint8_t>(j);
    const auto p = [this](const int index) { return permutation_[static_cast<std::uint8_t>(index)]; };
    const auto gi0 = p(ii + p(jj)) % 12U;
    const auto gi1 = p(ii + i1 + p(jj + j1)) % 12U;
    const auto gi2 = p(ii + 1 + p(jj + 1)) % 12U;
    return 70.0 * (corner_noise(gi0, x0, y0, 0.0, 0.5) + corner_noise(gi1, x1, y1, 0.0, 0.5) +
                   corner_noise(gi2, x2, y2, 0.0, 0.5));
}

}  // namespace minecraft::worldgen
