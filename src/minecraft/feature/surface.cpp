#include "surface.hpp"
#include "biome_memo.hpp"

#include <minecraft/random.hpp>

#include "../support/java_math.hpp"
#include "../worldgen/density.hpp"
#include "../worldgen/noise.hpp"
#include "../worldgen/rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace minecraft::feature {
namespace {

using Json = minecraft::worldgen::Json;
using worldgen::field;
using worldgen::number;
using worldgen::string;

constexpr std::int32_t min_y = SurfaceChunk::min_y, height = SurfaceChunk::height;
constexpr std::int32_t way_below_min_y = -2032 << 4;  // DimensionType.WAY_BELOW_MIN_Y
constexpr std::int32_t no_water = std::numeric_limits<std::int32_t>::min();

const Json* optional_field(const Json& value, std::string_view key) {
    if (!std::holds_alternative<minecraft::worldgen::JsonObject>(value)) return nullptr;
    const auto& object = std::get<minecraft::worldgen::JsonObject>(value);
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &it->second;
}

random::PositionalFactory state_random(std::uint64_t seed) {
    return random::RandomSource(seed, random::RandomAlgorithm::xoroshiro).fork_positional();
}

worldgen::NoiseParameters noise_parameters(std::string_view id) {
    const auto& value = field(field(worldgen::rules(), "noise_parameters"), id);
    worldgen::NoiseParameters parameters{static_cast<int>(number(field(value, "firstOctave"))), {}};
    for (const auto& amplitude : std::get<minecraft::worldgen::JsonArray>(field(value, "amplitudes")))
        parameters.amplitudes.push_back(number(amplitude));
    return parameters;
}

// PerlinSimplexNoise over non-positive octaves, as Biome's temperature noises use it.
class PerlinSimplex {
public:
    PerlinSimplex(std::uint64_t seed, std::vector<int> octaves) {
        std::sort(octaves.begin(), octaves.end());
        const int lowest = -octaves.front(), highest = octaves.back();
        if (highest > 0) throw std::runtime_error("positive simplex octaves are unsupported");
        const int count = lowest + highest + 1;
        random::LegacyRandom random(seed);
        const auto contains = [&](int octave) { return std::binary_search(octaves.begin(), octaves.end(), octave); };
        levels_.resize(static_cast<std::size_t>(count));
        worldgen::SimplexNoise first(random);
        if (highest >= 0 && highest < count && contains(0)) levels_[static_cast<std::size_t>(highest)].emplace(first);
        for (int i = highest + 1; i < count; ++i) {
            if (i >= 0 && contains(highest - i)) levels_[static_cast<std::size_t>(i)].emplace(random);
            else random.consume_count(262);
        }
        input_factor_ = std::pow(2.0, highest);
        value_factor_ = 1.0 / (std::pow(2.0, count) - 1.0);
    }
    [[nodiscard]] double value(double x, double y) const {
        double total = 0.0, input = input_factor_, scale = value_factor_;
        for (const auto& level : levels_) {
            if (level) total += level->sample2(x * input, y * input) * scale;
            input /= 2.0;
            scale *= 2.0;
        }
        return total;
    }

private:
    std::vector<std::optional<worldgen::SimplexNoise>> levels_;
    double input_factor_ = 1.0, value_factor_ = 1.0;
};

struct Condition {
    enum class Kind {
        biome, noise_threshold, vertical_gradient, y_above, water, temperature, steep, invert, hole,
        above_preliminary_surface, stone_depth
    } kind;
    std::vector<std::string_view> biomes;  // sorted
    std::size_t noise = 0;
    double min_threshold = 0, max_threshold = 0;
    std::optional<random::PositionalFactory> gradient;
    std::int32_t true_at = 0, false_at = 0;
    VerticalAnchor anchor;
    std::int32_t multiplier = 0, offset = 0, secondary_range = 0;
    bool add_stone_depth = false, add_surface_depth = false, ceiling = false;
    std::unique_ptr<Condition> target;
};

struct Rule {
    enum class Kind { block, sequence, condition, bandlands } kind;
    BlockId block = 0;
    std::vector<Rule> sequence;
    std::unique_ptr<Condition> condition;
    std::unique_ptr<Rule> then;
};

std::int32_t resolve(const VerticalAnchor& anchor) {
    return anchor.resolve(min_y, height);
}

}  // namespace

