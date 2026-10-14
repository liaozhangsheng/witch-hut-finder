#pragma once

#include <minecraft/biome_sampler.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace minecraft::feature {

// Noise biomes by quart position, for one thread. Block biomes of nearby blocks zoom to the same few quarts, and each
// noise biome costs a climate sample and a tree search.
class BiomeMemo {
public:
    explicit BiomeMemo(const biome::Sampler& biomes) : biomes_(biomes) {}

    [[nodiscard]] std::optional<std::string_view> noise(QuartPos q) {
        const auto key = Key{q.x, q.y, q.z};
        if (const auto it = known_.find(key); it != known_.end()) return it->second;
        if (known_.size() >= limit) known_.clear();
        return known_.emplace(key, biomes_.noise(q).biome_id).first->second;
    }

private:
    struct Key {
        std::int32_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& k) const {
            auto h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.z)) * 0xC2B2AE3D27D4EB4FULL;
            h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) * 0x165667B19E3779F9ULL;
            return static_cast<std::size_t>(h ^ (h >> 29U));
        }
    };
    static constexpr std::size_t limit = 1U << 16U;
    const biome::Sampler& biomes_;
    std::unordered_map<Key, std::optional<std::string_view>, Hash> known_;
};

}  // namespace minecraft::feature
