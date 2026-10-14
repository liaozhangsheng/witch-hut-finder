#include "world.hpp"

#include "../worldgen/rules.hpp"
#include "carver.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace minecraft::feature {
namespace {

constexpr std::int32_t unknown = std::numeric_limits<std::int32_t>::min();
constexpr std::uint16_t unfilled = 0xffffU;
std::uint64_t chunk_key(std::int32_t x, std::int32_t z) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U | static_cast<std::uint32_t>(z);
}

random::PositionalFactory state_random(std::uint64_t seed) {
    return random::RandomSource(seed, random::RandomAlgorithm::xoroshiro).fork_positional();
}

bool special_biome(const std::optional<std::string_view>& biome) {
    return !biome || *biome == "minecraft:eroded_badlands" || *biome == "minecraft:frozen_ocean" ||
           *biome == "minecraft:deep_frozen_ocean";
}

}  // namespace

struct BaseTerrain::Chunk {
    ChunkPos position;
    std::vector<std::uint16_t> blocks = std::vector<std::uint16_t>(256U * height, unfilled);
    std::array<std::uint32_t, 256> sections{};
    std::array<Column, 256> columns;
    std::unique_ptr<SurfaceChunk> surface;
    bool carved = false;
    // Carving writes made before the surface chunk existed, by local index; replayed into it when built.
    std::unordered_map<std::uint32_t, BlockId> carve_overlay;
    std::vector<std::pair<std::uint32_t, BlockId>> carve_log;
    explicit Chunk(ChunkPos position) : position(position) {
        columns.fill(Column{unknown, unknown, unknown, unknown, -1});
    }
    static std::uint32_t local(BlockPos p) {
        return static_cast<std::uint32_t>(index(p.x, p.z) * height + static_cast<std::size_t>(p.y - min_y));
    }
    static std::size_t index(std::int32_t x, std::int32_t z) {
        return static_cast<std::size_t>((x & 15) * 16 + (z & 15));
    }
};

bool BaseTerrain::Gradient::test(std::int32_t x, std::int32_t y, std::int32_t z) const {
    if (y <= true_at_and_below) return true;
    if (y >= false_at_and_above) return false;
    // Mth.map(y, true, false, 1.0, 0.0) is lerp(inverseLerp(...), 1, 0).
    const double t = (static_cast<double>(y) - true_at_and_below) / (static_cast<double>(false_at_and_above) - true_at_and_below);
    auto rng = random.at(x, y, z);
    return static_cast<double>(rng.next_float()) < 1.0 + t * (0.0 - 1.0);
}

BaseTerrain::BaseTerrain(std::uint64_t seed, std::size_t max_chunks)
    : seed_(seed), max_chunks_(std::max<std::size_t>(max_chunks, 16)),
      terrain_(seed, Dimension::overworld, terrain::ColumnSource::chunk_generation),
      biomes_(seed, Dimension::overworld), surface_builder_(seed) {
    // The overworld rule is a sequence: vertical gradients ahead of above_preliminary_surface apply everywhere and
    // later ones apply below the minimum surface level, where the surface sequence cannot claim blocks.
    const auto& settings = worldgen::field(worldgen::field(worldgen::rules(), "noise_settings"), "minecraft:overworld");
    const auto& rule = worldgen::field(settings, "surface_rule");
    if (worldgen::string(worldgen::field(rule, "type")) != "minecraft:sequence")
        throw std::runtime_error("unsupported overworld surface rule layout");
    bool surface_seen = false;
    const auto anchor = [](const minecraft::worldgen::Json& value) {
        const auto& object = std::get<minecraft::worldgen::JsonObject>(value);
        if (const auto it = object.find("absolute"); it != object.end())
            return static_cast<std::int32_t>(worldgen::number(it->second));
        if (const auto it = object.find("above_bottom"); it != object.end())
            return min_y + static_cast<std::int32_t>(worldgen::number(it->second));
        throw std::runtime_error("unsupported surface gradient anchor");
    };
    for (const auto& item : std::get<minecraft::worldgen::JsonArray>(worldgen::field(rule, "sequence"))) {
        const auto& condition = worldgen::field(item, "if_true");
        const auto type = worldgen::string(worldgen::field(condition, "type"));
        if (type == "minecraft:above_preliminary_surface") {
            surface_seen = true;
            continue;
        }
        const auto& result = worldgen::field(item, "then_run");
        if (type != "minecraft:vertical_gradient" || worldgen::string(worldgen::field(result, "type")) != "minecraft:block")
            throw std::runtime_error("unsupported overworld surface rule layout");
        Gradient gradient{state_random(seed).from_hash_of(worldgen::string(worldgen::field(condition, "random_name"))).fork_positional(),
                          anchor(worldgen::field(condition, "true_at_and_below")),
                          anchor(worldgen::field(condition, "false_at_and_above")),
                          Catalog::get().block(worldgen::string(worldgen::field(worldgen::field(result, "result_state"), "Name")))};
        (surface_seen ? deep_ : below_surface_).push_back(std::move(gradient));
    }
    if (!surface_seen) throw std::runtime_error("overworld surface rule has no above_preliminary_surface branch");
}