struct SurfaceBuilder::Impl {
    std::uint64_t seed;
    terrain::Sampler terrain;
    worldgen::DensitySampler density;
    biome::Sampler biomes;
    random::PositionalFactory noise_random;
    std::vector<std::string_view> noise_ids;
    std::vector<std::unique_ptr<worldgen::NormalNoise>> noises;
    std::size_t surface_noise, secondary_noise, band_offset_noise, badlands_surface, badlands_pillar, badlands_roof,
        iceberg_surface, iceberg_pillar, iceberg_roof;
    std::vector<BlockId> clay_bands;
    Rule rule;
    PerlinSimplex temperature_noise{1234, {0}}, frozen_noise{3456, {-2, -1, 0}}, info_noise{2345, {0}};
    std::unordered_map<std::string_view, std::pair<float, bool>> climate;  // base temperature, frozen modifier
    std::int32_t sea_level;
    bool rules_keep_floor = false;
    // Preliminary surface levels at surface-cell corners and noise biomes; a builder serves one thread.
    mutable std::unordered_map<std::uint64_t, std::int32_t> preliminary;
    mutable BiomeMemo biome_memo{biomes};

    explicit Impl(std::uint64_t seed)
        : seed(seed), terrain(seed, Dimension::overworld, terrain::ColumnSource::chunk_generation),
          density(seed, Dimension::overworld),
          biomes(seed, Dimension::overworld), noise_random(state_random(seed)), sea_level(density.sea_level()) {
        surface_noise = noise("minecraft:surface");
        secondary_noise = noise("minecraft:surface_secondary");
        band_offset_noise = noise("minecraft:clay_bands_offset");
        badlands_surface = noise("minecraft:badlands_surface");
        badlands_pillar = noise("minecraft:badlands_pillar");
        badlands_roof = noise("minecraft:badlands_pillar_roof");
        iceberg_surface = noise("minecraft:iceberg_surface");
        iceberg_pillar = noise("minecraft:iceberg_pillar");
        iceberg_roof = noise("minecraft:iceberg_pillar_roof");
        const auto& catalog = Catalog::get();
        for (const auto& [id, value] : std::get<minecraft::worldgen::JsonObject>(field(features_root(), "biome_climate")))
            climate.emplace(id, std::pair{static_cast<float>(number(field(value, "temperature"))),
                                          string(field(value, "temperature_modifier")) == "frozen"});
        generate_bands(catalog);
        const auto& settings = field(field(worldgen::rules(), "noise_settings"), "minecraft:overworld");
        rule = compile_rule(field(settings, "surface_rule"));
        rules_keep_floor = std::all_of(clay_bands.begin(), clay_bands.end(),
                                       [&](BlockId b) { return catalog.info(b).blocks_motion; }) &&
                           outputs_block_motion(rule, catalog);
    }

    // Whether every block the rule can produce blocks motion. Rules only replace stone, so then no choice among
    // them moves the OCEAN_FLOOR_WG height.
    static bool outputs_block_motion(const Rule& r, const Catalog& catalog) {
        switch (r.kind) {
            case Rule::Kind::block: return catalog.info(r.block).blocks_motion;
            case Rule::Kind::bandlands: return true;  // clay_bands, checked separately
            case Rule::Kind::condition: return outputs_block_motion(*r.then, catalog);
            case Rule::Kind::sequence:
                return std::all_of(r.sequence.begin(), r.sequence.end(),
                                   [&](const Rule& item) { return outputs_block_motion(item, catalog); });
        }
        return false;
    }

    // The highest OCEAN_FLOOR_WG height the eroded badlands pillar or the frozen ocean iceberg could give the column,
    // whichever biome a surface climate tie hides: both only add blocks, up to a top fixed by noise alone.
    [[nodiscard]] std::int32_t extension_ceiling(std::int32_t x, std::int32_t z) const {
        std::int32_t ceiling = std::numeric_limits<std::int32_t>::min();
        const double pillar = std::min(std::abs(noises[badlands_surface]->sample(x, 0.0, z) * 8.25),
                                       noises[badlands_pillar]->sample(x * 0.2, 0.0, z * 0.2) * 15.0);
        if (!(pillar <= 0.0)) {
            const double roof = std::abs(noises[badlands_roof]->sample(x * 0.75, 0.0, z * 0.75) * 1.5);
            ceiling = support::java_floor(64.0 + std::min(pillar * pillar * 2.5, std::ceil(roof * 50.0) + 24.0)) + 1;
        }
        const double iceberg = std::min(std::abs(noises[iceberg_surface]->sample(x, 0.0, z) * 8.25),
                                        noises[iceberg_pillar]->sample(x * 1.28, 0.0, z * 1.28) * 15.0);
        if (!(iceberg <= 1.8)) {
            const double roof = std::abs(noises[iceberg_roof]->sample(x * 1.17, 0.0, z * 1.17) * 1.5);
            // Without the warm-biome reduction of two, which only lowers it.
            const double height = std::min(iceberg * iceberg * 1.2, std::ceil(roof * 40.0) + 14.0);
            if (height > 2.0) ceiling = std::max(ceiling, support::java_d2i(height + sea_level));
        }
        return ceiling;
    }

