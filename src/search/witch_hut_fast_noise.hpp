#pragma once

#include "witch_hut_climate.hpp"

#include "minecraft/worldgen/noise.hpp"
#include "minecraft/worldgen/program.hpp"

#include <minecraft/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace witch_hut {
// Approximate erosion for coarse tile rejection. Candidates that survive a tile filter still need exact checking.
class FastErosion {
public:
    explicit FastErosion(std::uint64_t seed)
        : seed_(seed), program_(minecraft::worldgen::Program::get(minecraft::Dimension::overworld)) {
        const auto shape = erosion_shape();
        if (!shape)
            return;

        const auto& offset_spec = program_.noises[shape->first];
        const auto& erosion_spec = program_.noises[shape->second];
        const auto offset = sparse(offset_spec.parameters, 1);
        const auto erosion = sparse(erosion_spec.parameters, 2);
        if (!offset || !erosion)
            return;

        auto positional =
            minecraft::random::RandomSource(seed_, minecraft::random::RandomAlgorithm::xoroshiro).fork_positional();
        offset_ = std::make_unique<minecraft::worldgen::NormalNoise>(positional.from_hash_of(offset_spec.id),
                                                                   offset->parameters);
        offset_scale_ = offset->scale;
        erosion_ = std::make_unique<minecraft::worldgen::NormalNoise>(positional.from_hash_of(erosion_spec.id),
                                                                    erosion->parameters);
        erosion_scale_ = erosion->scale;
    }

    // Raw climate noise value, before quantization by 10,000. This intentionally omits higher octaves.
    [[nodiscard]] double sample(std::int32_t x, std::int32_t z) const {
        if (!offset_ || !erosion_)
            return exact().direct("erosion", {x, 0, z});
        constexpr double scale = 0.25;
        const double shift_x = offset_->sample(x * scale, 0.0, z * scale) * offset_scale_ * 4.0;
        const double shift_z = offset_->sample(z * scale, x * scale, 0.0) * offset_scale_ * 4.0;
        return erosion_->sample(x * scale + shift_x, 0.0, z * scale + shift_z) * erosion_scale_;
    }

    // sample() without the offset warp: the same two erosion octaves at the unshifted position, four noise samples
    // instead of eight. The warp moves the value little; see FastFilter::warp_margin.
    [[nodiscard]] double sample_unwarped(std::int32_t x, std::int32_t z) const {
        if (!offset_ || !erosion_)
            return exact().direct("erosion", {x, 0, z});
        constexpr double scale = 0.25;
        return erosion_->sample(x * scale, 0.0, z * scale) * erosion_scale_;
    }

    // sample_unwarped(x, z) is unwarped_noise()->sample(x / 4, 0, z / 4) * unwarped_factor() when approximate().
    [[nodiscard]] const minecraft::worldgen::NormalNoise* unwarped_noise() const noexcept {
        return erosion_.get();
    }
    [[nodiscard]] double unwarped_factor() const noexcept {
        return erosion_scale_;
    }
    // sample()'s warp: shift_x = offset_noise()->sample(x / 4, 0, z / 4) * offset_factor(), shift_z the same at
    // (z / 4, x / 4, 0); the erosion is then read at (x / 4 + shift_x, 0, z / 4 + shift_z).
    [[nodiscard]] const minecraft::worldgen::NormalNoise* offset_noise() const noexcept {
        return offset_.get();
    }
    [[nodiscard]] double offset_factor() const noexcept {
        return offset_scale_ * 4.0;
    }

    [[nodiscard]] bool approximate() const noexcept {
        return offset_ != nullptr && erosion_ != nullptr;
    }

private:
    struct Sparse {
        minecraft::worldgen::NoiseParameters parameters;
        double scale;
    };

    std::uint64_t seed_;
    const minecraft::worldgen::Program& program_;
    std::unique_ptr<minecraft::worldgen::NormalNoise> offset_;
    std::unique_ptr<minecraft::worldgen::NormalNoise> erosion_;
    double offset_scale_ = 1.0, erosion_scale_ = 1.0;
    mutable std::unique_ptr<Climate> exact_;

