#include "veins.hpp"

#include <minecraft/random.hpp>

#include <algorithm>
#include <cmath>

namespace minecraft::terrain {
namespace {
double clamped_map(double v, double a, double b, double c, double d) {
    return c + std::clamp((v - a) / (b - a), 0.0, 1.0) * (d - c);
}
}  // namespace

std::optional<VeinBlock> vein_block(const worldgen::DensitySampler& density,
                                    worldgen::DensitySampler::ColumnScope& scope, BlockPos p) {
    if (!density.ore_veins_enabled())
        return std::nullopt;
    const double toggle = scope.column("vein_toggle", p.y);
    const bool copper = toggle > 0;
    const int upper = (copper ? 50 : -8) - p.y, lower = p.y - (copper ? 0 : -60);
    if (upper < 0 || lower < 0)
        return std::nullopt;
    const double strength = std::abs(toggle);
    if (strength + clamped_map(std::min(upper, lower), 0, 20, -.2, 0) < static_cast<double>(.4F))
        return std::nullopt;
    auto rng = density.ore_random().at(p.x, p.y, p.z);
    if (rng.next_float() > .7F || scope.column("vein_ridged", p.y) >= 0)
        return std::nullopt;
    const auto kind = copper ? VeinKind::copper : VeinKind::iron;
    const double richness = clamped_map(strength, .4F, .6F, .1F, .3F);
    if (rng.next_float() < richness && scope.column("vein_gap", p.y) > static_cast<double>(-.3F))
        return VeinBlock{kind, rng.next_float() < .02F ? VeinMaterial::raw_block : VeinMaterial::ore};
    return VeinBlock{kind, VeinMaterial::filler};
}

std::string_view vein_block_id(VeinBlock block) {
    const bool copper = block.kind == VeinKind::copper;
    switch (block.material) {
        case VeinMaterial::raw_block: return copper ? "minecraft:raw_copper_block" : "minecraft:raw_iron_block";
        case VeinMaterial::ore: return copper ? "minecraft:copper_ore" : "minecraft:deepslate_iron_ore";
        case VeinMaterial::filler: break;
    }
    return copper ? "minecraft:granite" : "minecraft:tuff";
}

}  // namespace minecraft::terrain
