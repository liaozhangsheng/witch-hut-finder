#include "carver.hpp"

#include <minecraft/random.hpp>

#include "../support/java_math.hpp"
#include "../worldgen/rules.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace minecraft::feature {
namespace {

using Json = minecraft::worldgen::Json;
using worldgen::field;
using worldgen::number;
using worldgen::string;

constexpr std::int32_t min_gen_y = -64, gen_depth = 384;
constexpr float pi_f = static_cast<float>(3.141592653589793);

const Json* optional_field(const Json& value, std::string_view key) {
    if (!std::holds_alternative<minecraft::worldgen::JsonObject>(value)) return nullptr;
    const auto& object = std::get<minecraft::worldgen::JsonObject>(value);
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &it->second;
}

struct FloatProvider {
    enum class Kind { constant, uniform, trapezoid } kind = Kind::constant;
    float a = 0.0F, b = 0.0F, plateau = 0.0F;  // value, or min and max

    float sample(random::LegacyRandom& random) const {
        switch (kind) {
            case Kind::constant: return a;
            case Kind::uniform: return random.next_float() * (b - a) + a;  // Mth.randomBetween
            case Kind::trapezoid: break;
        }
        const float range = b - a, ramp = (range - plateau) / 2.0F, rest = range - ramp;
        const float first = random.next_float() * rest;
        return a + first + random.next_float() * ramp;
    }
};

FloatProvider float_provider(const Json& value) {
    FloatProvider result;
    if (!std::holds_alternative<minecraft::worldgen::JsonObject>(value)) {
        result.a = static_cast<float>(number(value));
        return result;
    }
    const auto type = string(field(value, "type"));
    if (type == "minecraft:uniform") {
        result.kind = FloatProvider::Kind::uniform;
        result.a = static_cast<float>(number(field(value, "min_inclusive")));
        result.b = static_cast<float>(number(field(value, "max_exclusive")));
    } else if (type == "minecraft:trapezoid") {
        result.kind = FloatProvider::Kind::trapezoid;
        result.a = static_cast<float>(number(field(value, "min")));
        result.b = static_cast<float>(number(field(value, "max")));
        result.plateau = static_cast<float>(number(field(value, "plateau")));
    } else if (type == "minecraft:constant") {
        result.a = static_cast<float>(number(field(value, "value")));
    } else {
        throw std::runtime_error("unsupported float provider " + std::string(type));
    }
    return result;
}

VerticalAnchor anchor(const Json& value) {
    if (const auto* v = optional_field(value, "absolute")) return {VerticalAnchor::Kind::absolute, static_cast<std::int32_t>(number(*v))};
    if (const auto* v = optional_field(value, "above_bottom")) return {VerticalAnchor::Kind::above_bottom, static_cast<std::int32_t>(number(*v))};
    if (const auto* v = optional_field(value, "below_top")) return {VerticalAnchor::Kind::below_top, static_cast<std::int32_t>(number(*v))};
    throw std::runtime_error("unsupported carver anchor");
}

struct Carver {
    enum class Type { cave, canyon } type;
    float probability;
    VerticalAnchor y_min, y_max;  // uniform height provider
    FloatProvider y_scale;
    std::int32_t lava_level;
    const BlockSet* replaceable;
    FloatProvider horizontal_radius_multiplier, vertical_radius_multiplier, floor_level;  // caves
    FloatProvider vertical_rotation, distance_factor, thickness, horizontal_radius_factor;  // canyons
    float vertical_radius_default_factor = 0.0F, vertical_radius_center_factor = 0.0F;
    std::int32_t width_smoothness = 1;