    static const Json& features_root();

    std::size_t noise(std::string_view id) {
        const auto it = std::find(noise_ids.begin(), noise_ids.end(), id);
        if (it != noise_ids.end()) return static_cast<std::size_t>(it - noise_ids.begin());
        noise_ids.push_back(id);
        noises.push_back(std::make_unique<worldgen::NormalNoise>(state_random(seed).from_hash_of(id), noise_parameters(id)));
        return noises.size() - 1;
    }

    // SurfaceSystem.generateBands.
    void generate_bands(const Catalog& catalog) {
        const auto terracotta = catalog.block("minecraft:terracotta");
        clay_bands.assign(192, terracotta);
        auto random = noise_random.from_hash_of("minecraft:clay_bands");
        const auto between = [&](std::int32_t min, std::int32_t max) { return random.next_int(max - min + 1) + min; };
        for (std::int32_t k = 0; k < 192; ++k) {
            k += random.next_int(5) + 1;
            if (k < 192) clay_bands[static_cast<std::size_t>(k)] = catalog.block("minecraft:orange_terracotta");
        }
        const auto make_bands = [&](std::int32_t base_width, BlockId state) {
            const auto count = between(6, 15);
            for (std::int32_t j = 0; j < count; ++j) {
                const auto width = base_width + random.next_int(3);
                const auto start = random.next_int(192);
                for (std::int32_t i = 0; start + i < 192 && i < width; ++i) clay_bands[static_cast<std::size_t>(start + i)] = state;
            }
        };
        make_bands(1, catalog.block("minecraft:yellow_terracotta"));
        make_bands(2, catalog.block("minecraft:brown_terracotta"));
        make_bands(1, catalog.block("minecraft:red_terracotta"));
        const auto whites = between(9, 15);
        const auto white = catalog.block("minecraft:white_terracotta");
        const auto light_gray = catalog.block("minecraft:light_gray_terracotta");
        std::int32_t placed = 0;
        for (std::int32_t j = 0; placed < whites && j < 192; j += random.next_int(16) + 4) {
            clay_bands[static_cast<std::size_t>(j)] = white;
            if (j - 1 > 0 && random.next_boolean()) clay_bands[static_cast<std::size_t>(j - 1)] = light_gray;
            if (j + 1 < 192 && random.next_boolean()) clay_bands[static_cast<std::size_t>(j + 1)] = light_gray;
            ++placed;
        }
    }

