#include <minecraft/types.hpp>

#include <bit>
#include <charconv>

namespace minecraft {

std::optional<std::uint64_t> parse_seed(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') {
            return std::nullopt;
        }
    }
    std::int64_t value;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return std::bit_cast<std::uint64_t>(value);
}

}  // namespace minecraft
