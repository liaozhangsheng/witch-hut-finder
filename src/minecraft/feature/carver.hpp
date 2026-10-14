#pragma once

#include <minecraft/biome_sampler.hpp>

#include "rules.hpp"

#include <cstdint>
#include <optional>

namespace minecraft::feature {

// The chunk CARVERS writes to, seen through the operations WorldCarver.carveBlock performs on a ProtoChunk.
class CarveTarget {
public:
    virtual ~CarveTarget() = default;
    [[nodiscard]] virtual BlockId get(BlockPos p) = 0;
    // ProtoChunk.setBlockState, heightmaps included.
    virtual void set(BlockPos p, BlockId block) = 0;
    // The chunk aquifer's substance for a carved block (density 0); empty when a barrier keeps the block.
    [[nodiscard]] virtual std::optional<BlockId> carved_substance(BlockPos p) = 0;
    // CarvingContext.topMaterial for the dirt below a carved grass block.
    [[nodiscard]] virtual std::optional<BlockId> top_material(BlockPos p, bool under_fluid) = 0;
};

// NoiseBasedChunkGenerator.applyCarvers for one overworld chunk: every carver of the chunks within eight chunks.
void carve_chunk(std::uint64_t seed, ChunkPos chunk, const biome::Sampler& biomes, CarveTarget& target);

}  // namespace minecraft::feature
