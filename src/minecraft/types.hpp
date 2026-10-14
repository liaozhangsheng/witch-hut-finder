#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace minecraft {

struct BlockPos {
    std::int32_t x, y, z;
    auto operator<=>(const BlockPos&) const = default;
};

struct ChunkPos {
    std::int32_t x, z;
    auto operator<=>(const ChunkPos&) const = default;
};

struct QuartPos {
    std::int32_t x, y, z;
    auto operator<=>(const QuartPos&) const = default;
};

// The divisor is positive; Java coordinate conversions round towards negative infinity.
constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) {
    return value / divisor - (value % divisor < 0);
}

constexpr QuartPos to_quart(BlockPos p) {
    return {floor_div(p.x, 4), floor_div(p.y, 4), floor_div(p.z, 4)};
}

// Strict decimal signed-long input; the result preserves its Java long bit pattern.
std::optional<std::uint64_t> parse_seed(std::string_view text);

enum class Dimension { overworld, nether, end };

}  // namespace minecraft