    std::int32_t sample_y(random::LegacyRandom& random) const {
        const auto low = y_min.resolve(min_gen_y, gen_depth), high = y_max.resolve(min_gen_y, gen_depth);
        if (low > high) return low;
        return random.next_int(high - low + 1) + low;
    }
};

class Carvers {
public:
    static const Carvers& get() {
        static const Carvers carvers;
        return carvers;
    }
    // The list every overworld biome uses, when they all use the same one; the carver biome is then irrelevant.
    [[nodiscard]] const std::vector<const Carver*>* shared() const { return shared_; }
    [[nodiscard]] const std::vector<const Carver*>& for_biome(const std::optional<std::string_view>& biome) const {
        if (!biome) {
            // Overworld biomes share one carver list, so a climate tie does not matter.
            if (!shared_) throw std::runtime_error("carver lists differ between biomes; climate ties are unsupported");
            return *shared_;
        }
        return biome_lists_.at(*biome);
    }

private:
    Carvers() {
        const auto& root = Catalog::features_json();
        const auto& catalog = Catalog::get();
        for (const auto& [id, value] : std::get<minecraft::worldgen::JsonObject>(field(root, "carvers"))) {
            const auto type = string(field(value, "type"));
            const auto& config = field(value, "config");
            Carver carver{};
            if (type == "minecraft:cave") carver.type = Carver::Type::cave;
            else if (type == "minecraft:canyon") carver.type = Carver::Type::canyon;
            else throw std::runtime_error("unsupported carver " + std::string(type));
            carver.probability = static_cast<float>(number(field(config, "probability")));
            const auto& y = field(config, "y");
            if (string(field(y, "type")) != "minecraft:uniform") throw std::runtime_error("unsupported carver height");
            carver.y_min = anchor(field(y, "min_inclusive"));
            carver.y_max = anchor(field(y, "max_inclusive"));
            carver.y_scale = float_provider(field(config, "yScale"));
            carver.lava_level = anchor(field(config, "lava_level")).resolve(min_gen_y, gen_depth);
            auto replaceable = string(field(config, "replaceable"));
            if (!replaceable.starts_with('#')) throw std::runtime_error("carver replaceable must be a tag");
            carver.replaceable = &catalog.tag(replaceable.substr(1));
            if (carver.type == Carver::Type::cave) {
                carver.horizontal_radius_multiplier = float_provider(field(config, "horizontal_radius_multiplier"));
                carver.vertical_radius_multiplier = float_provider(field(config, "vertical_radius_multiplier"));
                carver.floor_level = float_provider(field(config, "floor_level"));
            } else {
                const auto& shape = field(config, "shape");
                carver.vertical_rotation = float_provider(field(config, "vertical_rotation"));
                carver.distance_factor = float_provider(field(shape, "distance_factor"));
                carver.thickness = float_provider(field(shape, "thickness"));
                carver.horizontal_radius_factor = float_provider(field(shape, "horizontal_radius_factor"));
                carver.vertical_radius_default_factor = static_cast<float>(number(field(shape, "vertical_radius_default_factor")));
                carver.vertical_radius_center_factor = static_cast<float>(number(field(shape, "vertical_radius_center_factor")));
                carver.width_smoothness = static_cast<std::int32_t>(number(field(shape, "width_smoothness")));
            }
            carvers_.emplace(id, carver);
        }
        for (const auto& [biome, ids] : std::get<minecraft::worldgen::JsonObject>(field(root, "biome_carvers"))) {
            auto& list = biome_lists_[biome];
            for (const auto& id : std::get<minecraft::worldgen::JsonArray>(ids)) list.push_back(&carvers_.at(string(id)));
        }
        shared_ = &biome_lists_.begin()->second;
        for (const auto& [biome, list] : biome_lists_)
            if (list != *shared_) shared_ = nullptr;
        for (const auto biome : biome::catalog())
            if (!biome_lists_.contains(biome)) shared_ = nullptr;
    }

    std::unordered_map<std::string_view, Carver> carvers_;
    std::unordered_map<std::string_view, std::vector<const Carver*>> biome_lists_;
    const std::vector<const Carver*>* shared_ = nullptr;
};

using Skip = std::function<bool(double, double, double, std::int32_t)>;

class Carving {
public:
    Carving(ChunkPos chunk, CarveTarget& target) : chunk_(chunk), target_(target), mask_(16U * 16U * gen_depth) {}

