#pragma once

#include "minecraft/worldgen/density.hpp"
#include "minecraft/worldgen/noise.hpp"
#include "minecraft/worldgen/program.hpp"

#include <minecraft/random.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace witch_hut {
// Keep one instance per worker: direct() reuses the last column's shared shift-noise values.
class Climate {
public:
    explicit Climate(std::uint64_t seed)
        : seed_(seed), program_(minecraft::worldgen::Program::get(minecraft::Dimension::overworld)) {
        if (program_.legacy_random || !prepare_axes())
            return;

        noises_.resize(program_.noises.size());
        auto positional =
            minecraft::random::RandomSource(seed_, minecraft::random::RandomAlgorithm::xoroshiro).fork_positional();
        const auto init_noise = [&](std::uint32_t index) {
            if (index >= program_.noises.size())
                throw std::runtime_error("Malformed climate noise index");
            if (noises_[index])
                return;
            const auto& spec = program_.noises[index];
            if (spec.kind != minecraft::worldgen::NoiseSpec::Kind::normal)
                throw std::runtime_error("Unsupported climate noise kind");
            noises_[index] =
                std::make_unique<minecraft::worldgen::NormalNoise>(positional.from_hash_of(spec.id), spec.parameters);
        };
        init_noise(offset_noise_);
        for (const auto& axis : axes_) {
            init_noise(axis.noise);
        }
    }

    // Axis indices for sample().
    static constexpr std::size_t temperature = 0, vegetation = 1, continents = 2, erosion = 3, ridges = 4;

    [[nodiscard]] double direct(std::string_view name, minecraft::BlockPos position) const {
        for (std::size_t i = 0; i < axis_names.size(); ++i)
            if (axis_names[i] == name)
                return sample(i, position);
        return fallback().direct(name, position);
    }

    // The noise sample() reads for axis, null without the specialized path: axis_noise(axis)->sample(x / 4 + shift_x,
    // 0, z / 4 + shift_z), the shifts being the offset noise's warp.
    [[nodiscard]] const minecraft::worldgen::NormalNoise* axis_noise(std::size_t axis) const {
        return fast_path_ ? noises_[axes_[axis].noise].get() : nullptr;
    }

    // direct(axis_names[axis], position) without looking the axis up by name.
    [[nodiscard]] double sample(std::size_t axis, minecraft::BlockPos position) const {
        if (!fast_path_)
            return fallback().direct(axis_names[axis], position);
        const auto& a = axes_[axis];
        prepare_shifts(position.x, position.z);
        return noise(a.noise).sample(position.x * a.xz_scale + shift_x_, position.y * a.y_scale + 0.0,
                                     position.z * a.xz_scale + shift_z_);
    }

private:
    struct Axis {
        std::string_view name;
        std::uint32_t noise = 0;
        double xz_scale = 0.0, y_scale = 0.0;
    };

    static constexpr std::array<std::string_view, 5> axis_names{"temperature", "vegetation", "continents", "erosion",
                                                                "ridges"};
    static constexpr std::array<std::string_view, 5> noise_ids{"minecraft:temperature", "minecraft:vegetation",
                                                               "minecraft:continentalness", "minecraft:erosion",
                                                               "minecraft:ridge"};

    std::uint64_t seed_;
    const minecraft::worldgen::Program& program_;
    std::array<Axis, axis_names.size()> axes_{};
    std::vector<std::unique_ptr<minecraft::worldgen::NormalNoise>> noises_;
    std::uint32_t offset_noise_ = 0;
    bool fast_path_ = false;
    mutable bool shifts_valid_ = false;
    mutable std::int32_t cached_x_ = 0, cached_z_ = 0;
    mutable double shift_x_ = 0.0, shift_z_ = 0.0;
    mutable std::unique_ptr<minecraft::worldgen::DensitySampler> fallback_;

    [[nodiscard]] std::uint32_t without_flat_cache(std::uint32_t index) const {
        for (std::size_t depth = 0; depth <= program_.code.size(); ++depth) {
            if (index >= program_.code.size())
                return static_cast<std::uint32_t>(program_.code.size());
            const auto& instruction = program_.code[index];
            if (instruction.op != minecraft::worldgen::Op::flat_cache)
                return index;
            index = instruction.args[0];
        }
        return static_cast<std::uint32_t>(program_.code.size());
    }

    bool prepare_axes() {
        using minecraft::worldgen::Op;
        const auto& code = program_.code;
        const auto& noises = program_.noises;
        const auto zero = [&](std::uint32_t index) {
            index = without_flat_cache(index);
            return index < code.size() && code[index].op == Op::constant && code[index].p[0] == 0.0;
        };
        const auto shift = [&](std::uint32_t index, Op op) -> std::optional<std::uint32_t> {
            index = without_flat_cache(index);
            if (index >= code.size() || code[index].op != op || code[index].noise >= noises.size())
                return std::nullopt;
            const auto& spec = noises[code[index].noise];
            if (!code[index].y_free || spec.kind != minecraft::worldgen::NoiseSpec::Kind::normal ||
                spec.id != "minecraft:offset")
                return std::nullopt;
            return code[index].noise;
        };

        for (std::size_t i = 0; i < axes_.size(); ++i) {
            const auto output = program_.outputs.find(axis_names[i]);
            if (output == program_.outputs.end())
                return false;
            const auto index = without_flat_cache(output->second);
            if (index >= code.size())
                return false;
            const auto& instruction = code[index];
            if (instruction.op != Op::shifted_noise || !instruction.y_free || instruction.p[0] != 0.25 ||
                instruction.p[1] != 0.0 || instruction.noise >= noises.size() || !zero(instruction.args[1]))
                return false;
            const auto shift_x = shift(instruction.args[0], Op::shift_a);
            const auto shift_z = shift(instruction.args[2], Op::shift_b);
            const auto& spec = noises[instruction.noise];
            if (!shift_x || !shift_z || *shift_x != *shift_z || (i != 0 && *shift_x != offset_noise_) ||
                spec.kind != minecraft::worldgen::NoiseSpec::Kind::normal || spec.id != noise_ids[i])
                return false;
            axes_[i] = {axis_names[i], instruction.noise, instruction.p[0], instruction.p[1]};
            offset_noise_ = *shift_x;
        }
        fast_path_ = true;
        return true;
    }

    [[nodiscard]] const minecraft::worldgen::NormalNoise& noise(std::uint32_t index) const {
        return *noises_[index];
    }

    void prepare_shifts(std::int32_t x, std::int32_t z) const {
        if (shifts_valid_ && x == cached_x_ && z == cached_z_)
            return;
        constexpr double scale = 0.25;
        const auto& offset = noise(offset_noise_);
        shift_x_ = offset.sample(x * scale, 0.0, z * scale) * 4.0;
        shift_z_ = offset.sample(z * scale, x * scale, 0.0) * 4.0;
        cached_x_ = x;
        cached_z_ = z;
        shifts_valid_ = true;
    }

    [[nodiscard]] const minecraft::worldgen::DensitySampler& fallback() const {
        if (!fallback_)
            fallback_ = std::make_unique<minecraft::worldgen::DensitySampler>(seed_, minecraft::Dimension::overworld, true);
        return *fallback_;
    }
};
}  // namespace witch_hut
