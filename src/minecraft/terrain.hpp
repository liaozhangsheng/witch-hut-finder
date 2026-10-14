#pragma once

#include <minecraft/types.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace minecraft::terrain {

struct Column {
    std::vector<std::string_view> blocks;
};

// Which vanilla NoiseChunk columns come from: getBaseColumn and getBaseHeight build one per noise cell, chunk
// generation (fillFromNoise) one per chunk. They differ only where aquifers sample erosion and depth off the column.
enum class ColumnSource { base_column, chunk_generation };

// The generator's base column before surface rules, carvers and structures.
class Sampler {
public:
    Sampler(std::uint64_t seed, Dimension dimension, ColumnSource source = ColumnSource::base_column);
    ~Sampler();
    Sampler(Sampler&&) noexcept;
    Sampler& operator=(Sampler&&) noexcept;
    [[nodiscard]] Column column(int x, int z) const;
    // Blocks min_y..max_y of column(x, z), bottom to top; out must hold max_y - min_y + 1 entries.
    void blocks(int x, int z, int min_y, int max_y, std::span<std::string_view> out) const;
    [[nodiscard]] int height(int x, int z) const;

    // NoiseBasedAquifer.computeSubstance for one column, as carvers ask it with density 0: the fluid or air a carved
    // block becomes, or nothing when an aquifer barrier keeps the block.
    class AquiferProbe {
    public:
        AquiferProbe(const Sampler& sampler, int x, int z);
        ~AquiferProbe();
        AquiferProbe(AquiferProbe&&) noexcept;
        AquiferProbe& operator=(AquiferProbe&&) noexcept;
        [[nodiscard]] std::optional<std::string_view> carve(int y);

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace minecraft::terrain
