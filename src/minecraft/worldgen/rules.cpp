#include "rules.hpp"

#include <minecraft/worldgen_data.hpp>
#include <simdjson.h>

#include <limits>
#include <stdexcept>

namespace minecraft::worldgen {
namespace {

Json convert(simdjson::dom::element value) {
    switch (value.type()) {
        case simdjson::dom::element_type::ARRAY: {
            JsonArray result;
            for (const auto item : value.get_array()) result.push_back(convert(item));
            return result;
        }
        case simdjson::dom::element_type::OBJECT: {
            JsonObject result;
            for (const auto item : value.get_object()) result.emplace(std::string(item.key), convert(item.value));
            return result;
        }
        case simdjson::dom::element_type::STRING: return std::string(value.get_string().value());
        case simdjson::dom::element_type::INT64: return value.get_int64().value();
        case simdjson::dom::element_type::UINT64: {
            const auto number = value.get_uint64().value();
            return number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                       ? Json(static_cast<std::int64_t>(number))
                       : Json(static_cast<double>(number));
        }
        case simdjson::dom::element_type::DOUBLE: return value.get_double().value();
        case simdjson::dom::element_type::BOOL: return value.get_bool().value();
        case simdjson::dom::element_type::BIGINT:
            return std::stod(std::string(value.get_bigint().value()));
        case simdjson::dom::element_type::NULL_VALUE: return nullptr;
    }
    throw std::runtime_error("unsupported JSON value");
}

}  // namespace

Json parse_frozen_rules(std::span<const std::string_view> parts) {
    std::string json;
    for (auto part : parts)
        json.append(part);
    simdjson::padded_string padded(json);
    simdjson::dom::parser parser;
    return convert(parser.parse(padded));
}

const Json& rules() {
    static const auto data = parse_frozen_rules(version::worldgen_json);
    return data;
}

const Json& field(const Json& value, std::string_view key) {
    return std::get<JsonObject>(value).at(std::string(key));
}

double number(const Json& value) {
    if (std::holds_alternative<std::int64_t>(value)) return static_cast<double>(std::get<std::int64_t>(value));
    return std::get<double>(value);
}

std::string_view string(const Json& value) {
    if (std::holds_alternative<std::string>(value))
        return std::get<std::string>(value);
    throw std::runtime_error("JSON value is not a string");
}

}  // namespace minecraft::worldgen