    std::unique_ptr<Condition> compile_condition(const Json& value) {
        auto c = std::make_unique<Condition>();
        const auto type = string(field(value, "type"));
        if (type == "minecraft:biome") {
            c->kind = Condition::Kind::biome;
            for (const auto& id : std::get<minecraft::worldgen::JsonArray>(field(value, "biome_is"))) c->biomes.push_back(string(id));
            std::sort(c->biomes.begin(), c->biomes.end());
        } else if (type == "minecraft:noise_threshold") {
            c->kind = Condition::Kind::noise_threshold;
            c->noise = noise(string(field(value, "noise")));
            c->min_threshold = number(field(value, "min_threshold"));
            c->max_threshold = number(field(value, "max_threshold"));
        } else if (type == "minecraft:vertical_gradient") {
            c->kind = Condition::Kind::vertical_gradient;
            c->gradient = state_random(seed).from_hash_of(string(field(value, "random_name"))).fork_positional();
            c->true_at = resolve(anchor(field(value, "true_at_and_below")));
            c->false_at = resolve(anchor(field(value, "false_at_and_above")));
        } else if (type == "minecraft:y_above") {
            c->kind = Condition::Kind::y_above;
            c->anchor = anchor(field(value, "anchor"));
            c->multiplier = static_cast<std::int32_t>(number(field(value, "surface_depth_multiplier")));
            c->add_stone_depth = std::get<bool>(field(value, "add_stone_depth"));
        } else if (type == "minecraft:water") {
            c->kind = Condition::Kind::water;
            c->offset = static_cast<std::int32_t>(number(field(value, "offset")));
            c->multiplier = static_cast<std::int32_t>(number(field(value, "surface_depth_multiplier")));
            c->add_stone_depth = std::get<bool>(field(value, "add_stone_depth"));
        } else if (type == "minecraft:temperature") {
            c->kind = Condition::Kind::temperature;
        } else if (type == "minecraft:steep") {
            c->kind = Condition::Kind::steep;
        } else if (type == "minecraft:not") {
            c->kind = Condition::Kind::invert;
            c->target = compile_condition(field(value, "invert"));
        } else if (type == "minecraft:hole") {
            c->kind = Condition::Kind::hole;
        } else if (type == "minecraft:above_preliminary_surface") {
            c->kind = Condition::Kind::above_preliminary_surface;
        } else if (type == "minecraft:stone_depth") {
            c->kind = Condition::Kind::stone_depth;
            c->offset = static_cast<std::int32_t>(number(field(value, "offset")));
            c->add_surface_depth = std::get<bool>(field(value, "add_surface_depth"));
            c->secondary_range = static_cast<std::int32_t>(number(field(value, "secondary_depth_range")));
            c->ceiling = string(field(value, "surface_type")) == "ceiling";
        } else {
            throw std::runtime_error("unsupported surface condition " + std::string(type));
        }
        return c;
    }

    static VerticalAnchor anchor(const Json& value) {
        if (const auto* v = optional_field(value, "absolute")) return {VerticalAnchor::Kind::absolute, static_cast<std::int32_t>(number(*v))};
        if (const auto* v = optional_field(value, "above_bottom"))
            return {VerticalAnchor::Kind::above_bottom, static_cast<std::int32_t>(number(*v))};
        if (const auto* v = optional_field(value, "below_top")) return {VerticalAnchor::Kind::below_top, static_cast<std::int32_t>(number(*v))};
        throw std::runtime_error("unsupported surface anchor");
    }

    Rule compile_rule(const Json& value) {
        Rule r;
        const auto type = string(field(value, "type"));
        if (type == "minecraft:block") {
            r.kind = Rule::Kind::block;
            r.block = Catalog::get().block(string(field(field(value, "result_state"), "Name")));
        } else if (type == "minecraft:sequence") {
            r.kind = Rule::Kind::sequence;
            for (const auto& item : std::get<minecraft::worldgen::JsonArray>(field(value, "sequence"))) r.sequence.push_back(compile_rule(item));
        } else if (type == "minecraft:condition") {
            r.kind = Rule::Kind::condition;
            r.condition = compile_condition(field(value, "if_true"));
            r.then = std::make_unique<Rule>(compile_rule(field(value, "then_run")));
        } else if (type == "minecraft:bandlands") {
            r.kind = Rule::Kind::bandlands;
        } else {
            throw std::runtime_error("unsupported surface rule " + std::string(type));
        }
        return r;
    }

    std::optional<std::string_view> block_biome(BlockPos p, bool clamp = true) const {
        return biome_memo.noise(clamp ? biomes.stored_block_quart(p) : biomes.block_quart(p));
    }

    // Biome.getTemperature, including the frozen modifier and the height adjustment.
    float temperature(std::string_view biome, BlockPos p) const {
        const auto [base, frozen] = climate.at(biome);
        float value = base;
        if (frozen) {
            const double d0 = frozen_noise.value(p.x * 0.05, p.z * 0.05) * 7.0;
            const double d1 = info_noise.value(p.x * 0.2, p.z * 0.2);
            if (d0 + d1 < 0.3 && info_noise.value(p.x * 0.09, p.z * 0.09) < 0.8) value = 0.2F;
        }
        const auto snow_level = sea_level + 17;
        if (p.y > snow_level) {
            const auto noise_value = static_cast<float>(
                temperature_noise.value(static_cast<float>(p.x) / 8.0F, static_cast<float>(p.z) / 8.0F) * 8.0);
            return value - (noise_value + static_cast<float>(p.y) - static_cast<float>(snow_level)) * 0.05F / 40.0F;
        }
        return value;
    }

