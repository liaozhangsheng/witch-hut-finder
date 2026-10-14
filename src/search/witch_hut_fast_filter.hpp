#pragma once

#include "witch_hut_batch.hpp"
#include "witch_hut_climate.hpp"
#include "witch_hut_fast_noise.hpp"
#include "witch_hut_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace witch_hut {

// shortcut: coarse probes can miss local swamp patches and lower huts.
class FastFilter {
public:
    static constexpr int tile_width = 32;  // Placement regions per side; each region spans 512 blocks.
    static constexpr double tile_erosion_min = 0.30;
    static constexpr double candidate_erosion_min = 0.45;
    // Bound on how far the offset warp moves the proxy: the largest shift seen over 4.2 million candidates of five
    // seeds was 0.027 (99.99th percentile 0.016), so an unwarped value this far below the threshold rejects.
    static constexpr double warp_margin = 0.06;
    // Beyond this distance from the threshold the batched warped estimate decides; its error stays near 0.001.
    static constexpr double decision_band = 0.01;
    // The climate filter rejects temperatures above 0.2, and below -0.45 when Certificate::below_box holds. The
    // estimate at the sparse warp stayed within 0.01 of the
    // exact temperature over millions of candidates of several seeds.
    static constexpr double temperature_margin = 0.03;

    explicit FastFilter(std::uint64_t seed)
        : erosion_(seed), climate_(seed), below_box_(detail::certificate().valid && detail::certificate().below_box),
          temperature_min_(detail::certificate().swamp[0].climate.temperature_min / 10000.0) {
        if (erosion_.approximate()) {
            unwarped_.emplace(*erosion_.unwarped_noise(), erosion_.unwarped_factor());
            offset_.emplace(*erosion_.offset_noise(), erosion_.offset_factor());
            if (const auto* temperature = climate_.axis_noise(Climate::temperature))
                temperature_.emplace(*temperature, 1.0);
        }
    }

    // screen() results per candidate.
    enum : std::uint8_t { rejected = 0, kept = 1, off_climate = 2 };

