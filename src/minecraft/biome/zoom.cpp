#include <minecraft/biome_zoom.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace minecraft::biome {
namespace {

// BiomeManager uses Guava Hashing.sha256().hashLong(seed).asLong(). Both the
// hash input and HashCode.asLong() are little-endian; this one-block SHA-256
// specialization avoids a platform crypto dependency.
std::array<std::uint8_t, 32> sha256_seed(const std::uint64_t seed) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 8; ++i) {
        const auto byte = static_cast<std::uint8_t>(seed >> (i * 8U));
        const auto word = i / 4;
        words[word] |= static_cast<std::uint32_t>(byte) << (24U - static_cast<unsigned>(i % 4) * 8U);
    }
    words[2] = 0x80000000U;
    words[15] = 64U;
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3U);
        const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto temp1 = h + sum1 + choice + constants[i] + words[i];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[i * 4 + j] = static_cast<std::uint8_t>(state[i] >> (24U - static_cast<unsigned>(j) * 8U));
        }
    }
    return digest;
}

std::uint64_t lcg_next(std::uint64_t value, const std::uint64_t addend) {
    value *= value * 6364136223846793005ULL + 1442695040888963407ULL;
    return value + addend;
}

double fiddle(const std::uint64_t value) {
    const auto bucket = (value >> 24U) & 1023U;
    return (static_cast<double>(bucket) / 1024.0 - 0.5) * 0.9;
}

double fiddled_distance(const std::uint64_t seed, const std::int32_t x, const std::int32_t y,
                        const std::int32_t z, const double dx, const double dy, const double dz) {
    auto value = seed;
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(x)));
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(y)));
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(z)));
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(x)));
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(y)));
    value = lcg_next(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(z)));
    const double fiddle_x = fiddle(value);
    value = lcg_next(value, seed);
    const double fiddle_y = fiddle(value);
    value = lcg_next(value, seed);
    const double fiddle_z = fiddle(value);
    return (dz + fiddle_z) * (dz + fiddle_z) + (dy + fiddle_y) * (dy + fiddle_y) +
           (dx + fiddle_x) * (dx + fiddle_x);
}

std::int32_t wrap_subtract_two(const std::int32_t value) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value) - 2U);
}

std::int32_t wrap_increment(const std::int32_t value) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value) + 1U);
}

}  // namespace

std::uint64_t obfuscate_seed(const std::uint64_t world_seed) {
    const auto digest = sha256_seed(world_seed);
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(digest[i]) << (i * 8U);
    }
    return result;
}

QuartPos zoom_quart(const std::uint64_t obfuscated_seed, const BlockPos pos) {
    // Java BlockPos arithmetic wraps in 32 bits before the arithmetic shift.
    const std::int32_t shifted_x = wrap_subtract_two(pos.x);
    const std::int32_t shifted_y = wrap_subtract_two(pos.y);
    const std::int32_t shifted_z = wrap_subtract_two(pos.z);
    const std::int32_t base_x = floor_div(shifted_x, 4);
    const std::int32_t base_y = floor_div(shifted_y, 4);
    const std::int32_t base_z = floor_div(shifted_z, 4);
    const double frac_x = static_cast<double>(std::bit_cast<std::uint32_t>(shifted_x) & 3U) / 4.0;
    const double frac_y = static_cast<double>(std::bit_cast<std::uint32_t>(shifted_y) & 3U) / 4.0;
    const double frac_z = static_cast<double>(std::bit_cast<std::uint32_t>(shifted_z) & 3U) / 4.0;

    std::int32_t closest = 0;
    double best_distance = INFINITY;
    for (std::int32_t index = 0; index < 8; ++index) {
        const bool use_base_x = (index & 4) == 0;
        const bool use_base_y = (index & 2) == 0;
        const bool use_base_z = (index & 1) == 0;
        const auto x = use_base_x ? base_x : wrap_increment(base_x);
        const auto y = use_base_y ? base_y : wrap_increment(base_y);
        const auto z = use_base_z ? base_z : wrap_increment(base_z);
        const double dx = use_base_x ? frac_x : frac_x - 1.0;
        const double dy = use_base_y ? frac_y : frac_y - 1.0;
        const double dz = use_base_z ? frac_z : frac_z - 1.0;
        const double distance = fiddled_distance(obfuscated_seed, x, y, z, dx, dy, dz);
        if (distance < best_distance) {
            closest = index;
            best_distance = distance;
        }
    }

    return {(closest & 4) == 0 ? base_x : wrap_increment(base_x),
            (closest & 2) == 0 ? base_y : wrap_increment(base_y),
            (closest & 1) == 0 ? base_z : wrap_increment(base_z)};
}

}  // namespace minecraft::biome