    // SurfaceRules.Context.getMinSurfaceLevel.
    std::int32_t preliminary_corner(std::int32_t x, std::int32_t z) const {
        const auto key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U | static_cast<std::uint32_t>(z);
        if (const auto it = preliminary.find(key); it != preliminary.end()) return it->second;
        if (preliminary.size() > 65536) preliminary.clear();
        return preliminary[key] = density.preliminary_surface(x, z);
    }

    std::int32_t min_surface_level(std::int32_t x, std::int32_t z) const {
        const auto cell_x = x >> 4, cell_z = z >> 4;
        const auto p00 = preliminary_corner(cell_x << 4, cell_z << 4);
        const auto p10 = preliminary_corner((cell_x + 1) << 4, cell_z << 4);
        const auto p01 = preliminary_corner(cell_x << 4, (cell_z + 1) << 4);
        const auto p11 = preliminary_corner((cell_x + 1) << 4, (cell_z + 1) << 4);
        const double ax = static_cast<double>(static_cast<float>(x & 15) / 16.0F);
        const double az = static_cast<double>(static_cast<float>(z & 15) / 16.0F);
        const auto lerp = [](double t, double a, double b) { return a + t * (b - a); };
        const auto level = support::java_floor(lerp(az, lerp(ax, p00, p10), lerp(ax, p01, p11)));
        return level + surface_depth(x, z) - 8;
    }

    std::int32_t surface_depth(std::int32_t x, std::int32_t z) const {
        auto rng = noise_random.at(x, 0, z);
        return support::java_d2i(noises[surface_noise]->sample(x, 0.0, z) * 2.75 + 3.0 + rng.next_double() * 0.25);
    }
};

const Json& SurfaceBuilder::Impl::features_root() {
    return Catalog::features_json();
}

namespace {

// The per-column and per-block state SurfaceRules.Context keeps.
struct Context {
    Context(const SurfaceBuilder::Impl& impl, const SurfaceChunk& chunk, bool clamp_biomes = true)
        : impl(impl), chunk(chunk), clamp_biomes(clamp_biomes) {}

    const SurfaceBuilder::Impl& impl;
    const SurfaceChunk& chunk;
    bool clamp_biomes;
    std::int32_t x = 0, z = 0, surface_depth = 0;
    std::optional<double> secondary;
    std::optional<std::int32_t> min_surface;
    std::optional<bool> steep;
    std::map<std::size_t, double> noise_values;
    std::int32_t y = 0, stone_above = 0, stone_below = 0, water_height = no_water;
    std::optional<std::optional<std::string_view>> biome;
    bool tie = false;

    std::optional<std::string_view> block_biome() {
        if (!biome) biome = impl.block_biome({x, y, z}, clamp_biomes);
        if (!*biome) tie = true;
        return *biome;
    }
    double noise(std::size_t index) {
        if (const auto it = noise_values.find(index); it != noise_values.end()) return it->second;
        return noise_values[index] = impl.noises[index]->sample(x, 0.0, z);
    }
    std::int32_t minimum_surface() {
        if (!min_surface) min_surface = impl.min_surface_level(x, z);
        return *min_surface;
    }
    double surface_secondary() {
        if (!secondary) secondary = impl.noises[impl.secondary_noise]->sample(x, 0.0, z);
        return *secondary;
    }
    std::int32_t chunk_height(std::int32_t lx, std::int32_t lz) const {
        return chunk.world_surface[static_cast<std::size_t>(lx * 16 + lz)] - 1;
    }
    // Context.SteepMaterialCondition reads this chunk's WORLD_SURFACE_WG heightmap as it stands when first tested.
    bool is_steep() {
        if (steep) return *steep;
        const int i = x & 15, j = z & 15;
        const int k = std::max(j - 1, 0), l = std::min(j + 1, 15);
        if (chunk_height(i, l) >= chunk_height(i, k) + 4) return *(steep = true);
        const int k1 = std::max(i - 1, 0), l1 = std::min(i + 1, 15);
        return *(steep = chunk_height(k1, j) >= chunk_height(l1, j) + 4);
    }