    void cave(const Carver& c, random::LegacyRandom& random, ChunkPos source) {
        const std::int32_t range = 16 * (4 * 2 - 1);
        const auto tunnels = random.next_int(random.next_int(random.next_int(15) + 1) + 1);
        for (std::int32_t k = 0; k < tunnels; ++k) {
            const double x = source.x * 16 + random.next_int(16);
            const double y = c.sample_y(random);
            const double z = source.z * 16 + random.next_int(16);
            const double horizontal = c.horizontal_radius_multiplier.sample(random);
            const double vertical = c.vertical_radius_multiplier.sample(random);
            const double floor = c.floor_level.sample(random);
            const Skip skip = [floor](double xd, double yd, double zd, std::int32_t) {
                return yd <= floor || xd * xd + yd * yd + zd * zd >= 1.0;
            };
            std::int32_t count = 1;
            if (random.next_int(4) == 0) {
                const double y_scale = c.y_scale.sample(random);
                const float thickness = 1.0F + random.next_float() * 6.0F;
                // createRoom.
                const double radius = 1.5 + static_cast<double>(support::MthTrig::sin(static_cast<double>(static_cast<float>(3.141592653589793 / 2))) * thickness);
                ellipsoid(c, x + 1.0, y, z, radius, radius * y_scale, skip);
                count += random.next_int(4);
            }
            for (std::int32_t t = 0; t < count; ++t) {
                const float yaw = random.next_float() * static_cast<float>(3.141592653589793 * 2);
                const float pitch = (random.next_float() - 0.5F) / 4.0F;
                const float thickness = cave_thickness(random);
                const auto distance = range - random.next_int(range / 4);
                const auto seed = random.next_long();
                tunnel(c, seed, x, y, z, horizontal, vertical, thickness, yaw, pitch, 0, distance, 1.0, skip);
            }
        }
    }

    void canyon(const Carver& c, random::LegacyRandom& random, ChunkPos source) {
        const std::int32_t range = (4 * 2 - 1) * 16;
        const double x = source.x * 16 + random.next_int(16);
        const auto y = c.sample_y(random);
        const double z = source.z * 16 + random.next_int(16);
        const float yaw = random.next_float() * static_cast<float>(3.141592653589793 * 2);
        const float pitch = c.vertical_rotation.sample(random);
        const double y_scale = c.y_scale.sample(random);
        const float thickness = c.thickness.sample(random);
        const auto distance = support::java_d2i(static_cast<double>(static_cast<float>(range) * c.distance_factor.sample(random)));
        canyon_tunnel(c, static_cast<std::int64_t>(random.next_long()), x, y, z, thickness, yaw, pitch, 0, distance, y_scale);
    }

private:
    ChunkPos chunk_;
    CarveTarget& target_;
    std::vector<bool> mask_;

    static float cave_thickness(random::LegacyRandom& random) {
        float f = random.next_float() * 2.0F + random.next_float();
        if (random.next_int(10) == 0) f *= random.next_float() * random.next_float() * 3.0F + 1.0F;
        return f;
    }

    bool can_reach(double x, double z, std::int32_t step, std::int32_t total, float thickness) const {
        const double dx = x - (chunk_.x * 16 + 8), dz = z - (chunk_.z * 16 + 8);
        const double remaining = total - step;
        const double reach = static_cast<double>(thickness + 2.0F + 16.0F);
        return dx * dx + dz * dz - remaining * remaining <= reach * reach;
    }

    void tunnel(const Carver& c, std::uint64_t seed, double x, double y, double z, double horizontal, double vertical,
                float thickness, float yaw, float pitch, std::int32_t step, std::int32_t distance, double y_scale,
                const Skip& skip) {
        random::LegacyRandom random(seed);
        const auto branch = random.next_int(distance / 2) + distance / 4;
        const bool steep = random.next_int(6) == 0;
        float yaw_change = 0.0F, pitch_change = 0.0F;
        for (auto j = step; j < distance; ++j) {
            const double radius = 1.5 + static_cast<double>(
                support::MthTrig::sin(static_cast<double>(pi_f * static_cast<float>(j) / static_cast<float>(distance))) * thickness);
            const double vertical_radius = radius * y_scale;
            const float cos_pitch = support::MthTrig::cos(pitch);
            x += support::MthTrig::cos(yaw) * cos_pitch;
            y += support::MthTrig::sin(pitch);
            z += support::MthTrig::sin(yaw) * cos_pitch;
            pitch *= steep ? 0.92F : 0.7F;
            pitch += pitch_change * 0.1F;
            yaw += yaw_change * 0.1F;
            pitch_change *= 0.9F;
            yaw_change *= 0.75F;
            {
                const float a = random.next_float(), b = random.next_float(), d = random.next_float();
                pitch_change += (a - b) * d * 2.0F;
            }
            {
                const float a = random.next_float(), b = random.next_float(), d = random.next_float();
                yaw_change += (a - b) * d * 4.0F;
            }
            if (j == branch && thickness > 1.0F) {
                const auto left_seed = random.next_long();
                const float left_thickness = random.next_float() * 0.5F + 0.5F;
                tunnel(c, left_seed, x, y, z, horizontal, vertical, left_thickness, yaw - static_cast<float>(3.141592653589793 / 2),
                       pitch / 3.0F, j, distance, 1.0, skip);
                const auto right_seed = random.next_long();
                const float right_thickness = random.next_float() * 0.5F + 0.5F;
                tunnel(c, right_seed, x, y, z, horizontal, vertical, right_thickness, yaw + static_cast<float>(3.141592653589793 / 2),
                       pitch / 3.0F, j, distance, 1.0, skip);
                return;
            }
            if (random.next_int(4) != 0) {
                if (!can_reach(x, z, j, distance, thickness)) return;
                ellipsoid(c, x, y, z, radius * horizontal, vertical_radius * vertical, skip);
            }
        }
    }

