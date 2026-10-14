#pragma once

#include "witch_hut_filter.hpp"
#include "witch_hut_fast_filter.hpp"
#include "witch_hut_floor.hpp"
#include "minecraft/worldgen/density.hpp"

#include <minecraft/biome_sampler.hpp>
#include <minecraft/random.hpp>
#include <minecraft/structure.hpp>
#include <minecraft/terrain.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace witch_hut {
constexpr int world_edge = 29999984;

// Java nextInt(24), including its rare rejection draw; unsigned arithmetic preserves Java overflow.
inline minecraft::ChunkPos candidate(std::uint64_t seed, int rx, int rz) {
    auto state =
        (minecraft::random::large_feature_with_salt_seed(seed, rx, rz, 14357620) ^ 0x5deece66dULL) & ((1ULL << 48) - 1);
    const auto offset = [&]() {
        std::uint32_t bits, value;
        do {
            state = (state * 0x5deece66dULL + 11) & ((1ULL << 48) - 1);
            bits = static_cast<std::uint32_t>(state >> 17);
            value = bits % 24;
        } while (bits - value + 23 >= (1U << 31));
        return static_cast<int>(value);
    };
    const int x = offset(), z = offset();
    return {rx * 32 + x, rz * 32 + z};
}

inline bool in_bounds(minecraft::ChunkPos chunk, int edge) {
    const int x = chunk.x * 16, z = chunk.z * 16;
    return x + 8 >= -edge && x < edge && z + 8 >= -edge && z < edge;
}

constexpr unsigned max_top = 1000;

struct Hut {
    minecraft::BlockPos point;             // The floor: footprint anchor at the average ground height plus one.
    minecraft::BlockPos generation_point;  // Where structure generation checked the biome.
    bool height_uncertain = false;
};

struct Result {
    std::vector<Hut> huts;  // The lowest floors found, ascending by y; equal floors keep the order they were found.
    std::uint64_t regions = 0, candidates = 0, filtered = 0, bounded = 0, heights = 0, unknown = 0;
    std::uint64_t tiles_skipped = 0, regions_skipped = 0, proxy_rejected = 0, direct_bounded = 0;
    std::uint64_t floor_preview_rejected = 0;
    bool scan_complete = false;
};