    bool test(const Condition& c) {
        switch (c.kind) {
            case Condition::Kind::biome: {
                const auto b = block_biome();
                return b && std::binary_search(c.biomes.begin(), c.biomes.end(), *b);
            }
            case Condition::Kind::noise_threshold: {
                const auto v = noise(c.noise);
                return v >= c.min_threshold && v <= c.max_threshold;
            }
            case Condition::Kind::vertical_gradient: {
                if (y <= c.true_at) return true;
                if (y >= c.false_at) return false;
                const double t = (static_cast<double>(y) - c.true_at) / (static_cast<double>(c.false_at) - c.true_at);
                auto rng = c.gradient->at(x, y, z);
                return static_cast<double>(rng.next_float()) < 1.0 + t * (0.0 - 1.0);
            }
            case Condition::Kind::y_above:
                return y + (c.add_stone_depth ? stone_above : 0) >= resolve(c.anchor) + surface_depth * c.multiplier;
            case Condition::Kind::water:
                return water_height == no_water ||
                       y + (c.add_stone_depth ? stone_above : 0) >= water_height + c.offset + surface_depth * c.multiplier;
            case Condition::Kind::temperature: {
                const auto b = block_biome();
                return b && !(impl.temperature(*b, {x, y, z}) >= 0.15F);
            }
            case Condition::Kind::steep: return is_steep();
            case Condition::Kind::invert: return !test(*c.target);
            case Condition::Kind::hole: return surface_depth <= 0;
            case Condition::Kind::above_preliminary_surface: return y >= minimum_surface();
            case Condition::Kind::stone_depth: {
                const auto depth = c.ceiling ? stone_below : stone_above;
                const auto extra = c.add_surface_depth ? surface_depth : 0;
                const auto secondary_depth =
                    c.secondary_range == 0 ? 0 : support::java_d2i((surface_secondary() + 1.0) / 2.0 * c.secondary_range);
                return depth <= 1 + c.offset + extra + secondary_depth;
            }
        }
        return false;
    }

    std::optional<BlockId> apply(const Rule& r) {
        switch (r.kind) {
            case Rule::Kind::block: return r.block;
            case Rule::Kind::sequence:
                for (const auto& item : r.sequence)
                    if (const auto result = apply(item)) return result;
                return std::nullopt;
            case Rule::Kind::condition: return test(*r.condition) ? apply(*r.then) : std::nullopt;
            case Rule::Kind::bandlands: {
                const auto offset = static_cast<std::int32_t>(
                    support::java_d2l(std::floor(impl.noises[impl.band_offset_noise]->sample(x, 0.0, z) * 4.0 + 0.5)));
                return impl.clay_bands[static_cast<std::size_t>((y + offset + 192) % 192)];
            }
        }
        return std::nullopt;
    }
};

}  // namespace

SurfaceBuilder::SurfaceBuilder(std::uint64_t seed) : impl_(std::make_unique<Impl>(seed)) {}
SurfaceBuilder::~SurfaceBuilder() = default;
SurfaceBuilder::SurfaceBuilder(SurfaceBuilder&&) noexcept = default;
SurfaceBuilder& SurfaceBuilder::operator=(SurfaceBuilder&&) noexcept = default;

std::int32_t SurfaceBuilder::min_surface_level(std::int32_t x, std::int32_t z) const {
    return impl_->min_surface_level(x, z);
}

void SurfaceChunk::set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId block) {
    if (y < min_y || y >= min_y + height) return;
    const auto& catalog = Catalog::get();
    const auto column = static_cast<std::size_t>((x & 15) * 16 + (z & 15));
    blocks[column * height + static_cast<std::size_t>(y - min_y)] = block;
    // A later write near the top of a tied column could meet blocks vanilla has there and this chunk lacks.
    if (uncertain_from[column] != std::numeric_limits<std::int32_t>::max() &&
        y >= std::min(ocean_floor_min[column], uncertain_from[column]) - 2) {
        ocean_floor_min[column] = min_y;
        ocean_floor_max[column] = std::numeric_limits<std::int32_t>::max();
    }
    const auto update = [&](std::int32_t& first_available, auto opaque) {
        if (y <= first_available - 2) return;
        if (opaque(block)) {
            if (y >= first_available) first_available = y + 1;
        } else if (first_available - 1 == y) {
            for (auto below = y - 1; below >= min_y; --below)
                if (opaque(blocks[column * height + static_cast<std::size_t>(below - min_y)])) {
                    first_available = below + 1;
                    return;
                }
            first_available = min_y;
        }
    };
    update(world_surface[column], [&](BlockId id) { return !catalog.info(id).air; });
    update(ocean_floor[column], [&](BlockId id) { return catalog.info(id).blocks_motion; });
}

