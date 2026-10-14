#pragma once

#include <minecraft/types.hpp>

#include <string_view>

namespace minecraft::structure {

struct RandomSpreadPlacement {
    std::int32_t spacing, separation, salt;
};

struct StructureSet {
    std::string_view id;
    std::string_view structure_id;
    RandomSpreadPlacement placement;
};

// Compiled, immutable 26.1 swamp-hut placement.
const StructureSet* find_set(std::string_view id);

}  // namespace minecraft::structure
