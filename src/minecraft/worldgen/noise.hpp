#pragma once

#include <minecraft/types.hpp>
#include <minecraft/random.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace minecraft::worldgen {

struct NoiseParameters {
    int first_octave;
    std::vector<double> amplitudes;
};

struct PairKernel;

class ImprovedNoise {
public:
    explicit ImprovedNoise(random::RandomSource& rng);
    // Equivalent to the five-argument form with y_scale == 0, which skips the vertical fudge.
    [[nodiscard]] double sample(double x, double y, double z) const;
    [[nodiscard]] double sample(double x, double y, double z, double y_scale, double y_max) const;
    // The lattice offset and permutation, for evaluators that batch samples outside the library.
    [[nodiscard]] std::array<double, 3> origin() const {
        return {xo_, yo_, zo_};
    }
    [[nodiscard]] const std::array<std::uint8_t, 256>& permutation() const {
        return permutation_;
    }

private:
    [[nodiscard]] double sample_cell(std::int32_t cell_x, std::int32_t cell_y, std::int32_t cell_z, double local_x,
                                     double local_y, double local_z, double gradient_y) const;
    double xo_{};
    double yo_{};
    double zo_{};
    std::array<std::uint8_t, 256> permutation_{};
};

class PerlinNoise {
public:
    PerlinNoise(random::RandomSource& rng, const NoiseParameters& parameters, bool modern = true);
    [[nodiscard]] double sample(double x, double y, double z) const;
    [[nodiscard]] double sample(double x, double y, double z, double y_scale, double y_max) const;

    // One octave of sample(x, y, z): weight * noise->sample(wrap(x * input_factor), ...), summed in octave order.
    struct Term {
        const ImprovedNoise* noise;
        double input_factor;
        double weight;  // amplitude times value factor
    };
    [[nodiscard]] std::vector<Term> terms() const {
        std::vector<Term> result;
        for (const auto& octave : octaves_)
            result.push_back({&octave.noise, octave.input_factor, octave.amplitude * octave.value_factor});
        return result;
    }

private:
    friend class BlendedNoise;
    friend class NormalNoise;
    friend struct PairKernel;
    // Octaves with a nonzero amplitude, ascending, with their per-octave factors precomputed. The factors are exact
    // powers-of-two steps from the lowest octave, so they equal the running products vanilla keeps.
    struct Octave {
        ImprovedNoise noise;
        double amplitude;
        double input_factor;
        double value_factor;
    };
    [[nodiscard]] static double wrap(double value) noexcept;
    // The sum of octaves [0, end), added in order.
    [[nodiscard]] double sum(double x, double y, double z, std::size_t end) const;
    [[nodiscard]] const ImprovedNoise* octave(std::size_t index) const;
    [[nodiscard]] std::size_t octave_count() const {
        return slots_.size();
    }
    std::vector<Octave> octaves_;
    std::vector<std::int32_t> slots_;  // octave index -> position in octaves_, or -1
};

class NormalNoise {
public:
    NormalNoise(random::RandomSource rng, const NoiseParameters& parameters);
    [[nodiscard]] double sample(double x, double y, double z) const;
    // sample() is (first(x, y, z) + second(f x, f y, f z)) * value_factor() with f = second_input_factor.
    static constexpr double second_input_factor = 1.0181268882175227;
    [[nodiscard]] const PerlinNoise& first() const {
        return first_;
    }
    [[nodiscard]] const PerlinNoise& second() const {
        return second_;
    }
    [[nodiscard]] double value_factor() const {
        return value_factor_;
    }

private:
    PerlinNoise first_;
    PerlinNoise second_;
    double value_factor_{};
};

class BlendedNoise {
public:
    BlendedNoise(random::RandomSource& rng, double xz_scale, double y_scale, double xz_factor, double y_factor,
                 double smear_scale);
    [[nodiscard]] double sample(BlockPos position) const;
    // A bound on |sample(position)| for every position.
    [[nodiscard]] double max_magnitude() const {
        return max_magnitude_;
    }

private:
    double max_magnitude_{};
    double xz_scale_{};
    double y_scale_{};
    double xz_factor_{};
    double y_factor_{};
    double smear_scale_{};
    PerlinNoise min_limit_;
    PerlinNoise max_limit_;
    PerlinNoise main_;
};

class SimplexNoise {
public:
    explicit SimplexNoise(random::LegacyRandom& rng);
    [[nodiscard]] double sample2(double x, double z) const;

private:
    double xo_{};
    double yo_{};
    double zo_{};
    std::array<std::uint8_t, 512> permutation_{};
};

}  // namespace minecraft::worldgen