std::optional<BlockId> SurfaceBuilder::top_material(const SurfaceChunk& chunk, BlockPos pos, bool under_fluid) const {
    Context context(*impl_, chunk, false);
    context.x = pos.x;
    context.z = pos.z;
    context.surface_depth = impl_->surface_depth(pos.x, pos.z);
    context.y = pos.y;
    context.stone_above = 1;
    context.stone_below = 1;
    context.water_height = under_fluid ? pos.y + 1 : no_water;
    return context.apply(impl_->rule);
}

SurfaceChunk SurfaceBuilder::build(ChunkPos chunk_pos) const {
    const auto& impl = *impl_;
    const auto& catalog = Catalog::get();
    SurfaceChunk chunk;
    const auto stone = catalog.stone, water = catalog.water;
    const auto snow = catalog.block("minecraft:snow_block"), packed_ice = catalog.block("minecraft:packed_ice");
    const auto air_like = [&](BlockId id) { return catalog.info(id).air; };
    const auto fluid = [&](BlockId id) { return catalog.info(id).fluid != Fluid::none; };
    const auto column_index = [](std::int32_t lx, std::int32_t lz) { return static_cast<std::size_t>(lx * 16 + lz); };
    const auto at = [&](std::int32_t lx, std::int32_t y, std::int32_t lz) -> BlockId {
        if (y < min_y || y >= min_y + height) return catalog.void_air;
        return chunk.blocks[column_index(lx, lz) * height + static_cast<std::size_t>(y - min_y)];
    };
    const auto set = [&](std::int32_t lx, std::int32_t y, std::int32_t lz, BlockId block) { chunk.set(lx, y, lz, block); };

    for (std::int32_t lx = 0; lx < 16; ++lx)
        for (std::int32_t lz = 0; lz < 16; ++lz) {
            const auto column = impl.terrain.column(chunk_pos.x * 16 + lx, chunk_pos.z * 16 + lz);
            auto* out = &chunk.blocks[column_index(lx, lz) * height];
            std::int32_t surface = min_y, floor = min_y;
            for (std::int32_t i = 0; i < height; ++i) {
                out[i] = catalog.block(column.blocks[static_cast<std::size_t>(i)]);
                if (!catalog.info(out[i]).air) surface = min_y + i + 1;
                if (catalog.info(out[i]).blocks_motion) floor = min_y + i + 1;
            }
            chunk.world_surface[column_index(lx, lz)] = surface;
            chunk.ocean_floor[column_index(lx, lz)] = floor;
            chunk.uncertain_from[column_index(lx, lz)] = std::numeric_limits<std::int32_t>::max();
        }

    for (std::int32_t lx = 0; lx < 16; ++lx)
        for (std::int32_t lz = 0; lz < 16; ++lz) {
            const auto x = chunk_pos.x * 16 + lx, z = chunk_pos.z * 16 + lz;
            const auto first_available = chunk.world_surface[column_index(lx, lz)];
            const auto surface_biome = impl.block_biome({x, first_available, z});
            Context context(impl, chunk);
            context.x = x;
            context.z = z;
            if (!surface_biome) context.tie = true;
            if (surface_biome == std::optional<std::string_view>{"minecraft:eroded_badlands"}) {
                // SurfaceSystem.erodedBadlandsExtension.
                const auto& n = impl.noises;
                const double d1 = std::min(std::abs(n[impl.badlands_surface]->sample(x, 0.0, z) * 8.25),
                                           n[impl.badlands_pillar]->sample(x * 0.2, 0.0, z * 0.2) * 15.0);
                if (!(d1 <= 0.0)) {
                    const double d4 = std::abs(n[impl.badlands_roof]->sample(x * 0.75, 0.0, z * 0.75) * 1.5);
                    const double d5 = 64.0 + std::min(d1 * d1 * 2.5, std::ceil(d4 * 50.0) + 24.0);
                    const auto top = support::java_floor(d5);
                    if (first_available <= top) {
                        bool blocked = false;
                        for (auto j = top; j >= min_y; --j) {
                            const auto state = at(lx, j, lz);
                            if (state == stone) break;
                            if (state == water) {
                                blocked = true;
                                break;
                            }
                        }
                        if (!blocked)
                            for (auto k = top; k >= min_y && air_like(at(lx, k, lz)); --k) set(lx, k, lz, stone);
                    }
                }
            }
            const auto start = chunk.world_surface[column_index(lx, lz)];
            // Rules only replace stone, so they can lower this height but never raise it.
            const auto floor_before_rules = chunk.ocean_floor[column_index(lx, lz)];
            context.surface_depth = impl.surface_depth(x, z);
            std::int32_t stone_above = 0, water_height = no_water, stone_bottom = std::numeric_limits<std::int32_t>::max();
            for (auto y = start; y >= min_y; --y) {
                const auto state = at(lx, y, lz);
                if (air_like(state)) {
                    stone_above = 0;
                    water_height = no_water;
                } else if (fluid(state)) {
                    if (water_height == no_water) water_height = y + 1;
                } else {
                    if (stone_bottom >= y) {
                        stone_bottom = way_below_min_y;
                        for (auto j = y - 1; j >= min_y - 1; --j) {
                            const auto below = at(lx, j, lz);
                            if (air_like(below) || fluid(below)) {
                                stone_bottom = j + 1;
                                break;
                            }
                        }
                    }
                    ++stone_above;
                    context.y = y;
                    context.stone_above = stone_above;
                    context.stone_below = y - stone_bottom + 1;
                    context.water_height = water_height;
                    context.biome.reset();
                    if (state == stone)
                        if (const auto result = context.apply(impl.rule)) set(lx, y, lz, *result);
                }
            }
            if (surface_biome == std::optional<std::string_view>{"minecraft:frozen_ocean"} ||
                surface_biome == std::optional<std::string_view>{"minecraft:deep_frozen_ocean"}) {
                // SurfaceSystem.frozenOceanExtension.
                const auto& n = impl.noises;
                const auto sea = impl.sea_level;
                const double d1 = std::min(std::abs(n[impl.iceberg_surface]->sample(x, 0.0, z) * 8.25),
                                           n[impl.iceberg_pillar]->sample(x * 1.28, 0.0, z * 1.28) * 15.0);
                if (!(d1 <= 1.8)) {
                    const double d5 = std::abs(n[impl.iceberg_roof]->sample(x * 1.17, 0.0, z * 1.17) * 1.5);
                    double d6 = std::min(d1 * d1 * 1.2, std::ceil(d5 * 40.0) + 14.0);
                    if (impl.temperature(*surface_biome, {x, sea, z}) > 0.1F) d6 -= 2.0;
                    double d2 = 0.0;
                    if (d6 > 2.0) {
                        d2 = sea - d6 - 7.0;
                        d6 += sea;
                    } else {
                        d6 = 0.0;
                    }
                    const auto top = support::java_d2i(d6), bottom = support::java_d2i(d2);
                    auto rng = impl.noise_random.at(x, 0, z);
                    const auto snow_count = 2 + rng.next_int(4);
                    const auto snow_from = sea + 18 + rng.next_int(10);
                    std::int32_t snowed = 0;
                    const auto minimum = context.minimum_surface();
                    for (auto y = std::max(first_available, top + 1); y >= minimum; --y) {
                        const auto state = at(lx, y, lz);
                        const bool ice = (air_like(state) && y < top && rng.next_double() > 0.01) ||
                                         (state == water && y > bottom && y < sea && d2 != 0.0 && rng.next_double() > 0.15);
                        if (!ice) continue;
                        if (snowed <= snow_count && y > snow_from) {
                            set(lx, y, lz, snow);
                            ++snowed;
                        } else {
                            set(lx, y, lz, packed_ice);
                        }
                    }
                }
            }
            // Eroded badlands pillars rise from the top, icebergs from the minimum surface level.
            const auto index = column_index(lx, lz);
            if (context.tie) {
                const auto from = std::min(context.minimum_surface(), first_available);
                chunk.uncertain_from[index] = from;
                // Pillars and icebergs only add blocks, and the blocks below the tied range are this chunk's.
                auto high = std::max(floor_before_rules, chunk.ocean_floor[index]);
                if (!surface_biome) high = std::max(high, impl.extension_ceiling(x, z));
                auto low = chunk.ocean_floor[index];
                if (!impl.rules_keep_floor) {
                    low = min_y;
                    for (auto y = std::min(from, chunk.ocean_floor[index]) - 1; y >= min_y; --y)
                        if (catalog.info(at(lx, y, lz)).blocks_motion) {
                            low = y + 1;
                            break;
                        }
                }
                chunk.ocean_floor_min[index] = low;
                chunk.ocean_floor_max[index] = high;
            }
        }
    return chunk;
}

}  // namespace minecraft::feature