    [[nodiscard]] static double normal_scale(const minecraft::worldgen::NoiseParameters& parameters) {
        auto minimum = parameters.amplitudes.size();
        std::size_t maximum = 0;
        for (std::size_t i = 0; i < parameters.amplitudes.size(); ++i) {
            if (parameters.amplitudes[i] == 0.0)
                continue;
            minimum = std::min(minimum, i);
            maximum = std::max(maximum, i);
        }
        if (minimum == parameters.amplitudes.size())
            return 0.0;
        const auto span = maximum - minimum;
        return 0.16666666666666666 / (0.1 * (1.0 + 1.0 / (static_cast<double>(span) + 1.0)));
    }

    [[nodiscard]] static std::optional<Sparse> sparse(const minecraft::worldgen::NoiseParameters& original,
                                                      std::size_t keep) {
        auto parameters = original;
        std::size_t active = 0;
        for (std::size_t i = 0; i < parameters.amplitudes.size(); ++i) {
            if (original.amplitudes[i] == 0.0)
                continue;
            if (active++ >= keep)
                parameters.amplitudes[i] = 0.0;
        }
        const double full_scale = normal_scale(original);
        const double partial_scale = normal_scale(parameters);
        if (full_scale == 0.0 || partial_scale == 0.0)
            return std::nullopt;
        return Sparse{std::move(parameters), full_scale / partial_scale};
    }

    [[nodiscard]] std::uint32_t unwrap_cache(std::uint32_t index) const {
        using minecraft::worldgen::Op;
        for (std::size_t depth = 0; depth <= program_.code.size(); ++depth) {
            if (index >= program_.code.size())
                return static_cast<std::uint32_t>(program_.code.size());
            const auto& node = program_.code[index];
            if (node.op != Op::flat_cache)
                return index;
            index = node.args[0];
        }
        return static_cast<std::uint32_t>(program_.code.size());
    }

    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> erosion_shape() const {
        using minecraft::worldgen::Op;
        if (program_.legacy_random)
            return std::nullopt;
        const auto output = program_.outputs.find("erosion");
        if (output == program_.outputs.end())
            return std::nullopt;
        const auto index = unwrap_cache(output->second);
        if (index >= program_.code.size())
            return std::nullopt;
        const auto& node = program_.code[index];
        if (node.op != Op::shifted_noise || !node.y_free || node.p[0] != 0.25 || node.p[1] != 0.0 ||
            node.noise >= program_.noises.size())
            return std::nullopt;
        const auto zero_index = unwrap_cache(node.args[1]);
        if (zero_index >= program_.code.size() || program_.code[zero_index].op != Op::constant ||
            program_.code[zero_index].p[0] != 0.0)
            return std::nullopt;
        const auto shift_x = unwrap_cache(node.args[0]);
        const auto shift_z = unwrap_cache(node.args[2]);
        if (shift_x >= program_.code.size() || shift_z >= program_.code.size())
            return std::nullopt;
        const auto& x_node = program_.code[shift_x];
        const auto& z_node = program_.code[shift_z];
        if (x_node.op != Op::shift_a || z_node.op != Op::shift_b || !x_node.y_free || !z_node.y_free ||
            x_node.noise >= program_.noises.size() || x_node.noise != z_node.noise)
            return std::nullopt;
        const auto& offset = program_.noises[x_node.noise];
        const auto& erosion = program_.noises[node.noise];
        if (offset.kind != minecraft::worldgen::NoiseSpec::Kind::normal || offset.id != "minecraft:offset" ||
            erosion.kind != minecraft::worldgen::NoiseSpec::Kind::normal || erosion.id != "minecraft:erosion")
            return std::nullopt;
        return std::pair{x_node.noise, node.noise};
    }

    [[nodiscard]] const Climate& exact() const {
        if (!exact_)
            exact_ = std::make_unique<Climate>(seed_);
        return *exact_;
    }
};
}  // namespace witch_hut
