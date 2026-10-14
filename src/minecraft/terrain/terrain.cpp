#include <minecraft/terrain.hpp>
#include <minecraft/random.hpp>

#include "../worldgen/density.hpp"
#include "veins.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace minecraft::terrain {
namespace {
constexpr std::string_view air = "minecraft:air", water = "minecraft:water", lava = "minecraft:lava";
double map(double v, double a, double b, double c, double d) {
    return c + (v - a) / (b - a) * (d - c);
}
double clamped_map(double v, double a, double b, double c, double d) {
    return c + std::clamp((v - a) / (b - a), 0.0, 1.0) * (d - c);
}
std::uint64_t pack(int x, int z) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U | static_cast<std::uint32_t>(z);
}

// Preliminary surfaces shared by every column of a sampler. Only used when the router proves they do not depend on
// the column they are evaluated from; the aquifer's skip height then depends on the chunk alone.
class SharedSurfaces {
public:
    std::optional<int> surface(std::uint64_t key) const {
        return find(surfaces_, key);
    }
    std::optional<int> skip_above(std::uint64_t chunk) const {
        return find(skip_above_, chunk);
    }
    void add_surface(std::uint64_t key, int value) {
        add(surfaces_, key, value);
    }
    void add_skip_above(std::uint64_t chunk, int value) {
        add(skip_above_, chunk, value);
    }

private:
    static constexpr std::size_t limit = 1U << 16U;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, int> surfaces_, skip_above_;

    std::optional<int> find(const std::unordered_map<std::uint64_t, int>& map, std::uint64_t key) const {
        const std::lock_guard lock(mutex_);
        const auto it = map.find(key);
        return it == map.end() ? std::nullopt : std::optional(it->second);
    }
    void add(std::unordered_map<std::uint64_t, int>& map, std::uint64_t key, int value) {
        const std::lock_guard lock(mutex_);
        if (map.size() >= limit)
            map.clear();
        map.emplace(key, value);
    }
};

struct Fluid {
    int level;
    std::string_view type;
    std::string_view at(int y) const {
        return y < level ? type : air;
    }
};

// What NoiseBasedAquifer caches: aquifer centers, their fluid status and preliminary surfaces. A chunk-generation
// NoiseChunk computes these the same way for every column of its chunk, so its columns share one state.
struct AquiferState {
    std::optional<int> skip_above;
    std::map<std::tuple<int, int, int>, BlockPos> centers;
    std::map<BlockPos, Fluid> fluids;
    std::map<std::pair<int, int>, int> surfaces;

    // The state of a chunk of a chunk-generation sampler on this thread. A few recent chunks are kept.
    static std::shared_ptr<AquiferState> of_chunk(const std::uint64_t sampler, const int chunk_x, const int chunk_z) {
        struct Slot {
            std::uint64_t sampler = 0, used = 0;
            int x = 0, z = 0;
            std::shared_ptr<AquiferState> state;
        };
        thread_local std::array<Slot, 4> slots;
        thread_local std::uint64_t clock = 0;
        ++clock;
        Slot* victim = &slots[0];
        for (auto& slot : slots) {
            if (slot.state && slot.sampler == sampler && slot.x == chunk_x && slot.z == chunk_z) {
                slot.used = clock;
                return slot.state;
            }
            if (slot.used < victim->used)
                victim = &slot;
        }
        *victim = {sampler, clock, chunk_x, chunk_z, std::make_shared<AquiferState>()};
        return victim->state;
    }
};

class ColumnAquifer {
public:
    // A chunk-generation column passes its chunk's shared state; a base column keeps its own.
    ColumnAquifer(const worldgen::DensitySampler& density, worldgen::DensitySampler::ColumnScope& scope,
                  SharedSurfaces* shared, std::shared_ptr<AquiferState> state = nullptr)
        : density_(density), scope_(scope), shared_(shared),
          state_(state ? std::move(state) : std::make_shared<AquiferState>()) {
        if (!density_.aquifers_enabled())
            return;
        if (state_->skip_above) {
            skip_above_ = *state_->skip_above;
            return;
        }
        const int chunk_x = floor_div(scope.x(), 16) * 16, chunk_z = floor_div(scope.z(), 16) * 16;
        const auto chunk = pack(chunk_x, chunk_z);
        if (const auto known = shared_ ? shared_->skip_above(chunk) : std::nullopt) {
            skip_above_ = *known;
            state_->skip_above = skip_above_;
            return;
        }
        const int min_x = floor_div(chunk_x - 5, 16), max_x = floor_div(chunk_x + 15 - 5, 16) + 1;
        const int min_z = floor_div(chunk_z - 5, 16), max_z = floor_div(chunk_z + 15 - 5, 16) + 1;
        int surface = std::numeric_limits<int>::min();
        for (int zz = min_z * 16; zz <= max_z * 16 + 9; zz += 4)
            for (int xx = min_x * 16; xx <= max_x * 16 + 9; xx += 4)
                surface = std::max(surface, preliminary(xx, zz));
        skip_above_ = (floor_div(surface + 8 + 12, 12) + 1) * 12 + 10;
        state_->skip_above = skip_above_;
        if (shared_)
            shared_->add_skip_above(chunk, skip_above_);
    }

