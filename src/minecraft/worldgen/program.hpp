#pragma once

#include "noise.hpp"

#include <minecraft/types.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace minecraft::worldgen {

enum class Op : std::uint8_t {
    constant,
    add,
    mul,
    minimum,
    maximum,
    clamp,
    abs,
    square,
    cube,
    half_negative,
    quarter_negative,
    invert,
    squeeze,
    gradient,
    noise,
    blended_noise,
    shifted_noise,
    shift,
    shift_a,
    shift_b,
    weird,
    spline,
    range_choice,
    interpolated,
    flat_cache,
    find_top_surface,
};

// One noise the program samples. Seed-bound instances are created from these on first use.
struct NoiseSpec {
    enum class Kind : std::uint8_t { normal, blended };
    Kind kind;
    std::string id;  // noise parameter id; empty for blended
    NoiseParameters parameters;
    std::array<double, 5> blended{};  // xz_scale, y_scale, xz_factor, y_factor, smear_scale_multiplier
};

struct SplinePoint {
    float location;
    float derivative;
    std::uint32_t value;
};

struct Instruction {
    Op op;
    bool memo = false;            // evaluation scopes cache the value
    bool y_free = false;          // the value never depends on the context's block y
    bool costly = false;          // the subtree samples noise
    bool bounded = false;         // the subtree reads blended noise through operations an interval bound passes
    std::uint32_t noise = 0;      // NoiseSpec index for noise-sampling operations
    std::uint32_t cell_slot = 0;  // interpolated only: index of its per-column cell cache
    std::uint32_t memo_slot = 0;  // memo only: index of its line in evaluation scopes
    std::array<std::uint32_t, 3> args{};
    std::uint32_t points_begin = 0, points_end = 0;  // spline only
    std::array<double, 4> p{};
};

// A noise router compiled once per dimension. It holds no seed state, so every sampler of that dimension shares
// it; children are instruction indices and analysis results live on the instructions.
struct Program {
    std::vector<Instruction> code;
    std::vector<SplinePoint> points;
    std::vector<NoiseSpec> noises;
    std::map<std::string, std::uint32_t, std::less<>> outputs;
    std::uint32_t cell_count = 0, memo_count = 0;

    int cell_width = 0, cell_height = 0, min_y = 0, height = 0, sea_level = 0;
    bool legacy_random = false, aquifers_enabled = false, ore_veins_enabled = false;
    std::string_view default_block, default_fluid;

    [[nodiscard]] static const Program& get(Dimension dimension);
    [[nodiscard]] std::uint32_t output(std::string_view name) const;
    // True when the value at quart-aligned positions ignores the evaluating column's cell origin.
    [[nodiscard]] bool column_free(std::uint32_t index) const;
};

// Pure arithmetic shared by the evaluator and compile-time folding, so both round identically.
[[nodiscard]] double apply_unary(Op op, double value, const std::array<double, 4>& p) noexcept;

}  // namespace minecraft::worldgen
