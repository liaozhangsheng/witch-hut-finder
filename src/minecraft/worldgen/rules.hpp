#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minecraft::worldgen {

struct Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::map<std::string, Json>;

struct Json : std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray, JsonObject> {
    using variant::variant;
    bool is_number() const noexcept {
        return std::holds_alternative<std::int64_t>(*this) || std::holds_alternative<double>(*this);
    }
    bool is_string() const noexcept { return std::holds_alternative<std::string>(*this); }
};

Json parse_frozen_rules(std::span<const std::string_view> parts);
const Json& rules();
const Json& field(const Json& value, std::string_view key);
double number(const Json& value);
std::string_view string(const Json& value);

}  // namespace minecraft::worldgen