    void canyon_tunnel(const Carver& c, std::int64_t seed, double x, double y, double z, float thickness, float yaw,
                       float pitch, std::int32_t step, std::int32_t distance, double y_scale) {
        random::LegacyRandom random(static_cast<std::uint64_t>(seed));
        std::vector<float> widths(static_cast<std::size_t>(gen_depth));
        float width = 1.0F;
        for (std::int32_t j = 0; j < gen_depth; ++j) {
            if (j == 0 || random.next_int(c.width_smoothness) == 0) width = 1.0F + random.next_float() * random.next_float();
            widths[static_cast<std::size_t>(j)] = width * width;
        }
        const Skip skip = [&widths](double xd, double yd, double zd, std::int32_t block_y) {
            const auto index = block_y - min_gen_y;
            return (xd * xd + zd * zd) * widths[static_cast<std::size_t>(index - 1)] + yd * yd / 6.0 >= 1.0;
        };
        float yaw_change = 0.0F, pitch_change = 0.0F;
        for (auto i = step; i < distance; ++i) {
            double radius = 1.5 + static_cast<double>(
                support::MthTrig::sin(static_cast<double>(static_cast<float>(i) * pi_f / static_cast<float>(distance))) * thickness);
            double vertical_radius = radius * y_scale;
            radius *= c.horizontal_radius_factor.sample(random);
            {
                // updateVerticalRadius.
                const float f = 1.0F - std::abs(0.5F - static_cast<float>(i) / static_cast<float>(distance)) * 2.0F;
                const float factor = c.vertical_radius_default_factor + c.vertical_radius_center_factor * f;
                const float scale = random.next_float() * (1.0F - 0.75F) + 0.75F;
                vertical_radius = static_cast<double>(factor) * vertical_radius * static_cast<double>(scale);
            }
            const float cos_pitch = support::MthTrig::cos(pitch);
            const float sin_pitch = support::MthTrig::sin(pitch);
            x += support::MthTrig::cos(yaw) * cos_pitch;
            y += sin_pitch;
            z += support::MthTrig::sin(yaw) * cos_pitch;
            pitch *= 0.7F;
            pitch += pitch_change * 0.05F;
            yaw += yaw_change * 0.05F;
            pitch_change *= 0.8F;
            yaw_change *= 0.5F;
            {
                const float a = random.next_float(), b = random.next_float(), d = random.next_float();
                pitch_change += (a - b) * d * 2.0F;
            }
            {
                const float a = random.next_float(), b = random.next_float(), d = random.next_float();
                yaw_change += (a - b) * d * 4.0F;
            }
            if (random.next_int(4) != 0) {
                if (!can_reach(x, z, i, distance, thickness)) return;
                ellipsoid(c, x, y, z, radius, vertical_radius, skip);
            }
        }
    }

    std::size_t mask_index(std::int32_t lx, std::int32_t y, std::int32_t lz) const {
        return static_cast<std::size_t>((lx & 15) | (lz & 15) << 4 | (y - min_gen_y) << 8);
    }

