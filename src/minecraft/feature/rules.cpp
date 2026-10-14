#include "rules.hpp"

#include <minecraft/feature_data.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace minecraft::feature {
namespace {

using worldgen::field;
using worldgen::string;
using Json = minecraft::worldgen::Json;

const Json& rules() {
    static const auto data = worldgen::parse_frozen_rules(version::feature_json);
    return data;
}
}  // namespace

std::int32_t VerticalAnchor::resolve(std::int32_t min_y, std::int32_t depth) const {
    switch (kind) {
        case Kind::absolute: return value;
        case Kind::above_bottom: return min_y + value;
        case Kind::below_top: break;
    }
    return depth - 1 + min_y - value;
}

const minecraft::worldgen::Json& Catalog::features_json() {
    return rules();
}

const Catalog& Catalog::get() {
    static const Catalog catalog;
    return catalog;
}

Catalog::Catalog() {
    const auto& root = rules();
    for (const auto& [id, properties] : std::get<minecraft::worldgen::JsonObject>(field(root, "blocks"))) {
        BlockInfo info;
        info.id = id;
        info.air = std::get<bool>(field(properties, "air"));
        info.blocks_motion = std::get<bool>(field(properties, "blocks_motion"));
        const auto fluid = string(field(properties, "fluid"));
        info.fluid = fluid == "minecraft:water" ? Fluid::water : fluid == "minecraft:lava" ? Fluid::lava : Fluid::none;
        if (!fluid.empty() && info.fluid == Fluid::none)
            throw std::runtime_error("unsupported fluid " + std::string(fluid));
        blocks_.push_back(info);
    }
    std::sort(blocks_.begin(), blocks_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (blocks_.size() > 1024) throw std::runtime_error("block table exceeds BlockSet capacity");
    for (std::size_t i = 0; i < blocks_.size(); ++i) block_ids_.emplace(blocks_[i].id, static_cast<BlockId>(i));
    for (const auto& [id, members] : std::get<minecraft::worldgen::JsonObject>(field(root, "block_tags"))) {
        BlockSet set;
        for (const auto& member : std::get<minecraft::worldgen::JsonArray>(members)) set.insert(block(string(member)));
        tags_.emplace(id, set);
    }
    void_air = block("minecraft:void_air");
    water = block("minecraft:water");
    stone = block("minecraft:stone");
}

BlockId Catalog::block(std::string_view id) const {
    const auto it = block_ids_.find(id);
    if (it == block_ids_.end())
        throw std::out_of_range("block outside the frozen decoration table: " + std::string(id));
    return it->second;
}

const BlockSet& Catalog::tag(std::string_view id) const {
    const auto it = tags_.find(id);
    if (it == tags_.end()) throw std::out_of_range("block tag outside the frozen decoration table: " + std::string(id));
    return it->second;
}
}  // namespace minecraft::feature
