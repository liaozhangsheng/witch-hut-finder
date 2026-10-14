#pragma once

#include <minecraft/biome.hpp>
#include <minecraft/types.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace minecraft::random {
class PositionalFactory;
}

namespace minecraft::worldgen {

[[nodiscard]] std::int64_t quantize_climate(float value) noexcept;

// The NoiseChunk a column is evaluated in. getBaseColumn and getBaseHeight build one per noise cell; chunk generation
// (fillFromNoise, whose aquifer the carvers also consult) builds one per chunk. Flat caches answer for their whole
// NoiseChunk, so off-column samples such as aquifer erosion and depth depend on which one it is.
enum class NoiseScope { cell, chunk };

// A seed-bound vanilla router. Climate uses direct contexts; columns retain NoiseChunk markers.
class DensitySampler {
public:
    DensitySampler(std::uint64_t seed, Dimension dimension, bool climate_only = false);
    ~DensitySampler();
    DensitySampler(DensitySampler&&) noexcept;
    DensitySampler& operator=(DensitySampler&&) noexcept;

    [[nodiscard]] double direct(std::string_view output, BlockPos position) const;
    // The router output's index, for the ColumnScope overloads that skip the lookup by name.
    [[nodiscard]] std::uint32_t output_index(std::string_view output) const;
    [[nodiscard]] biome::ClimateTarget climate(QuartPos position) const;
    [[nodiscard]] int preliminary_surface(int x, int z) const;
    [[nodiscard]] int preliminary_surface(int x, int z, int column_x, int column_z) const;
    // True when preliminary_surface ignores the column it is evaluated from, so callers may share results.
    [[nodiscard]] bool preliminary_surface_column_free() const;
    [[nodiscard]] int min_y() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] int cell_width() const;
    [[nodiscard]] int cell_height() const;
    [[nodiscard]] int sea_level() const;
    [[nodiscard]] bool aquifers_enabled() const;
    [[nodiscard]] bool ore_veins_enabled() const;
    [[nodiscard]] std::string_view default_block() const;
    [[nodiscard]] std::string_view default_fluid() const;
    [[nodiscard]] const random::PositionalFactory& aquifer_random() const;
    [[nodiscard]] const random::PositionalFactory& ore_random() const;

    // Memoizes evaluations for one column. Values equal the unscoped calls bit for bit. Columns of the same 16x16
    // area share cell corners through a per-thread cache, so a scope must stay on the thread that created it.
    class ColumnScope {
    public:
        ColumnScope(const DensitySampler& sampler, int x, int z, NoiseScope noise_scope = NoiseScope::cell);
        ~ColumnScope();
        ColumnScope(ColumnScope&&) noexcept;
        ColumnScope& operator=(ColumnScope&&) noexcept;

        [[nodiscard]] int x() const;
        [[nodiscard]] int z() const;

        // Visit the column from top to bottom, retaining cell caches; false stops the scan.
        void visit(std::string_view output, bool filling_cell, const std::function<bool(BlockPos, double)>& visitor);
        // The interpolated value of output at height y of this column.
        [[nodiscard]] double column(std::string_view output, int y, bool filling_cell = false);
        // output evaluated at position as a cell corner, sharing this column's caches.
        [[nodiscard]] double direct(std::string_view output, BlockPos position);
        [[nodiscard]] double direct(std::uint32_t output, BlockPos position);
        // Same as preliminary_surface(x, z, column x, column z).
        [[nodiscard]] int preliminary_surface(int x, int z);
        // Supplies the value direct(output, position) returns, so later evaluations reuse it instead of sampling
        // noise. Only memoized y-free outputs, such as the flat-cached climate, take it; others ignore it. The
        // value must be exactly the one this scope would compute.
        void remember(std::uint32_t output, BlockPos position, double value);

    private:
        struct State;
        const DensitySampler* sampler_;
        std::unique_ptr<State> state_;
    };

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace minecraft::worldgen