    // keep[i] = possible(xs[i], zs[i]) for i < count, mostly from batched estimates: the unwarped one screens, the
    // warped one decides away from the threshold, and the exact proxy decides within decision_band of it. A kept
    // candidate whose estimated temperature lies past one of the climate filter's cuts by temperature_margin becomes
    // off_climate.
    void screen(const std::int32_t* xs, const std::int32_t* zs, std::size_t count, std::vector<std::uint8_t>& keep) {
        keep.assign(count, kept);
        if (!unwarped_)
            return;
        const auto padded = (count + NoiseBatch::lanes - 1) / NoiseBatch::lanes * NoiseBatch::lanes;
        const auto fill = [&](std::vector<double>& out, std::size_t used) {
            out.resize(padded);
            for (std::size_t i = used; i < padded; ++i)
                out[i] = used ? out[used - 1] : 0.0;
        };
        quart_x_.resize(count);
        quart_z_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            quart_x_[i] = xs[i] * 0.25;
            quart_z_[i] = zs[i] * 0.25;
        }
        fill(quart_x_, count);
        fill(quart_z_, count);
        estimates_.resize(padded);
        unwarped_->estimate(quart_x_.data(), nullptr, quart_z_.data(), padded, estimates_.data());
        // Survivors of the unwarped screen, compacted for the warped estimate.
        survivors_.clear();
        warped_x_.clear();
        warped_z_.clear();
        for (std::size_t i = 0; i < count; ++i) {
            if (estimates_[i] < candidate_erosion_min - warp_margin - NoiseBatch::max_error) {
                keep[i] = rejected;
                continue;
            }
            survivors_.push_back(i);
            warped_x_.push_back(quart_x_[i]);
            warped_z_.push_back(quart_z_[i]);
        }
        if (survivors_.empty())
            return;
        const auto used = survivors_.size();
        const auto warped_padded = (used + NoiseBatch::lanes - 1) / NoiseBatch::lanes * NoiseBatch::lanes;
        const auto pad = [&](std::vector<double>& out) {
            out.resize(warped_padded, out[used - 1]);
        };
        pad(warped_x_);
        pad(warped_z_);
        zeros_.assign(warped_padded, 0.0);
        shift_x_.resize(warped_padded);
        shift_z_.resize(warped_padded);
        offset_->estimate(warped_x_.data(), nullptr, warped_z_.data(), warped_padded, shift_x_.data());
        offset_->estimate(warped_z_.data(), warped_x_.data(), zeros_.data(), warped_padded, shift_z_.data());
        for (std::size_t i = 0; i < warped_padded; ++i) {
            warped_x_[i] += shift_x_[i];
            warped_z_[i] += shift_z_[i];
        }
        unwarped_->estimate(warped_x_.data(), nullptr, warped_z_.data(), warped_padded, estimates_.data());
        // The sparse warp sits close to the climate's full one, and temperature varies over thousands of blocks.
        if (temperature_) {
            temperatures_.resize(warped_padded);
            temperature_->estimate(warped_x_.data(), nullptr, warped_z_.data(), warped_padded, temperatures_.data());
        }
        for (std::size_t s = 0; s < used; ++s) {
            const auto i = survivors_[s];
            const double estimate = estimates_[s];
            if (estimate < candidate_erosion_min - decision_band) {
                keep[i] = rejected;
                continue;
            }
            if (estimate < candidate_erosion_min + decision_band) {
                const auto erosion = erosion_.sample(xs[i], zs[i]);
                if (std::isfinite(erosion) && erosion < candidate_erosion_min) {
                    keep[i] = rejected;
                    continue;
                }
            }
            if (temperature_ && (temperatures_[s] > 0.2 + temperature_margin ||
                                 (below_box_ && temperatures_[s] < temperature_min_ - temperature_margin)))
                keep[i] = off_climate;
        }
    }

    // Cheap approximate candidate gate. The exact climate filter and structure verification follow this check.
    [[nodiscard]] bool possible(std::int32_t x, std::int32_t z) const {
        if (!erosion_.approximate())
            return true;
        if (erosion_.sample_unwarped(x, z) < candidate_erosion_min - warp_margin)
            return false;
        const auto erosion = erosion_.sample(x, z);
        return !std::isfinite(erosion) || erosion >= candidate_erosion_min;
    }

    // Tile coordinates are placement-region coordinates. Partial tiles at the search border are supported.
    // Use globally aligned 32-region tile starts for repeatable results. A tile is kept when any of four coarse
    // erosion probes is at least 0.30; the lower threshold allows for spatial variation and proxy error.
    [[nodiscard]] bool tile_possible(int min_rx, int min_rz, int width, int height) const {
        if (!erosion_.approximate())
            return true;
        if (width <= 0 || height <= 0 || width > tile_width || height > tile_width)
            return true;

        constexpr int sample_side = 2;
        const std::int64_t base_x = static_cast<std::int64_t>(min_rx) * 512 + 192;
        const std::int64_t base_z = static_cast<std::int64_t>(min_rz) * 512 + 192;
        const std::int64_t span_x = static_cast<std::int64_t>(width - 1) * 512;
        const std::int64_t span_z = static_cast<std::int64_t>(height - 1) * 512;

        for (int ix = 0; ix < sample_side; ++ix) {
            const auto x = base_x + (2 * ix + 1) * span_x / (2 * sample_side);
            if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max())
                return true;
            for (int iz = 0; iz < sample_side; ++iz) {
                const auto z = base_z + (2 * iz + 1) * span_z / (2 * sample_side);
                if (z < std::numeric_limits<std::int32_t>::min() || z > std::numeric_limits<std::int32_t>::max())
                    return true;
                const auto erosion = erosion_.sample(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
                if (!std::isfinite(erosion) || erosion >= tile_erosion_min)
                    return true;
            }
        }
        return false;
    }

private:
    FastErosion erosion_;
    Climate climate_;
    std::optional<NoiseBatch> unwarped_, offset_, temperature_;
    bool below_box_;
    double temperature_min_;
    std::vector<double> quart_x_, quart_z_, warped_x_, warped_z_, zeros_;
    std::vector<float> estimates_, shift_x_, shift_z_, temperatures_;
    std::vector<std::size_t> survivors_;
};

}  // namespace witch_hut
