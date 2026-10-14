#pragma once

#include <minecraft/biome.hpp>
#include <minecraft/types.hpp>

#include <memory>
#include <span>

namespace minecraft::biome {

class Sampler {
public:
    Sampler(std::uint64_t seed, Dimension dimension);
    ~Sampler();
    Sampler(Sampler&&) noexcept;
    Sampler& operator=(Sampler&&) noexcept;

    [[nodiscard]] Sample noise(QuartPos position) const;
    // BiomeManager's zoom alone, over an unclamped source as generation's carvers read it. The quart y can leave the
    // world by one at its bottom and top four block layers.
    [[nodiscard]] QuartPos block_quart(BlockPos position) const;
    // block_quart with quart y clamped to the dimension type's height, as ChunkAccess.getNoiseBiome reads its storage.
    [[nodiscard]] QuartPos stored_block_quart(BlockPos position) const;
    [[nodiscard]] std::optional<bool> matches(QuartPos position, std::span<const std::string_view> allowed) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::span<const std::string_view> catalog();

}  // namespace minecraft::biome