    std::optional<std::string_view> substance(BlockPos p, double density) {
        if (density > 0)
            return std::nullopt;
        const auto global = global_fluid(p.y);
        if (!density_.aquifers_enabled() || p.y > skip_above_)
            return global.at(p.y);
        if (global.at(p.y) == lava)
            return lava;
        struct Nearby {
            int distance = std::numeric_limits<int>::max();
            BlockPos pos{};
        };
        std::array<Nearby, 3> nearest;
        const int gx = floor_div(p.x - 5, 16), gy = floor_div(p.y + 1, 12), gz = floor_div(p.z - 5, 16);
        for (int xx = 0; xx <= 1; ++xx)
            for (int yy = -1; yy <= 1; ++yy)
                for (int zz = 0; zz <= 1; ++zz) {
                    const auto pos = center(gx + xx, gy + yy, gz + zz);
                    const int dx = pos.x - p.x, dy = pos.y - p.y, dz = pos.z - p.z;
                    Nearby candidate{dx * dx + dy * dy + dz * dz, pos};
                    for (auto& n : nearest)
                        if (n.distance >= candidate.distance)
                            std::swap(n, candidate);
                }
        const auto first = fluid(nearest[0].pos);
        const auto block = first.at(p.y);
        const auto similarity = [](int a, int b) {
            return 1.0 - (b - a) / 25.0;
        };
        const double s01 = similarity(nearest[0].distance, nearest[1].distance);
        if (s01 <= 0)
            return block;
        if (block == water && global_fluid(p.y - 1).at(p.y - 1) == lava)
            return block;
        double barrier = std::numeric_limits<double>::quiet_NaN();
        const auto second = fluid(nearest[1].pos);
        if (density + s01 * pressure(p, barrier, first, second) > 0)
            return std::nullopt;
        const auto third = fluid(nearest[2].pos);
        const double s02 = similarity(nearest[0].distance, nearest[2].distance);
        if (s02 > 0 && density + s01 * s02 * pressure(p, barrier, first, third) > 0)
            return std::nullopt;
        const double s12 = similarity(nearest[1].distance, nearest[2].distance);
        if (s12 > 0 && density + s01 * s12 * pressure(p, barrier, second, third) > 0)
            return std::nullopt;
        return block;
    }

private:
    const worldgen::DensitySampler& density_;
    worldgen::DensitySampler::ColumnScope& scope_;
    SharedSurfaces* shared_;
    std::shared_ptr<AquiferState> state_;
    int skip_above_ = 0;

