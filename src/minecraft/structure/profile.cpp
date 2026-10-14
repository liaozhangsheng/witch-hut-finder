#include <minecraft/structure.hpp>

namespace minecraft::structure {
namespace {
const StructureSet set{"minecraft:swamp_huts", "minecraft:swamp_hut",
                       RandomSpreadPlacement{32, 8, 14357620}};
}

const StructureSet* find_set(std::string_view id) {
    return id == set.id ? &set : nullptr;
}
}  // namespace minecraft::structure
