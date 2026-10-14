#include <minecraft/random.hpp>

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minecraft::random {
namespace {

constexpr std::uint64_t legacy_multiplier = 0x5DEECE66DULL;
constexpr std::uint64_t legacy_increment = 0xBULL;
constexpr std::uint64_t legacy_mask = (1ULL << 48U) - 1U;
constexpr std::uint64_t golden_ratio_64 = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t silver_ratio_64 = 0x6A09E667F3BCC909ULL;

std::uint64_t mix_stafford13(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::uint64_t upgrade_seed_lo(std::uint64_t seed) noexcept {
    return mix_stafford13(seed ^ silver_ratio_64);
}

std::uint64_t upgrade_seed_hi(std::uint64_t seed) noexcept {
    return mix_stafford13((seed ^ silver_ratio_64) + golden_ratio_64);
}

// Both halves of the MD5 digest, as the positional fork consumes them.
std::array<std::uint64_t, 2> md5_words_be(const std::string_view input) {
    constexpr std::array<std::uint32_t, 64> shifts{
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    if (input.size() > (std::numeric_limits<std::uint64_t>::max() >> 3U)) {
        throw std::length_error("name is too long to hash");
    }

    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    bytes.push_back(0x80U);
    while (bytes.size() % 64U != 56U) {
        bytes.push_back(0);
    }
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(bit_length >> (i * 8U)));
    }

    std::uint32_t a0 = 0x67452301U;
    std::uint32_t b0 = 0xefcdab89U;
    std::uint32_t c0 = 0x98badcfeU;
    std::uint32_t d0 = 0x10325476U;
    for (std::size_t block = 0; block < bytes.size(); block += 64U) {
        std::array<std::uint32_t, 16> words{};
        for (unsigned i = 0; i < words.size(); ++i) {
            const auto offset = block + i * 4U;
            words[i] = static_cast<std::uint32_t>(bytes[offset]) |
                       (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
                       (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
                       (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
        }

        auto a = a0;
        auto b = b0;
        auto c = c0;
        auto d = d0;
        for (std::uint32_t i = 0; i < 64U; ++i) {
            std::uint32_t f;
            std::uint32_t g;
            if (i < 16U) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32U) {
                f = (d & b) | (~d & c);
                g = (5U * i + 1U) % 16U;
            } else if (i < 48U) {
                f = b ^ c ^ d;
                g = (3U * i + 5U) % 16U;
            } else {
                f = c ^ (b | ~d);
                g = (7U * i) % 16U;
            }
            const auto rotated = std::rotl(a + f + constants[i] + words[g], static_cast<int>(shifts[i]));
            a = d;
            d = c;
            c = b;
            b += rotated;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    // Java's Longs.fromBytes reads each eight-byte digest half in big-endian order.
    std::array<std::uint8_t, 16> digest{};
    const std::array<std::uint32_t, 4> state{a0, b0, c0, d0};
    for (std::size_t i = 0; i < state.size(); ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            digest[i * 4U + j] = static_cast<std::uint8_t>(state[i] >> (j * 8U));
        }
    }
    std::array<std::uint64_t, 2> result{};
    for (std::size_t half = 0; half < 2; ++half) {
        for (unsigned i = 0; i < 8; ++i) {
            result[half] = (result[half] << 8U) | digest[half * 8U + i];
        }
    }
    return result;
}

// Positional forks hash the same few hundred names for every seed.
const std::array<std::uint64_t, 2>& cached_md5_words_be(const std::string_view input) {
    thread_local std::unordered_map<std::string, std::array<std::uint64_t, 2>> cache;
    if (const auto it = cache.find(std::string(input)); it != cache.end()) {
        return it->second;
    }
    return cache.emplace(std::string(input), md5_words_be(input)).first->second;
}

std::uint32_t next_utf8_codepoint(const std::string_view text, std::size_t& offset) noexcept {
    const auto first = static_cast<std::uint8_t>(text[offset++]);
    if (first < 0x80U) {
        return first;
    }
    unsigned extra;
    std::uint32_t value;
    if ((first & 0xe0U) == 0xc0U) {
        extra = 1;
        value = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
        extra = 2;
        value = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
        extra = 3;
        value = first & 0x07U;
    } else {
        return 0xfffdU;
    }
    if (offset + extra > text.size()) {
        offset = text.size();
        return 0xfffdU;
    }
    for (unsigned i = 0; i < extra; ++i) {
        const auto byte = static_cast<std::uint8_t>(text[offset]);
        if ((byte & 0xc0U) != 0x80U) {
            return 0xfffdU;
        }
        ++offset;
        value = (value << 6U) | (byte & 0x3fU);
    }
    return value;
}

std::uint32_t java_string_hash(const std::string_view name) noexcept {
    std::uint32_t hash = 0;
    std::size_t offset = 0;
    while (offset < name.size()) {
        const auto codepoint = next_utf8_codepoint(name, offset);
        if (codepoint <= 0xffffU) {
            hash = hash * 31U + codepoint;
        } else {
            const auto adjusted = codepoint - 0x10000U;
            hash = hash * 31U + (0xd800U + (adjusted >> 10U));
            hash = hash * 31U + (0xdc00U + (adjusted & 0x3ffU));
        }
    }
    return hash;
}

}  // namespace

LegacyRandom::LegacyRandom(const std::uint64_t seed_bits) {
    set_seed(seed_bits);
}

void LegacyRandom::set_seed(const std::uint64_t seed_bits) noexcept {
    seed_ = (seed_bits ^ legacy_multiplier) & legacy_mask;
}

std::uint32_t LegacyRandom::next_bits(const unsigned bits) {
    if (bits == 0 || bits > 32) {
        throw std::invalid_argument("bits must be between 1 and 32");
    }
    seed_ = (seed_ * legacy_multiplier + legacy_increment) & legacy_mask;
    return static_cast<std::uint32_t>(seed_ >> (48U - bits));
}

std::int32_t LegacyRandom::next_int() {
    return std::bit_cast<std::int32_t>(next_bits(32));
}

std::int32_t LegacyRandom::next_int(const std::int32_t bound) {
    if (bound <= 0) {
        throw std::invalid_argument("bound must be positive");
    }

    const auto unsigned_bound = static_cast<std::uint32_t>(bound);
    if ((unsigned_bound & (unsigned_bound - 1U)) == 0) {
        return static_cast<std::int32_t>((static_cast<std::uint64_t>(unsigned_bound) * next_bits(31)) >> 31U);
    }

    for (;;) {
        const auto bits = next_bits(31);
        const auto value = bits % unsigned_bound;
        const auto java_overflow = bits - value + (unsigned_bound - 1U);
        if ((java_overflow & 0x80000000U) == 0) {
            return static_cast<std::int32_t>(value);
        }
    }
}

std::uint64_t LegacyRandom::next_long() {
    const auto high = next_bits(32);
    const auto low = std::bit_cast<std::int32_t>(next_bits(32));
    return (static_cast<std::uint64_t>(high) << 32U) + static_cast<std::uint64_t>(static_cast<std::int64_t>(low));
}

float LegacyRandom::next_float() {
    return static_cast<float>(next_bits(24)) * 5.9604645E-8F;
}

double LegacyRandom::next_double() {
    const auto bits = (static_cast<std::uint64_t>(next_bits(26)) << 27U) + next_bits(27);
    return static_cast<double>(bits) * 0x1.0p-53;
}

void LegacyRandom::consume_count(const std::int32_t rounds) {
    auto multiplier = legacy_multiplier;
    auto increment = legacy_increment;
    // Compose powers of the same affine LCG modulo 2^48, preserving every skipped state.
    for (auto remaining = rounds; remaining > 0; remaining >>= 1) {
        if ((remaining & 1) != 0)
            seed_ = (seed_ * multiplier + increment) & legacy_mask;
        increment = ((multiplier + 1) * increment) & legacy_mask;
        multiplier = (multiplier * multiplier) & legacy_mask;
    }
}

XoroshiroRandom::XoroshiroRandom(const std::uint64_t seed_bits) {
    set_seed(seed_bits);
}

XoroshiroRandom::XoroshiroRandom(const std::uint64_t seed_lo, const std::uint64_t seed_hi) noexcept
    : seed_lo_(seed_lo), seed_hi_(seed_hi) {
    if ((seed_lo_ | seed_hi_) == 0) {
        seed_lo_ = golden_ratio_64;
        seed_hi_ = silver_ratio_64;
    }
}

void XoroshiroRandom::set_seed(const std::uint64_t seed_bits) noexcept {
    seed_lo_ = upgrade_seed_lo(seed_bits);
    seed_hi_ = upgrade_seed_hi(seed_bits);
}

std::uint64_t XoroshiroRandom::next_long() noexcept {
    const auto lo = seed_lo_;
    auto hi = seed_hi_;
    const auto result = std::rotl(lo + hi, 17) + lo;
    hi ^= lo;
    seed_lo_ = std::rotl(lo, 49) ^ hi ^ (hi << 21U);
    seed_hi_ = std::rotl(hi, 28);
    return result;
}

std::uint32_t XoroshiroRandom::next_bits(const unsigned bits) {
    if (bits == 0 || bits > 32) {
        throw std::invalid_argument("bits must be between 1 and 32");
    }
    return static_cast<std::uint32_t>(next_long() >> (64U - bits));
}

std::int32_t XoroshiroRandom::next_int() {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(next_long()));
}

std::int32_t XoroshiroRandom::next_int(const std::int32_t bound) {
    if (bound <= 0) {
        throw std::invalid_argument("bound must be positive");
    }
    const auto unsigned_bound = static_cast<std::uint32_t>(bound);
    auto value = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(next_int()));
    auto product = value * unsigned_bound;
    auto low = static_cast<std::uint32_t>(product);
    if (low < unsigned_bound) {
        const auto threshold = (0U - unsigned_bound) % unsigned_bound;
        while (low < threshold) {
            value = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(next_int()));
            product = value * unsigned_bound;
            low = static_cast<std::uint32_t>(product);
        }
    }
    return static_cast<std::int32_t>(product >> 32U);
}

float XoroshiroRandom::next_float() {
    return static_cast<float>(next_bits(24)) * 5.9604645E-8F;
}

double XoroshiroRandom::next_double() {
    return static_cast<double>(next_long() >> 11U) * 0x1.0p-53;
}

void XoroshiroRandom::consume_count(const std::int32_t rounds) noexcept {
    for (std::int32_t i = 0; i < rounds; ++i) {
        static_cast<void>(next_long());
    }
}

RandomSource::RandomSource(const std::uint64_t seed_bits, const RandomAlgorithm algorithm)
    : source_(algorithm == RandomAlgorithm::legacy ? std::variant<LegacyRandom, XoroshiroRandom>(LegacyRandom(seed_bits))
                                                   : std::variant<LegacyRandom, XoroshiroRandom>(XoroshiroRandom(seed_bits))) {}

RandomSource::RandomSource(LegacyRandom source) noexcept : source_(std::move(source)) {}

RandomSource::RandomSource(XoroshiroRandom source) noexcept : source_(std::move(source)) {}

void RandomSource::set_seed(const std::uint64_t seed_bits) {
    std::visit([seed_bits](auto& source) { source.set_seed(seed_bits); }, source_);
}

std::uint32_t RandomSource::next_bits(const unsigned bits) {
    return std::visit([bits](auto& source) { return source.next_bits(bits); }, source_);
}

bool RandomSource::next_boolean() {
    if (auto* legacy = std::get_if<LegacyRandom>(&source_)) return legacy->next_bits(1) != 0;
    return (std::get<XoroshiroRandom>(source_).next_long() & 1U) != 0;
}

std::int32_t RandomSource::next_int() {
    return std::visit([](auto& source) { return source.next_int(); }, source_);
}

std::int32_t RandomSource::next_int(const std::int32_t bound) {
    return std::visit([bound](auto& source) { return source.next_int(bound); }, source_);
}

std::uint64_t RandomSource::next_long() {
    return std::visit([](auto& source) { return source.next_long(); }, source_);
}

float RandomSource::next_float() {
    return std::visit([](auto& source) { return source.next_float(); }, source_);
}

double RandomSource::next_double() {
    return std::visit([](auto& source) { return source.next_double(); }, source_);
}

void RandomSource::consume_count(const std::int32_t rounds) {
    std::visit([rounds](auto& source) { source.consume_count(rounds); }, source_);
}

PositionalFactory RandomSource::fork_positional() {
    if (auto* legacy = std::get_if<LegacyRandom>(&source_)) {
        return PositionalFactory(RandomAlgorithm::legacy, legacy->next_long());
    }
    auto& xoro = std::get<XoroshiroRandom>(source_);
    const auto seed_lo = xoro.next_long();
    const auto seed_hi = xoro.next_long();
    return PositionalFactory(RandomAlgorithm::xoroshiro, seed_lo, seed_hi);
}

PositionalFactory::PositionalFactory(const RandomAlgorithm algorithm, const std::uint64_t seed_lo,
                                     const std::uint64_t seed_hi) noexcept
    : algorithm_(algorithm), seed_lo_(seed_lo), seed_hi_(seed_hi) {}

RandomSource PositionalFactory::at(const std::int32_t x, const std::int32_t y, const std::int32_t z) const {
    const auto seed = position_seed(x, y, z);
    if (algorithm_ == RandomAlgorithm::legacy) {
        return RandomSource(LegacyRandom(seed ^ seed_lo_));
    }
    return RandomSource(XoroshiroRandom(seed ^ seed_lo_, seed_hi_));
}

RandomSource PositionalFactory::from_hash_of(const std::string_view name) const {
    if (algorithm_ == RandomAlgorithm::legacy) {
        return RandomSource(LegacyRandom(static_cast<std::uint64_t>(java_string_hash(name)) ^ seed_lo_));
    }
    const auto& words = cached_md5_words_be(name);
    return RandomSource(XoroshiroRandom(words[0] ^ seed_lo_, words[1] ^ seed_hi_));
}

std::uint64_t position_seed(const std::int32_t x, const std::int32_t y, const std::int32_t z) noexcept {
    // The first multiplication is an int operation in Java, so it wraps at 32 bits.
    const auto x_product_bits = static_cast<std::uint32_t>(x) * 3129871U;
    const auto x_product = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(x_product_bits)));
    auto value = x_product ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * 116129781ULL) ^
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    value = value * value * 42317861ULL + value * 11ULL;
    if ((value >> 63U) != 0) {
        return (value >> 16U) | 0xffff000000000000ULL;
    }
    return value >> 16U;
}

std::uint64_t large_feature_seed(const std::uint64_t seed, const std::int32_t chunk_x, const std::int32_t chunk_z) {
    LegacyRandom random(seed);
    const auto x_multiplier = random.next_long();
    const auto z_multiplier = random.next_long();
    return (static_cast<std::uint64_t>(static_cast<std::int64_t>(chunk_x)) * x_multiplier) ^
           (static_cast<std::uint64_t>(static_cast<std::int64_t>(chunk_z)) * z_multiplier) ^ seed;
}

}  // namespace minecraft::random
