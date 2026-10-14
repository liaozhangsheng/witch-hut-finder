#pragma once

#include <minecraft/types.hpp>

#include "../worldgen/density.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace minecraft::terrain {

enum class VeinKind : std::uint8_t { copper, iron };
enum class VeinMaterial : std::uint8_t { filler, ore, raw_block };

struct VeinBlock {
    VeinKind kind;
    VeinMaterial material;
};

// OreVeinifier: what a position becomes when the base density there is solid. Positions whose density is not positive
// keep their aquifer substance and never hold vein blocks.
[[nodiscard]] std::optional<VeinBlock> vein_block(const worldgen::DensitySampler& density,
                                                  worldgen::DensitySampler::ColumnScope& scope, BlockPos p);
[[nodiscard]] std::string_view vein_block_id(VeinBlock block);

}  // namespace minecraft::terrain