    double sample(std::string_view name, BlockPos p) const {
        return scope_.direct(name, p);
    }
    int preliminary(int x, int z) {
        const auto key = std::pair(floor_div(x, 4) * 4, floor_div(z, 4) * 4);
        auto [it, added] = state_->surfaces.try_emplace(key);
        if (!added)
            return it->second;
        const auto shared_key = pack(key.first, key.second);
        if (const auto known = shared_ ? shared_->surface(shared_key) : std::nullopt) {
            it->second = *known;
        } else {
            it->second = scope_.preliminary_surface(x, z);
            if (shared_)
                shared_->add_surface(shared_key, it->second);
        }
        return it->second;
    }
    Fluid global_fluid(int y) const {
        return y < std::min(-54, density_.sea_level()) ? Fluid{-54, lava}
                                                       : Fluid{density_.sea_level(), density_.default_fluid()};
    }
    BlockPos center(int x, int y, int z) {
        const auto key = std::tuple(x, y, z);
        auto [it, added] = state_->centers.try_emplace(key);
        if (added) {
            auto rng = density_.aquifer_random().at(x, y, z);
            const int dx = rng.next_int(10), dy = rng.next_int(9), dz = rng.next_int(10);
            it->second = {x * 16 + dx, y * 12 + dy, z * 16 + dz};
        }
        return it->second;
    }
    Fluid fluid(BlockPos p) {
        if (auto it = state_->fluids.find(p); it != state_->fluids.end())
            return it->second;
        const auto global = global_fluid(p.y);
        constexpr std::array<std::array<int, 2>, 13> offsets{{{0, 0},
                                                              {-2, -1},
                                                              {-1, -1},
                                                              {0, -1},
                                                              {1, -1},
                                                              {-3, 0},
                                                              {-2, 0},
                                                              {-1, 0},
                                                              {1, 0},
                                                              {-2, 1},
                                                              {-1, 1},
                                                              {0, 1},
                                                              {1, 1}}};
        int lowest = std::numeric_limits<int>::max();
        bool flooded_center = false;
        for (auto o : offsets) {
            const int x = p.x + o[0] * 16, z = p.z + o[1] * 16;
            const int surface = preliminary(x, z), adjusted = surface + 8;
            const bool center = o[0] == 0 && o[1] == 0;
            if (center && p.y - 12 > adjusted)
                return state_->fluids.emplace(p, global).first->second;
            const bool above = p.y + 12 > adjusted;
            if (above || center) {
                const auto f = global_fluid(adjusted);
                if (f.at(adjusted) != air) {
                    if (center)
                        flooded_center = true;
                    if (above)
                        return state_->fluids.emplace(p, f).first->second;
                }
            }
            lowest = std::min(lowest, surface);
        }
        double d0, d1;
        if (sample("erosion", p) < static_cast<double>(-.225F) && sample("depth", p) > static_cast<double>(.9F))
            d0 = d1 = -1;
        else {
            const double factor = flooded_center ? clamped_map(lowest + 8 - p.y, 0, 64, 1, 0) : 0;
            const double noise = std::clamp(sample("fluid_level_floodedness", p), -1.0, 1.0);
            d0 = noise - map(factor, 1, 0, -.8, .4);
            d1 = noise - map(factor, 1, 0, -.3, .8);
        }
        constexpr int way_below = -2032 * 16;
        int level = way_below;
        if (d1 > 0)
            level = global.level;
        else if (d0 > 0) {
            const int gx = floor_div(p.x, 16), gy = floor_div(p.y, 40), gz = floor_div(p.z, 16);
            const double spread = sample("fluid_level_spread", {gx, gy, gz}) * 10;
            level = std::min(lowest, gy * 40 + 20 + static_cast<int>(std::floor(spread / 3)) * 3);
        }
        auto type = global.type;
        if (level <= -10 && level != way_below && type != lava &&
            std::abs(sample("lava", {floor_div(p.x, 64), floor_div(p.y, 40), floor_div(p.z, 64)})) > .3)
            type = lava;
        return state_->fluids.emplace(p, Fluid{level, type}).first->second;
    }
    double pressure(BlockPos p, double& barrier, Fluid a, Fluid b) const {
        const auto aa = a.at(p.y), bb = b.at(p.y);
        if ((aa == lava && bb == water) || (aa == water && bb == lava))
            return 2;
        const int delta = std::abs(a.level - b.level);
        if (delta == 0)
            return 0;
        const double mid = .5 * (a.level + b.level), rel = p.y + .5 - mid, overlap = delta / 2.0 - std::abs(rel);
        const double pressure = rel > 0 ? (overlap > 0 ? overlap / 1.5 : overlap / 2.5)
                                        : (3 + overlap > 0 ? (3 + overlap) / 3 : (3 + overlap) / 10);
        double noise = 0;
        if (pressure >= -2 && pressure <= 2) {
            if (std::isnan(barrier))
                barrier = sample("barrier", p);
            noise = barrier;
        }
        return 2 * (noise + pressure);
    }
};

std::optional<std::string_view> ore(const worldgen::DensitySampler& density,
                                    worldgen::DensitySampler::ColumnScope& scope, BlockPos p) {
    if (const auto block = vein_block(density, scope, p))
        return vein_block_id(*block);
    return std::nullopt;
}
}  // namespace