    void ellipsoid(const Carver& c, double x, double y, double z, double horizontal, double vertical, const Skip& skip) {
        const double middle_x = chunk_.x * 16 + 8, middle_z = chunk_.z * 16 + 8;
        const double reach = 16.0 + horizontal * 2.0;
        if (std::abs(x - middle_x) > reach || std::abs(z - middle_z) > reach) return;
        const auto min_x = chunk_.x * 16, min_z = chunk_.z * 16;
        const auto x0 = std::max(support::java_floor(x - horizontal) - min_x - 1, 0);
        const auto x1 = std::min(support::java_floor(x + horizontal) - min_x, 15);
        const auto y0 = std::max(support::java_floor(y - vertical) - 1, min_gen_y + 1);
        const auto y1 = std::min(support::java_floor(y + vertical) + 1, min_gen_y + gen_depth - 1 - 7);
        const auto z0 = std::max(support::java_floor(z - horizontal) - min_z - 1, 0);
        const auto z1 = std::min(support::java_floor(z + horizontal) - min_z, 15);
        for (auto lx = x0; lx <= x1; ++lx) {
            const auto bx = min_x + lx;
            const double dx = (bx + 0.5 - x) / horizontal;
            for (auto lz = z0; lz <= z1; ++lz) {
                const auto bz = min_z + lz;
                const double dz = (bz + 0.5 - z) / horizontal;
                if (dx * dx + dz * dz >= 1.0) continue;
                bool grass = false;
                for (auto by = y1; by > y0; --by) {
                    const double dy = (by - 0.5 - y) / vertical;
                    if (skip(dx, dy, dz, by)) continue;
                    const auto index = mask_index(lx, by, lz);
                    if (mask_[index]) continue;
                    mask_[index] = true;
                    carve_block(c, {bx, by, bz}, grass);
                }
            }
        }
    }

    void carve_block(const Carver& c, BlockPos p, bool& grass) {
        const auto& catalog = Catalog::get();
        static const auto grass_block = catalog.block("minecraft:grass_block");
        static const auto mycelium = catalog.block("minecraft:mycelium");
        static const auto dirt = catalog.block("minecraft:dirt");
        static const auto lava = catalog.block("minecraft:lava");
        const auto state = target_.get(p);
        if (state == grass_block || state == mycelium) grass = true;
        if (!c.replaceable->contains(state)) return;
        BlockId carved;
        if (p.y <= c.lava_level) {
            carved = lava;
        } else {
            const auto substance = target_.carved_substance(p);
            if (!substance) return;
            carved = *substance;
        }
        target_.set(p, carved);
        if (!grass) return;
        const BlockPos below{p.x, p.y - 1, p.z};
        if (target_.get(below) != dirt) return;
        if (const auto top = target_.top_material(below, catalog.info(carved).fluid != Fluid::none)) target_.set(below, *top);
    }
};

}  // namespace

void carve_chunk(std::uint64_t seed, ChunkPos chunk, const biome::Sampler& biomes, CarveTarget& target) {
    const auto& carvers = Carvers::get();
    Carving carving(chunk, target);
    for (std::int32_t dx = -8; dx <= 8; ++dx)
        for (std::int32_t dz = -8; dz <= 8; ++dz) {
            const ChunkPos source{chunk.x + dx, chunk.z + dz};
            const auto& list = carvers.shared() ? *carvers.shared()
                                                : carvers.for_biome(biomes.noise({source.x * 4, 0, source.z * 4}).biome_id);
            std::int32_t index = 0;
            for (const auto* carver : list) {
                // WorldgenRandom.setLargeFeatureSeed over a LegacyRandomSource.
                random::LegacyRandom random(seed + static_cast<std::uint64_t>(index));
                const auto a = random.next_long(), b = random.next_long();
                random.set_seed((static_cast<std::uint64_t>(static_cast<std::int64_t>(source.x)) * a) ^
                                (static_cast<std::uint64_t>(static_cast<std::int64_t>(source.z)) * b) ^
                                (seed + static_cast<std::uint64_t>(index)));
                if (random.next_float() <= carver->probability) {
                    if (carver->type == Carver::Type::cave) carving.cave(*carver, random, source);
                    else carving.canyon(*carver, random, source);
                }
                ++index;
            }
        }
}

}  // namespace minecraft::feature