BaseTerrain::~BaseTerrain() = default;

BaseTerrain::Chunk& BaseTerrain::chunk(std::int32_t chunk_x, std::int32_t chunk_z) {
    const auto key = chunk_key(chunk_x, chunk_z);
    if (const auto it = chunks_.find(key); it != chunks_.end()) return *it->second;
    if (chunks_.size() >= max_chunks_) {
        chunks_.erase(order_.front());
        order_.pop_front();
    }
    order_.push_back(key);
    return *chunks_.emplace(key, std::make_unique<Chunk>(ChunkPos{chunk_x, chunk_z})).first->second;
}

BaseTerrain::Column& BaseTerrain::column(Chunk& c, std::int32_t x, std::int32_t z) {
    auto& column = c.columns[Chunk::index(x, z)];
    if (column.min_surface == unknown) column.min_surface = surface_builder_.min_surface_level(x, z);
    return column;
}

const SurfaceChunk& BaseTerrain::surface(Chunk& c) {
    if (!c.surface) {
        c.surface = std::make_unique<SurfaceChunk>(surface_builder_.build(c.position));
        // Logged carving writes left both heightmaps alone, so writing the blocks alone replays them.
        for (const auto& [index, block] : c.carve_log) c.surface->blocks[index] = block;
        c.carve_log.clear();
        c.carve_overlay.clear();
    }
    return *c.surface;
}

// The CARVERS step's view of one chunk.
class BaseTerrain::Target final : public CarveTarget {
public:
    Target(BaseTerrain& terrain, Chunk& chunk) : terrain_(terrain), chunk_(chunk) {}

    BlockId get(BlockPos p) override { return terrain_.read(chunk_, p).block; }

    void set(BlockPos p, BlockId block) override {
        if (!chunk_.surface) {
            // Heightmap.update ignores writes two or more below the first available height, which the lower
            // bounds prove for most underground carving without building the surface.
            auto& col = terrain_.column(chunk_, p.x, p.z);
            if (p.y <= std::min(terrain_.ocean_floor_lower_bound(p.x, p.z), lower_surface(col, p)) - 2) {
                chunk_.carve_overlay[Chunk::local(p)] = block;
                chunk_.carve_log.emplace_back(Chunk::local(p), block);
                return;
            }
            (void)terrain_.surface(chunk_);
        }
        chunk_.surface->set(p.x, p.y, p.z, block);
    }

    std::optional<BlockId> carved_substance(BlockPos p) override {
        terrain::Sampler::AquiferProbe probe(terrain_.terrain_, p.x, p.z);
        const auto substance = probe.carve(p.y);
        if (!substance) return std::nullopt;
        return Catalog::get().block(*substance);
    }

    std::optional<BlockId> top_material(BlockPos p, bool under_fluid) override {
        return terrain_.surface_builder_.top_material(terrain_.surface(chunk_), p, under_fluid);
    }

private:
    std::int32_t lower_surface(Column& col, BlockPos p) {
        if (col.lower_surface == unknown) {
            // Like the ocean floor bound, for WORLD_SURFACE_WG: the highest non-air block below the surface's reach.
            col.lower_surface = min_y;
            const auto& catalog = Catalog::get();
            for (auto y = std::min(col.min_surface, min_y + height) - 1; y >= min_y; --y)
                if (!catalog.info(terrain_.read(chunk_, {p.x, y, p.z}).block).air) {
                    col.lower_surface = y + 1;
                    break;
                }
        }
        return col.lower_surface;
    }

    BaseTerrain& terrain_;
    Chunk& chunk_;
};

BaseTerrain::Chunk& BaseTerrain::carved(std::int32_t chunk_x, std::int32_t chunk_z) {
    auto& c = chunk(chunk_x, chunk_z);
    if (!c.carved) {
        c.carved = true;
        Target target(*this, c);
        carve_chunk(seed_, c.position, biomes_, target);
    }
    return c;
}

