#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <minecraft/types.hpp>

namespace minecraft::biome {

struct ClimateTarget {
    std::array<std::int64_t, 6> values;
};

struct Sample {
    ClimateTarget climate;
    std::optional<std::string_view> biome_id;
};

}  // namespace minecraft::biome
