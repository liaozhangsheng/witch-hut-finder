#pragma once

#include <minecraft/biome_sampler.hpp>
#include <minecraft/terrain.hpp>

#include "rules.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace minecraft::feature {

// One overworld chunk at the end of SURFACE: base terrain (without structure terrain adaptation) with
// SurfaceSystem.buildSurface applied, including the eroded badlands pillars and frozen ocean icebergs.
struct SurfaceChunk {
    static constexpr std::int32_t min_y = -64, height = 384;
    std::vector<BlockId> blocks = std::vector<BlockId>(256U * height);  // [(x & 15) * 16 + (z & 15)][y - min_y]
    std::array<std::int32_t, 256> world_surface{}, ocean_floor{};       // First available heights, as getHeight.
    // Columns whose surface consulted a biome lost to a climate tie: their blocks from uncertain_from up may differ.
    std::array<std::int32_t, 256> uncertain_from{};
    // Bounds on the OCEAN_FLOOR_WG first-available height over every resolution of those ties; ocean_floor is the
    // height of this chunk's resolution. Only meaningful in tied columns; the maximum is INT32_MAX when unknown.
    std::array<std::int32_t, 256> ocean_floor_min{}, ocean_floor_max{};
    [[nodiscard]] BlockId at(std::int32_t x, std::int32_t y, std::int32_t z) const {
        return blocks[static_cast<std::size_t>(((x & 15) * 16 + (z & 15)) * height + (y - min_y))];
    }
    // ProtoChunk.setBlockState: the block, then Heightmap.update for both worldgen heightmaps.
    void set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId block);
};

// Not thread-safe: it caches preliminary surface levels. Use one per thread.
class SurfaceBuilder {
public:
    explicit SurfaceBuilder(std::uint64_t seed);
    ~SurfaceBuilder();
    SurfaceBuilder(SurfaceBuilder&&) noexcept;
    SurfaceBuilder& operator=(SurfaceBuilder&&) noexcept;

    [[nodiscard]] SurfaceChunk build(ChunkPos chunk) const;
    // The level below which surface rules only lay bedrock and deepslate gradients (Context.getMinSurfaceLevel).
    [[nodiscard]] std::int32_t min_surface_level(std::int32_t x, std::int32_t z) const;
    // SurfaceSystem.topMaterial: the surface rule at pos with one block of stone above and below, which carvers use to
    // re-cover dirt they expose under grass. Steepness reads chunk's current heightmap; biomes are not clamped, as
    // with the carving context's biome manager.
    [[nodiscard]] std::optional<BlockId> top_material(const SurfaceChunk& chunk, BlockPos pos, bool under_fluid) const;

    struct Impl;  // Internal; public so the rule evaluator in surface.cpp can name it.

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace minecraft::feature
