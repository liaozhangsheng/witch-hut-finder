#pragma once

#include <minecraft/types.hpp>

#include <cstdint>

namespace minecraft::biome {

[[nodiscard]] std::uint64_t obfuscate_seed(std::uint64_t world_seed);
[[nodiscard]] QuartPos zoom_quart(std::uint64_t obfuscated_seed, BlockPos pos);

}  // namespace minecraft::biome
