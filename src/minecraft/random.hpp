#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

namespace minecraft::random {

enum class RandomAlgorithm { legacy, xoroshiro };

class PositionalFactory;

class LegacyRandom {
public:
    explicit LegacyRandom(std::uint64_t seed_bits);

    void set_seed(std::uint64_t seed_bits) noexcept;
    [[nodiscard]] std::uint32_t next_bits(unsigned bits);
    [[nodiscard]] std::int32_t next_int();
    [[nodiscard]] std::int32_t next_int(std::int32_t bound);
    [[nodiscard]] std::uint64_t next_long();
    [[nodiscard]] float next_float();
    [[nodiscard]] double next_double();
    void consume_count(std::int32_t rounds);

private:
    std::uint64_t seed_{};
};

class XoroshiroRandom {
public:
    explicit XoroshiroRandom(std::uint64_t seed_bits);
    XoroshiroRandom(std::uint64_t seed_lo, std::uint64_t seed_hi) noexcept;

    void set_seed(std::uint64_t seed_bits) noexcept;
    [[nodiscard]] std::uint64_t next_long() noexcept;
    [[nodiscard]] std::uint32_t next_bits(unsigned bits);
    [[nodiscard]] std::int32_t next_int();
    [[nodiscard]] std::int32_t next_int(std::int32_t bound);
    [[nodiscard]] float next_float();
    [[nodiscard]] double next_double();
    void consume_count(std::int32_t rounds) noexcept;

private:
    std::uint64_t seed_lo_{};
    std::uint64_t seed_hi_{};
};

class RandomSource {
public:
    explicit RandomSource(std::uint64_t seed_bits, RandomAlgorithm algorithm = RandomAlgorithm::legacy);
    explicit RandomSource(LegacyRandom source) noexcept;
    explicit RandomSource(XoroshiroRandom source) noexcept;

    void set_seed(std::uint64_t seed_bits);
    [[nodiscard]] std::uint32_t next_bits(unsigned bits);
    [[nodiscard]] std::int32_t next_int();
    [[nodiscard]] std::int32_t next_int(std::int32_t bound);
    [[nodiscard]] std::uint64_t next_long();
    [[nodiscard]] float next_float();
    [[nodiscard]] double next_double();
    // Legacy draws one bit; Xoroshiro tests the low bit of a long.
    [[nodiscard]] bool next_boolean();
    void consume_count(std::int32_t rounds);
    [[nodiscard]] PositionalFactory fork_positional();

private:
    std::variant<LegacyRandom, XoroshiroRandom> source_;
};

class PositionalFactory {
public:
    [[nodiscard]] RandomSource at(std::int32_t x, std::int32_t y, std::int32_t z) const;
    [[nodiscard]] RandomSource from_hash_of(std::string_view name) const;

private:
    friend class RandomSource;
    PositionalFactory(RandomAlgorithm algorithm, std::uint64_t seed_lo, std::uint64_t seed_hi = 0) noexcept;

    RandomAlgorithm algorithm_;
    std::uint64_t seed_lo_;
    std::uint64_t seed_hi_;
};

[[nodiscard]] std::uint64_t position_seed(std::int32_t x, std::int32_t y, std::int32_t z) noexcept;

[[nodiscard]] std::uint64_t large_feature_seed(std::uint64_t seed, std::int32_t chunk_x, std::int32_t chunk_z);
[[nodiscard]] inline std::uint64_t large_feature_with_salt_seed(const std::uint64_t seed, const std::int32_t region_x,
                                                             const std::int32_t region_z,
                                                             const std::int32_t salt) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(region_x)) * 341873128712ULL +
           static_cast<std::uint64_t>(static_cast<std::int64_t>(region_z)) * 132897987541ULL + seed +
           static_cast<std::uint64_t>(static_cast<std::int64_t>(salt));
}

}  // namespace minecraft::random
