#pragma once

#include "minecraft/feature/world.hpp"
#include "minecraft/worldgen/density.hpp"
#include <minecraft/random.hpp>
#include <minecraft/terrain.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace witch_hut {
struct Footprint {
    int x, z, width, depth, direction;  // NORTH, EAST, SOUTH, WEST, as Direction.Plane.HORIZONTAL.

    [[nodiscard]] minecraft::BlockPos floor_point(int ground_y) const {
        switch (direction) {
            case 0:
                return {x + 2, ground_y + 1, z + depth - 1 - 5};
            case 1:
                return {x + 5, ground_y + 1, z + 2};
            case 2:
                return {x + 2, ground_y + 1, z + 5};
            default:
                return {x + width - 1 - 5, ground_y + 1, z + 2};
        }
    }
};

inline Footprint footprint(std::uint64_t seed, minecraft::ChunkPos chunk) {
    minecraft::random::LegacyRandom random(minecraft::random::large_feature_seed(seed, chunk.x, chunk.z));
    const int direction = random.next_int(4);
    return {chunk.x * 16, chunk.z * 16, direction % 2 ? 9 : 7, direction % 2 ? 7 : 9, direction};
}

struct Floor {
    minecraft::BlockPos point;
    int ground_y;
    bool uncertain;
};

struct FloorInspection {
    std::optional<Floor> floor;
    int lower_bound;
};

// shortcut: terrain stops at CARVERS before early features and neighboring structures, validate in-game for exact Y.
class FloorSampler {
public:
    explicit FloorSampler(std::uint64_t seed)
        : seed_(seed), terrain_(seed, 16),
          preview_(seed, minecraft::Dimension::overworld, minecraft::terrain::ColumnSource::chunk_generation),
          density_(seed, minecraft::Dimension::overworld) {}

    // Whether the preview floor, the average base-terrain height of nine footprint columns plus one, reaches
    // incumbent. shortcut: nine columns can miss carver openings and local lows. Positive final density at y makes
    // the block solid whatever the aquifer says, so the top-down scan stops at or above it: height >= y + 1. A few
    // such probes usually prove the rejection; only otherwise are the nine columns scanned from the top.
    [[nodiscard]] bool preview_rejects(minecraft::ChunkPos chunk, int incumbent) const {
        const int min_y = density_.min_y(), top = min_y + density_.height() - 1, step = density_.cell_height();
        if (incumbent > top + 2)
            return false;  // Above the highest possible preview, top + 1 + 1.
        const auto box = footprint(seed_, chunk);
        const int start = std::max(min_y, minecraft::floor_div(incumbent - 2 + step - 1, step) * step);
        const auto rejects = [incumbent](int sum) { return sum / 9 + 1 >= incumbent; };
        // A probe interpolates between the corner columns around its column, one per axis the column is not
        // cell-aligned on, so aligned columns go first: with a low incumbent two solid columns already reject.
        struct Probe {
            int x, z, corners;
        };
        std::array<Probe, 9> columns;
        int i = 0;
        const int width = density_.cell_width();
        for (int iz : {0, box.depth / 2, box.depth - 1})
            for (int ix : {0, box.width / 2, box.width - 1}) {
                const int x = box.x + ix, z = box.z + iz;
                columns[i++] = {x, z, (x % width ? 2 : 1) * (z % width ? 2 : 1)};
            }
        std::stable_sort(columns.begin(), columns.end(),
                         [](const Probe& a, const Probe& b) { return a.corners < b.corners; });
        std::array<std::optional<minecraft::worldgen::DensitySampler::ColumnScope>, 9> scopes;
        const auto scope = [&](int c) -> minecraft::worldgen::DensitySampler::ColumnScope& {
            if (!scopes[c])
                scopes[c].emplace(density_, columns[c].x, columns[c].z, minecraft::worldgen::NoiseScope::chunk);
            return *scopes[c];
        };
        std::array<int, 9> bound;
        bound.fill(min_y);
        int sum = 9 * min_y;
        // Swamp ground mostly sits a little under sea level, so two high levels usually settle a column at once; the
        // rest climb from the lowest useful level out of whatever cave they start in. A solid level raises the
        // column's bound, which lets solid columns pay for columns still in caves.
        int previous = min_y - 1;
        for (const auto [absolute, level] : probe_ladder) {
            const int y = absolute ? std::max(start, level) : start + level;
            if (y > top || y == previous)
                continue;  // Clamping to start can repeat a level.
            previous = y;
            for (int c = 0; c < 9; ++c) {
                if (bound[c] >= y + 1)
                    continue;
                if (scope(c).column("final_density", y, true) > 0) {
                    sum += y + 1 - bound[c];
                    bound[c] = y + 1;
                    if (rejects(sum))
                        return true;
                }
            }
        }
        // Exact heights only raise the sum, so the columns with the most to gain are scanned first and the scan
        // stops once the sum proves rejection; finishing all nine gives the exact preview.
        std::array<int, 9> order;
        for (int c = 0; c < 9; ++c)
            order[c] = c;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return bound[a] < bound[b]; });
        for (const int c : order) {
            scopes[c].reset();
            sum += preview_.height(columns[c].x, columns[c].z) - bound[c];
            if (rejects(sum))
                return true;
        }
        return false;
    }

    [[nodiscard]] FloorInspection inspect(minecraft::ChunkPos chunk, int incumbent) {
        const auto box = footprint(seed_, chunk);
        constexpr int columns = 63;
        int sum = 0, bound_sum = 0, done = 0;
        bool uncertain = false;
        for (int z = box.z; z < box.z + box.depth; ++z) {
            for (int x = box.x; x < box.x + box.width; ++x) {
                const auto [height, tied] = height_at(x, z);
                sum += height;
                bound_sum += tied ? minecraft::feature::BaseTerrain::min_y : height;
                uncertain = uncertain || tied;
                ++done;
                // Both C++ and Java integer division truncate toward zero, including negative ground heights.
                const int lower = (bound_sum + (columns - done) * minecraft::feature::BaseTerrain::min_y) / columns + 1;
                if (lower >= incumbent)
                    return {std::nullopt, lower};
            }
        }
        const int ground_y = sum / columns;
        return {Floor{box.floor_point(ground_y), ground_y, uncertain}, bound_sum / columns + 1};
    }

private:
    struct ProbeLevel {
        bool absolute;  // Otherwise relative to the lowest level whose bound alone can reject.
        int y;
    };
    static constexpr std::array<ProbeLevel, 5> probe_ladder{
        {{true, 56}, {true, 40}, {false, 0}, {false, 16}, {false, 32}}};
    std::uint64_t seed_;
    minecraft::feature::BaseTerrain terrain_;
    minecraft::terrain::Sampler preview_;
    minecraft::worldgen::DensitySampler density_;

    [[nodiscard]] std::pair<int, bool> height_at(int x, int z) {
        const auto& catalog = minecraft::feature::Catalog::get();
        bool uncertain = false;
        for (int y = minecraft::feature::BaseTerrain::min_y + minecraft::feature::BaseTerrain::height - 1;
             y >= minecraft::feature::BaseTerrain::min_y; --y) {
            const auto cell = terrain_.at({x, y, z});
            const auto& block = catalog.info(cell.block);
            uncertain = uncertain || cell.uncertain;
            if ((block.blocks_motion || block.fluid != minecraft::feature::Fluid::none) && !block.id.ends_with("_leaves"))
                return {y + 1, uncertain};
        }
        return {minecraft::feature::BaseTerrain::min_y, uncertain};
    }
};
}  // namespace witch_hut
