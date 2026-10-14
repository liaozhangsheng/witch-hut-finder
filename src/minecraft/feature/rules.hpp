#pragma once

#include <minecraft/types.hpp>

#include "../worldgen/rules.hpp"

#include <bitset>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minecraft::feature {

using BlockId = std::uint16_t;
enum class Fluid : std::uint8_t { none, water, lava };

struct BlockInfo {
    std::string_view id;
    bool air = false;
    bool blocks_motion = false;
    Fluid fluid = Fluid::none;
};

class BlockSet {
public:
    void insert(BlockId id) { bits_.set(id); }
    [[nodiscard]] bool contains(BlockId id) const { return bits_.test(id); }

private:
    std::bitset<1024> bits_;
};

struct VerticalAnchor {
    enum class Kind { absolute, above_bottom, below_top } kind = Kind::absolute;
    std::int32_t value = 0;
    [[nodiscard]] std::int32_t resolve(std::int32_t min_y, std::int32_t depth) const;
};

// Block properties and tags used by surface and carver simulation.
class Catalog {
public:
    static const Catalog& get();

    [[nodiscard]] BlockId block(std::string_view id) const;
    [[nodiscard]] const BlockInfo& info(BlockId id) const { return blocks_[id]; }
    [[nodiscard]] const BlockSet& tag(std::string_view id) const;
    [[nodiscard]] static const minecraft::worldgen::Json& features_json();

    BlockId void_air, water, stone;

private:
    Catalog();
    std::vector<BlockInfo> blocks_;
    std::unordered_map<std::string_view, BlockId> block_ids_;
    std::unordered_map<std::string_view, BlockSet> tags_;
};

}  // namespace minecraft::feature