struct Sampler::Impl {
    worldgen::DensitySampler density;
    std::unique_ptr<SharedSurfaces> surfaces;
    ColumnSource source;
    std::uint64_t id;  // Unique for the process lifetime, unlike the address.
    Impl(std::uint64_t seed, Dimension d, ColumnSource source) : density(seed, d), source(source), id(next_id()) {
        if (density.aquifers_enabled() && density.preliminary_surface_column_free())
            surfaces = std::make_unique<SharedSurfaces>();
    }
    [[nodiscard]] worldgen::DensitySampler::ColumnScope scope(int x, int z) const {
        return {density, x, z,
                source == ColumnSource::chunk_generation ? worldgen::NoiseScope::chunk : worldgen::NoiseScope::cell};
    }
    // Columns of one chunk-generation chunk share aquifer state when nothing in it depends on the column.
    [[nodiscard]] ColumnAquifer aquifer(worldgen::DensitySampler::ColumnScope& scope) const {
        std::shared_ptr<AquiferState> state;
        if (source == ColumnSource::chunk_generation && surfaces)
            state = AquiferState::of_chunk(id, floor_div(scope.x(), 16), floor_div(scope.z(), 16));
        return {density, scope, surfaces.get(), std::move(state)};
    }

private:
    static std::uint64_t next_id() {
        static std::atomic<std::uint64_t> counter{0};
        return ++counter;
    }
};
Sampler::Sampler(std::uint64_t seed, Dimension d, ColumnSource source)
    : impl_(std::make_unique<Impl>(seed, d, source)) {}
Sampler::~Sampler() = default;
Sampler::Sampler(Sampler&&) noexcept = default;
Sampler& Sampler::operator=(Sampler&&) noexcept = default;
Column Sampler::column(int x, int z) const {
    const auto& density = impl_->density;
    Column result{std::vector<std::string_view>(density.height())};
    auto scope = impl_->scope(x, z);
    std::vector<double> values(static_cast<std::size_t>(density.height()));
    scope.visit("final_density", true, [&](BlockPos p, double value) {
        values[static_cast<std::size_t>(p.y - density.min_y())] = value;
        return true;
    });
    auto aquifer = impl_->aquifer(scope);
    for (int i = density.height() - 1; i >= 0; --i) {
        const BlockPos p{x, density.min_y() + i, z};
        const double d = values[static_cast<std::size_t>(i)];
        auto block = aquifer.substance(p, d);
        if (!block)
            block = ore(density, scope, p);
        result.blocks[static_cast<std::size_t>(i)] = block.value_or(density.default_block());
    }
    return result;
}
void Sampler::blocks(int x, int z, int min_y, int max_y, std::span<std::string_view> out) const {
    const auto& density = impl_->density;
    auto scope = impl_->scope(x, z);
    auto aquifer = impl_->aquifer(scope);
    for (int y = min_y; y <= max_y; ++y) {
        const BlockPos p{x, y, z};
        auto block = aquifer.substance(p, scope.column("final_density", y, true));
        if (!block)
            block = ore(density, scope, p);
        out[static_cast<std::size_t>(y - min_y)] = block.value_or(density.default_block());
    }
}
int Sampler::height(int x, int z) const {
    const auto& density = impl_->density;
    auto scope = impl_->scope(x, z);
    auto aquifer = impl_->aquifer(scope);
    int result = density.min_y();
    scope.visit("final_density", true, [&](BlockPos p, double value) {
        // Ore veins only replace solid blocks, so they cannot change either heightmap.
        const auto block = aquifer.substance(p, value).value_or(density.default_block());
        if (block != air) {
            result = p.y + 1;
            return false;
        }
        return true;
    });
    return result;
}
struct Sampler::AquiferProbe::State {
    worldgen::DensitySampler::ColumnScope scope;
    ColumnAquifer aquifer;
    int x, z;
    State(const Sampler::Impl& impl, int x, int z)
        : scope(impl.scope(x, z)), aquifer(impl.aquifer(scope)), x(x), z(z) {}
};
Sampler::AquiferProbe::AquiferProbe(const Sampler& sampler, int x, int z)
    : state_(std::make_unique<State>(*sampler.impl_, x, z)) {}
Sampler::AquiferProbe::~AquiferProbe() = default;
Sampler::AquiferProbe::AquiferProbe(AquiferProbe&&) noexcept = default;
Sampler::AquiferProbe& Sampler::AquiferProbe::operator=(AquiferProbe&&) noexcept = default;
std::optional<std::string_view> Sampler::AquiferProbe::carve(int y) {
    return state_->aquifer.substance({state_->x, y, state_->z}, 0.0);
}
}  // namespace minecraft::terrain