void BaseTerrain::fill_section(Chunk& c, std::int32_t x, std::int32_t z, std::int32_t section) {
    const auto& catalog = Catalog::get();
    std::array<std::string_view, 16> ids{};
    const auto y0 = min_y + section * 16;
    terrain_.blocks(x, z, y0, y0 + 15, ids);
    const auto index = Chunk::index(x, z);
    for (std::int32_t i = 0; i < 16; ++i) {
        const auto y = y0 + i;
        auto block = catalog.block(ids[static_cast<std::size_t>(i)]);
        // Below the minimum surface level the overworld rule is its gradients alone.
        if (block == catalog.stone) {
            bool resolved = false;
            for (const auto& gradient : below_surface_)
                if (gradient.test(x, y, z)) {
                    block = gradient.block;
                    resolved = true;
                    break;
                }
            if (!resolved)
                for (const auto& gradient : deep_)
                    if (gradient.test(x, y, z)) {
                        block = gradient.block;
                        break;
                    }
        }
        c.blocks[index * height + static_cast<std::size_t>(y - min_y)] = block;
    }
    c.sections[index] |= 1U << static_cast<unsigned>(section);
}

BlockId BaseTerrain::fast_block(Chunk& c, std::int32_t x, std::int32_t y, std::int32_t z) {
    const auto index = Chunk::index(x, z);
    const auto section = (y - min_y) >> 4;
    if (!(c.sections[index] & (1U << static_cast<unsigned>(section)))) fill_section(c, x, z, section);
    return c.blocks[index * height + static_cast<std::size_t>(y - min_y)];
}

Cell BaseTerrain::at(BlockPos p) {
    const auto& catalog = Catalog::get();
    if (p.y < min_y || p.y >= min_y + height) return {catalog.void_air, false};
    return read(carved(p.x >> 4, p.z >> 4), p);
}

Cell BaseTerrain::read(Chunk& c, BlockPos p) {
    const auto& catalog = Catalog::get();
    if (p.y < min_y || p.y >= min_y + height) return {catalog.void_air, false};
    const auto surface_cell = [&] {
        const auto& s = surface(c);
        return Cell{s.at(p.x, p.y, p.z), p.y >= s.uncertain_from[Chunk::index(p.x, p.z)]};
    };
    if (c.surface) return surface_cell();
    if (const auto it = c.carve_overlay.find(Chunk::local(p)); it != c.carve_overlay.end()) return {it->second, false};
    auto& col = column(c, p.x, p.z);
    const auto needs = [&] {
        if (col.top == unknown) col.top = terrain_.height(p.x, p.z);
        if (col.special < 0)
            col.special = special_biome(biome_memo_.noise(biomes_.stored_block_quart({p.x, col.top, p.z}))) ? 1 : 0;
    };
    if (p.y < col.min_surface) {
        // Surface rules reach no lower; only eroded badlands pillars, which fill the open air above the top.
        const auto block = fast_block(c, p.x, p.y, p.z);
        if (!catalog.info(block).air) return {block, false};
        needs();
        if (p.y < col.top || col.special == 0) return {block, false};
        return surface_cell();
    }
    needs();
    // Rules only replace default stone, and the extensions only touch special columns.
    if (col.special == 0 && p.y >= col.top) return {fast_block(c, p.x, p.y, p.z), false};
    return surface_cell();
}

std::int32_t BaseTerrain::ocean_floor_lower_bound(std::int32_t x, std::int32_t z) {
    // Carving keeps this bound: before the surface exists it only writes two or more below it.
    auto& c = carved(x >> 4, z >> 4);
    if (c.surface) {
        const auto index = Chunk::index(x, z);
        return c.surface->uncertain_from[index] == std::numeric_limits<std::int32_t>::max()
                   ? c.surface->ocean_floor[index]
                   : c.surface->ocean_floor_min[index];
    }
    auto& col = column(c, x, z);
    if (col.lower_floor == unknown) {
        // Blocks below the minimum surface level are exact, and later steps of SURFACE only add blocks there.
        col.lower_floor = min_y;
        const auto& catalog = Catalog::get();
        for (auto y = std::min(col.min_surface, min_y + height) - 1; y >= min_y; --y)
            if (catalog.info(read(c, {x, y, z}).block).blocks_motion) {
                col.lower_floor = y + 1;
                break;
            }
    }
    return col.lower_floor;
}

}  // namespace minecraft::feature
