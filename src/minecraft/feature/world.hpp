#pragma once

#include <minecraft/biome_sampler.hpp>
#include <minecraft/random.hpp>
#include <minecraft/terrain.hpp>

#include "biome_memo.hpp"
#include "rules.hpp"
#include "surface.hpp"

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace minecraft::feature {

struct Cell {
    BlockId block;
    bool uncertain;
};

// Overworld chunks at the end of CARVERS, without structure terrain adaptation: the input of FEATURES. Below the
// surface rules' reach a block is base terrain plus the bedrock and deepslate gradients; nearer the surface whole
// chunks are built with SurfaceBuilder on first use. Carving runs once per chunk before its first read; its writes
// stay in an overlay until one could move a heightmap, which builds the surface chunk and replays them into it.
class BaseTerrain {
public:
    explicit BaseTerrain(std::uint64_t seed, std::size_t max_chunks = 256);
    ~BaseTerrain();

    [[nodiscard]] Cell at(BlockPos p);
    // A height the OCEAN_FLOOR_WG heightmap is known to reach, without building the surface.
    [[nodiscard]] std::int32_t ocean_floor_lower_bound(std::int32_t x, std::int32_t z);

    static constexpr std::int32_t min_y = -64, height = 384;

private:
    struct Column {
        std::int32_t top, min_surface, lower_floor, lower_surface;
        std::int8_t special = -1;
    };
    class Target;
    struct Chunk;
    struct Gradient {
        random::PositionalFactory random;
        std::int32_t true_at_and_below, false_at_and_above;
        BlockId block;
        [[nodiscard]] bool test(std::int32_t x, std::int32_t y, std::int32_t z) const;
    };
    Chunk& chunk(std::int32_t chunk_x, std::int32_t chunk_z);
    Column& column(Chunk& chunk, std::int32_t x, std::int32_t z);
    bool fast(Column& column, std::int32_t y) const;
    const SurfaceChunk& surface(Chunk& chunk);
    BlockId fast_block(Chunk& chunk, std::int32_t x, std::int32_t y, std::int32_t z);
    // A block of a chunk as carving left it so far.
    Cell read(Chunk& chunk, BlockPos p);
    Chunk& carved(std::int32_t chunk_x, std::int32_t chunk_z);
    void fill_section(Chunk& chunk, std::int32_t x, std::int32_t z, std::int32_t section);

    std::uint64_t seed_;
    std::size_t max_chunks_;
    terrain::Sampler terrain_;
    biome::Sampler biomes_;
    mutable BiomeMemo biome_memo_{biomes_};
    SurfaceBuilder surface_builder_;
    std::vector<Gradient> below_surface_;  // Surface-rule gradients ahead of above_preliminary_surface (bedrock).
    std::vector<Gradient> deep_;           // Gradients after it (deepslate).
    std::unordered_map<std::uint64_t, std::unique_ptr<Chunk>> chunks_;
    std::list<std::uint64_t> order_;
};

}  // namespace minecraft::feature