// Keeps the top lowest huts. Pruning rejects whatever cannot beat the top-th floor, so a larger top screens less.
inline Result search(std::uint64_t seed, int edge, unsigned workers, unsigned top, const std::function<bool()>& stop = {},
                     const std::function<void(const Result&, std::uint64_t)>& progress = {}) {
    const auto* set = minecraft::structure::find_set("minecraft:swamp_huts");
    if (!set || set->structure_id != "minecraft:swamp_hut" ||
        set->placement.spacing != 32 || set->placement.separation != 8 || set->placement.salt != 14357620)
        throw std::runtime_error("Unsupported swamp hut placement profile");
    if (edge < 1 || edge > world_edge || workers < 1 || workers > 256 || top < 1 || top > max_top)
        throw std::invalid_argument("Invalid border, worker count or result count");

    const int first = minecraft::floor_div(minecraft::floor_div(7 - edge, 16), 32);
    const int last = minecraft::floor_div(minecraft::floor_div(edge - 1, 16), 32);
    const std::uint64_t side = last - first + 1, total = side * side;
    constexpr int tile_width = FastFilter::tile_width;
    const int first_tile = minecraft::floor_div(first, tile_width), last_tile = minecraft::floor_div(last, tile_width);
    const std::uint64_t tile_side = total ? last_tile - first_tile + 1 : 0, total_tiles = tile_side * tile_side;
    std::atomic<std::uint64_t> next{0};
    // A hut enters the results only with a floor strictly below this: the top-th floor once that many are known.
    std::atomic<int> cutoff{322};
    std::atomic<unsigned> running{workers};
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable finished;
    Result result;
    std::exception_ptr error;
    const auto snapshot = [&] {
        const std::lock_guard lock(mutex);
        return result;
    };
    const auto add = [](Result& to, const Result& from) {
        to.regions += from.regions;
        to.candidates += from.candidates;
        to.filtered += from.filtered;
        to.bounded += from.bounded;
        to.heights += from.heights;
        to.unknown += from.unknown;
        to.tiles_skipped += from.tiles_skipped;
        to.regions_skipped += from.regions_skipped;
        to.proxy_rejected += from.proxy_rejected;
        to.direct_bounded += from.direct_bounded;
        to.floor_preview_rejected += from.floor_preview_rejected;
    };
    const auto worker = [&] {
        Result pending;
        int pending_tiles = 0;
        const auto flush = [&] {
            const std::lock_guard lock(mutex);
            add(result, pending);
            pending = Result{};
            pending_tiles = 0;
        };
        try {
            Filter filter(seed);
            FastFilter fast(seed);
            minecraft::biome::Sampler biomes(seed, minecraft::Dimension::overworld);
            minecraft::terrain::Sampler terrain(seed, minecraft::Dimension::overworld);
            FloorSampler floors(seed);
            minecraft::worldgen::DensitySampler density(seed, minecraft::Dimension::overworld);
            const int cell_width = density.cell_width(), cell_height = density.cell_height();
            const auto final_density = density.output_index("final_density");
            std::array<std::uint32_t, Filter::Sampled::axes.size()> climate_outputs;
            for (std::size_t i = 0; i < climate_outputs.size(); ++i)
                climate_outputs[i] = density.output_index(Filter::Sampled::axes[i]);
            constexpr std::array<std::string_view, 1> swamp{"minecraft:swamp"};
            std::array<minecraft::ChunkPos, tile_width * tile_width> chunks;
            std::array<std::int32_t, tile_width * tile_width> xs, zs;
            std::vector<std::uint8_t> proxy;
            for (;;) {
                if (cancelled.load(std::memory_order_relaxed))
                    break;
                const auto tile = next.fetch_add(1, std::memory_order_relaxed);
                if (tile >= total_tiles)
                    break;
                Result local;
                const int tile_x = (first_tile + static_cast<int>(tile / tile_side)) * tile_width;
                const int tile_z = (first_tile + static_cast<int>(tile % tile_side)) * tile_width;
                const int begin_x = std::max(tile_x, first), end_x = std::min(tile_x + tile_width - 1, last);
                const int begin_z = std::max(tile_z, first), end_z = std::min(tile_z + tile_width - 1, last);
                const bool skip_tile = !fast.tile_possible(tile_x, tile_z, tile_width, tile_width);
                if (skip_tile) {
                    local.tiles_skipped = 1;
                    local.regions = local.regions_skipped = std::uint64_t(end_x - begin_x + 1) * (end_z - begin_z + 1);
                }
                // The tile's candidates are gathered first so the proxy screen can batch them; they are then
                // checked in the same order as before.
                std::size_t count = 0;
                for (int rx = begin_x; !skip_tile && rx <= end_x; ++rx)
                    for (int rz = begin_z; rz <= end_z; ++rz) {
                        ++local.regions;
                        const auto chunk = candidate(seed, rx, rz);
                        if (!in_bounds(chunk, edge))
                            continue;
                        chunks[count] = chunk;
                        xs[count] = chunk.x * 16 + 8;
                        zs[count++] = chunk.z * 16 + 8;
                    }
                fast.screen(xs.data(), zs.data(), count, proxy);
                for (std::size_t index = 0; index < count; ++index) {
                    if (cancelled.load(std::memory_order_relaxed)) {
                        local.regions = 0;  // A partly checked tile does not count as scanned.
                        break;
                    }
                    const auto chunk = chunks[index];
                    ++local.candidates;
                    const int x = xs[index], z = zs[index];
                    if (proxy[index] == FastFilter::rejected) {
                        ++local.proxy_rejected;
                        continue;
                    }
                    if (proxy[index] == FastFilter::off_climate) {
                        ++local.filtered;
                        continue;
                    }
                    if (!filter.possible(x, z)) {
                        ++local.filtered;
                        continue;
                    }
                    const int incumbent = cutoff.load(std::memory_order_relaxed);
                    if (incumbent >= -63 && incumbent < 312) {
                        minecraft::worldgen::DensitySampler::ColumnScope scope(density, x, z);
                        // The climate filter just sampled the flat-cached climate at this quart.
                        const auto& sampled = filter.sampled();
                        for (std::size_t i = 0; i < sampled.axes.size(); ++i)
                            if (sampled.known >> i & 1U)
                                scope.remember(climate_outputs[i], sampled.position, sampled.values[i]);
                        const int probe_y =
                            minecraft::floor_div(incumbent + 8 + cell_height - 1, cell_height) * cell_height;
                        // shortcut: a high center column need not imply a high footprint average.
                        if (x % cell_width == 0 && z % cell_width == 0 &&
                            scope.direct(final_density, {x, probe_y, z}) > 0) {
                            ++local.direct_bounded;
                            continue;
                        }
                    }
                    const auto anchor = footprint(seed, chunk).floor_point(0);
                    if (anchor.x < -edge || anchor.x >= edge || anchor.z < -edge || anchor.z >= edge)
                        continue;
                    // Every check here only rejects, so the cheap probed preview may run before the full
                    // generation-height scan without changing which huts survive.
                    if (floors.preview_rejects(chunk, cutoff.load(std::memory_order_relaxed))) {
                        ++local.floor_preview_rejected;
                        continue;
                    }
                    ++local.heights;
                    const int y = terrain.height(x, z) - 1;
                    const auto match = biomes.matches(minecraft::to_quart(minecraft::BlockPos{x, y, z}), swamp);
                    if (match && !*match)
                        continue;
                    const auto inspected = floors.inspect(chunk, cutoff.load(std::memory_order_relaxed));
                    if (!inspected.floor) {
                        ++local.bounded;
                        continue;
                    }
                    const auto& floor = *inspected.floor;
                    if (!match || floor.uncertain)
                        ++local.unknown;
                    if (match && *match) {
                        const std::lock_guard lock(mutex);
                        auto& huts = result.huts;
                        if (huts.size() < top || floor.point.y < huts.back().point.y) {
                            const auto at = std::upper_bound(
                                huts.begin(), huts.end(), floor.point.y,
                                [](int floor_y, const Hut& hut) { return floor_y < hut.point.y; });
                            huts.insert(at, Hut{floor.point, minecraft::BlockPos{x, y, z}, floor.uncertain});
                            if (huts.size() > top)
                                huts.pop_back();
                            if (huts.size() == top)
                                cutoff.store(huts.back().point.y, std::memory_order_relaxed);
                        }
                    }
                }
                add(pending, local);
                // Most tiles finish in microseconds, so counters reach the shared result in batches.
                if (++pending_tiles == 64)
                    flush();
            }
            flush();
        } catch (...) {
            const std::lock_guard lock(mutex);
            if (!error)
                error = std::current_exception();
            cancelled.store(true, std::memory_order_relaxed);
        }
        running.fetch_sub(1, std::memory_order_release);
        finished.notify_one();
    };
    std::vector<std::jthread> threads;
    try {
        for (unsigned i = 0; i < workers; ++i)
            threads.emplace_back(worker);
        auto last_report = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            if (stop && stop())
                cancelled.store(true, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (progress && now - last_report >= std::chrono::milliseconds(200)) {
                progress(snapshot(), total);
                last_report = now;
            }
            std::unique_lock lock(mutex);
            finished.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return running.load(std::memory_order_acquire) == 0;
            });
        }
    } catch (...) {
        cancelled.store(true, std::memory_order_relaxed);
        throw;
    }
    for (auto& thread : threads)
        thread.join();
    if (error)
        std::rethrow_exception(error);
    result.scan_complete = result.regions == total;
    return result;
}
}  // namespace witch_hut
