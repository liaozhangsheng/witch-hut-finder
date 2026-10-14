#pragma once

#include "witch_hut_climate.hpp"
#include "minecraft/worldgen/program.hpp"
#include "minecraft/worldgen/rules.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace witch_hut {
namespace detail {
constexpr std::int32_t max_safe_climate = 1000000;

struct Box {
    std::int32_t temperature_min, temperature_max;
    std::int32_t continents_min, continents_max;
    std::int32_t erosion_min, erosion_max;
    std::int32_t ridges_min, ridges_max;
    friend bool operator==(const Box&, const Box&) = default;
};

struct Parameter {
    std::string_view biome;
    Box climate;
    std::int32_t humidity_min, humidity_max;
    std::int32_t depth_min, depth_max;
    std::int64_t offset;
};

struct Target {
    Box climate;
    std::int32_t humidity_min, humidity_max;
    std::int32_t depth;
    std::int64_t offset;
    std::vector<Box> before_ridges;
    std::vector<Box> after_ridges;
};

// Rival boxes in struct-of-arrays form: temperature, continents, erosion and ridges bounds. They are kept as doubles:
// every bound and squared distance here is an integer below 2^53, so the arithmetic stays exact and vectorizes.
struct Rivals {
    std::array<std::vector<double>, 4> min, max;
    void add(const Box& box) {
        const std::array<std::pair<std::int32_t, std::int32_t>, 4> axes{{{box.temperature_min, box.temperature_max},
                                                                          {box.continents_min, box.continents_max},
                                                                          {box.erosion_min, box.erosion_max},
                                                                          {box.ridges_min, box.ridges_max}}};
        for (std::size_t a = 0; a < axes.size(); ++a) {
            min[a].push_back(axes[a].first);
            max[a].push_back(axes[a].second);
        }
    }
};

struct Certificate {
    bool valid = false;
    bool common_after_ridges = false;
    // Every point below the swamp box's temperature, continents or erosion minimum has a strictly closer rival,
    // whatever its other coordinates; see cut_certified. Above the temperature maximum is part of valid.
    bool below_box = false;
    std::array<Target, 14> swamp{};
    // Once humidity is known, a rival need only cover it rather than every humidity: per band between consecutive
    // humidity bounds of the table, the rivals covering the band and every swamp depth point, nearest first. Every
    // swamp target shares the list, or the stage stays off.
    bool humidity_valid = false;
    std::vector<std::int32_t> humidity_bounds;
    std::vector<Rivals> humidity_rivals;
    // swamp[i].after_ridges as Rivals.
    std::array<Rivals, 14> after_rivals;
    // Every swamp point shares its temperature, continents and erosion box, so the ridge-free distance is the same for
    // all of them: the union of their before_ridges lists, with a mask of the points each rival belongs to, answers
    // them in one pass.
    Rivals before_union;
    std::vector<std::uint32_t> before_points;
};

constexpr std::array<std::pair<std::int32_t, std::int32_t>, 7> swamp_ridges{{
    {-10000, -9333},
    {-4000, -2666},
    {-2666, -500},
    {-500, 500},
    {500, 2666},
    {2666, 4000},
    {9333, 10000},
}};

bool integer(const minecraft::worldgen::Json& value, std::int64_t& out) {
    const auto number = minecraft::worldgen::number(value);
    if (!std::isfinite(number) || std::trunc(number) != number || number < -0x1p53 || number > 0x1p53)
        return false;
    out = static_cast<std::int64_t>(number);
    return true;
}

bool interval(const minecraft::worldgen::Json& minimum, const minecraft::worldgen::Json& maximum, std::size_t axis, std::int32_t& lo,
              std::int32_t& hi) {
    const auto& lows = std::get<minecraft::worldgen::JsonArray>(minimum);
    const auto& highs = std::get<minecraft::worldgen::JsonArray>(maximum);
    if (lows.size() != 6 || highs.size() != 6)
        return false;
    std::int64_t low, high;
    if (!integer(lows[axis], low) || !integer(highs[axis], high) || low < std::numeric_limits<std::int32_t>::min() ||
        low > std::numeric_limits<std::int32_t>::max() || high < std::numeric_limits<std::int32_t>::min() ||
        high > std::numeric_limits<std::int32_t>::max() || low > high)
        return false;
    lo = static_cast<std::int32_t>(low);
    hi = static_cast<std::int32_t>(high);
    return true;
}

bool parameter(const minecraft::worldgen::Json& item, Parameter& out) {
    const auto& minimum = minecraft::worldgen::field(item, "minimum");
    const auto& maximum = minecraft::worldgen::field(item, "maximum");
    if (!interval(minimum, maximum, 0, out.climate.temperature_min, out.climate.temperature_max) ||
        !interval(minimum, maximum, 1, out.humidity_min, out.humidity_max) ||
        !interval(minimum, maximum, 2, out.climate.continents_min, out.climate.continents_max) ||
        !interval(minimum, maximum, 3, out.climate.erosion_min, out.climate.erosion_max) ||
        !interval(minimum, maximum, 4, out.depth_min, out.depth_max) ||
        !interval(minimum, maximum, 5, out.climate.ridges_min, out.climate.ridges_max))
        return false;
    // Keep all later squared box distances comfortably within int64_t.
    if (out.climate.temperature_min < -max_safe_climate || out.climate.temperature_max > max_safe_climate ||
        out.climate.continents_min < -max_safe_climate || out.climate.continents_max > max_safe_climate ||
        out.climate.erosion_min < -max_safe_climate || out.climate.erosion_max > max_safe_climate ||
        out.climate.ridges_min < -max_safe_climate || out.climate.ridges_max > max_safe_climate)
        return false;
    out.biome = minecraft::worldgen::string(minecraft::worldgen::field(item, "biome_id"));
    return integer(minecraft::worldgen::field(item, "offset"), out.offset);
}

constexpr std::int64_t gap(std::int32_t a_min, std::int32_t a_max, std::int32_t b_min, std::int32_t b_max) {
    const auto delta = a_max < b_min ? std::int64_t(b_min) - a_max : b_max < a_min ? std::int64_t(a_min) - b_max : 0;
    return delta * delta;
}

void build_humidity_stage(const std::vector<Parameter>& parameters, Certificate& result) {
    std::vector<std::int32_t> bounds;
    for (const auto& p : parameters) {
        bounds.push_back(p.humidity_min);
        bounds.push_back(p.humidity_max);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    // Swamp spans every humidity, so clamping an outside value to the ends keeps each listed rival at most as far.
    if (bounds.size() < 2 || bounds.front() != result.swamp[0].humidity_min ||
        bounds.back() != result.swamp[0].humidity_max)
        return;
    std::vector<Rivals> bands;
    for (std::size_t b = 0; b + 1 < bounds.size(); ++b) {
        std::vector<Box> common;
        for (std::size_t t = 0; t < result.swamp.size(); ++t) {
            const auto& target = result.swamp[t];
            std::vector<Box> rivals;
            for (const auto& p : parameters)
                if (p.biome != "minecraft:swamp" && p.offset == target.offset && p.humidity_min <= bounds[b] &&
                    p.humidity_max >= bounds[b + 1] && p.depth_min <= target.depth && p.depth_max >= target.depth)
                    rivals.push_back(p.climate);
            if (t == 0)
                common = std::move(rivals);
            else if (rivals != common)
                return;
        }
        // Rivals bordering the swamp box dominate most often; trying them first ends rejections early.
        const auto& swamp = result.swamp[0].climate;
        const auto closeness = [&](const Box& box) {
            return gap(box.temperature_min, box.temperature_max, swamp.temperature_min, swamp.temperature_max) +
                   gap(box.continents_min, box.continents_max, swamp.continents_min, swamp.continents_max) +
                   gap(box.erosion_min, box.erosion_max, swamp.erosion_min, swamp.erosion_max);
        };
        std::stable_sort(common.begin(), common.end(),
                         [&](const Box& a, const Box& b) { return closeness(a) < closeness(b); });
        Rivals band;
        for (const auto& box : common)
            band.add(box);
        bands.push_back(std::move(band));
    }
    result.humidity_bounds = std::move(bounds);
    result.humidity_rivals = std::move(bands);
    result.humidity_valid = true;
}

// A parameter's ranges over temperature, humidity, continents, erosion and ridges.
struct Ranges {
    std::array<std::int32_t, 5> min, max;
};

inline Ranges ranges(const Box& box, std::int32_t humidity_min, std::int32_t humidity_max) {
    return {{box.temperature_min, humidity_min, box.continents_min, box.erosion_min, box.ridges_min},
            {box.temperature_max, humidity_max, box.continents_max, box.erosion_max, box.ridges_max}};
}

// Whether every point below every swamp target's minimum on axis (an index into Ranges) has a strictly closer rival,
// whatever its other coordinates. The table's bounds cut each axis into closed cells. On another axis a rival is no
// farther than the target over a whole cell when the cell lies in the rival's range, or when the rival holds the
// target's nearest point to every value of the cell, which spans [clamp(low), clamp(high)]. On the cut axis, a rival
// reaching the cell's top whose minimum lies below the target's is strictly closer to each value under that minimum.
// The rival's depth range holds the target's depth point, so it is no farther in depth either.
bool cut_certified(const std::vector<Parameter>& parameters, const Certificate& result, std::size_t axis) {
    std::array<std::vector<std::int32_t>, 5> bounds;
    for (const auto& p : parameters) {
        const auto r = ranges(p.climate, p.humidity_min, p.humidity_max);
        for (std::size_t a = 0; a < bounds.size(); ++a) {
            bounds[a].push_back(r.min[a]);
            bounds[a].push_back(r.max[a]);
        }
    }
    for (auto& b : bounds) {
        // Climate outside the safe range fails open in Filter::possible, so the cells need not reach further.
        b.push_back(-max_safe_climate);
        b.push_back(max_safe_climate);
        std::sort(b.begin(), b.end());
        b.erase(std::unique(b.begin(), b.end()), b.end());
    }
    for (const auto& target : result.swamp) {
        const auto t = ranges(target.climate, target.humidity_min, target.humidity_max);
        std::vector<Ranges> rivals;
        for (const auto& p : parameters)
            if (p.biome != "minecraft:swamp" && p.offset == target.offset && p.depth_min <= target.depth &&
                p.depth_max >= target.depth)
                rivals.push_back(ranges(p.climate, p.humidity_min, p.humidity_max));
        const auto& cut = bounds[axis];
        for (std::size_t c = 0; c + 1 < cut.size() && cut[c] < t.min[axis]; ++c) {
            const auto top = std::min(cut[c + 1], t.min[axis] - 1);
            std::vector<const Ranges*> closer_on_cut;
            for (const auto& r : rivals)
                if (r.max[axis] >= top && r.min[axis] < t.min[axis])
                    closer_on_cut.push_back(&r);
            std::array<std::size_t, 5> cell{};
            // Neighbouring cells mostly share a covering rival, so each search starts from the last one found.
            std::size_t last = 0;
            const auto covered = [&] {
                for (std::size_t k = 0; k < closer_on_cut.size(); ++k) {
                    const auto* r = closer_on_cut[(last + k) % closer_on_cut.size()];
                    bool closer = true;
                    for (std::size_t a = 0; a < cell.size() && closer; ++a) {
                        if (a == axis)
                            continue;
                        const auto low = bounds[a][cell[a]], high = bounds[a][cell[a] + 1];
                        const auto near_low = std::clamp(low, t.min[a], t.max[a]);
                        const auto near_high = std::clamp(high, t.min[a], t.max[a]);
                        closer = (r->min[a] <= low && r->max[a] >= high) ||
                                 (r->min[a] <= near_low && r->max[a] >= near_high);
                    }
                    if (closer) {
                        last = (last + k) % closer_on_cut.size();
                        return true;
                    }
                }
                return false;
            };
            const auto every_cell = [&](const auto& self, std::size_t a) -> bool {
                if (a == cell.size())
                    return covered();
                if (a == axis)
                    return self(self, a + 1);
                for (cell[a] = 0; cell[a] + 1 < bounds[a].size(); ++cell[a])
                    if (!self(self, a + 1))
                        return false;
                return true;
            };
            if (!every_cell(every_cell, 0))
                return false;
        }
    }
    return true;
}

Certificate build_certificate() {
    Certificate result;
    std::vector<Parameter> parameters;
    const auto& rows =
        std::get<minecraft::worldgen::JsonArray>(minecraft::worldgen::field(minecraft::worldgen::rules(), "overworld_parameters"));
    parameters.reserve(rows.size());
    for (const auto& row : rows) {
        Parameter p{};
        if (!parameter(row, p))
            return result;
        parameters.push_back(p);
    }

    std::array<std::array<bool, 2>, swamp_ridges.size()> seen{};
    std::size_t count = 0;
    for (const auto& p : parameters) {
        if (p.biome != "minecraft:swamp")
            continue;
        if (count == result.swamp.size() || p.climate.temperature_min != -4500 || p.climate.temperature_max != 2000 ||
            p.humidity_min != -10000 || p.humidity_max != 10000 || p.climate.continents_min != -1100 ||
            p.climate.continents_max != 10000 || p.climate.erosion_min != 5500 || p.climate.erosion_max != 10000 ||
            p.offset != 0)
            return result;
        std::size_t band = swamp_ridges.size();
        for (std::size_t i = 0; i < swamp_ridges.size(); ++i)
            if (p.climate.ridges_min == swamp_ridges[i].first && p.climate.ridges_max == swamp_ridges[i].second)
                band = i;
        const std::size_t depth = p.depth_min == 0 && p.depth_max == 0           ? 0
                                  : p.depth_min == 10000 && p.depth_max == 10000 ? 1
                                                                                 : 2;
        if (band == swamp_ridges.size() || depth == 2 || seen[band][depth])
            return result;
        seen[band][depth] = true;
        auto& target = result.swamp[band * 2 + depth];
        target = {p.climate, p.humidity_min, p.humidity_max, p.depth_min, p.offset, {}, {}};
        ++count;
    }
    if (count != result.swamp.size())
        return result;
    for (const auto& pair : seen)
        if (!pair[0] || !pair[1])
            return result;

    bool temperature_cut_certified = true;
    for (auto& target : result.swamp) {
        bool covered_above_swamp = false;
        for (const auto& p : parameters) {
            if (p.biome == "minecraft:swamp" || p.offset != target.offset || p.humidity_min > target.humidity_min ||
                p.humidity_max < target.humidity_max || p.depth_min > target.depth || p.depth_max < target.depth)
                continue;
            if (p.climate.temperature_min == 2000 && p.climate.temperature_max == 10000 &&
                p.climate.continents_min <= target.climate.continents_min &&
                p.climate.continents_max >= target.climate.continents_max &&
                p.climate.erosion_min <= target.climate.erosion_min &&
                p.climate.erosion_max >= target.climate.erosion_max &&
                p.climate.ridges_min <= target.climate.ridges_min && p.climate.ridges_max >= target.climate.ridges_max)
                covered_above_swamp = true;
            target.after_ridges.push_back(p.climate);
            if (p.climate.ridges_min <= target.climate.ridges_min && p.climate.ridges_max >= target.climate.ridges_max)
                target.before_ridges.push_back(p.climate);
        }
        temperature_cut_certified = temperature_cut_certified && covered_above_swamp;
        if (target.before_ridges.empty() || target.after_ridges.empty())
            return result;
    }
    for (std::size_t band = 0; band < swamp_ridges.size(); ++band) {
        const auto& shallow = result.swamp[band * 2];
        const auto& deep = result.swamp[band * 2 + 1];
        if (!(shallow.climate == deep.climate) || shallow.humidity_min != deep.humidity_min ||
            shallow.humidity_max != deep.humidity_max || shallow.offset != deep.offset ||
            shallow.before_ridges != deep.before_ridges || shallow.after_ridges != deep.after_ridges)
            return result;
    }
    if (!temperature_cut_certified)
        return result;
    result.common_after_ridges = true;
    for (std::size_t i = 1; i < result.swamp.size(); ++i)
        if (result.swamp[i].after_ridges != result.swamp[0].after_ridges)
            result.common_after_ridges = false;
    result.valid = true;
    std::vector<Box> union_boxes;
    for (std::size_t i = 0; i < result.swamp.size(); ++i) {
        for (const auto& box : result.swamp[i].after_ridges)
            result.after_rivals[i].add(box);
        for (const auto& box : result.swamp[i].before_ridges) {
            const auto found = std::find(union_boxes.begin(), union_boxes.end(), box);
            if (found == union_boxes.end()) {
                union_boxes.push_back(box);
                result.before_points.push_back(0);
            }
            result.before_points[static_cast<std::size_t>(std::find(union_boxes.begin(), union_boxes.end(), box) -
                                                          union_boxes.begin())] |= 1U << i;
        }
    }
    for (const auto& box : union_boxes)
        result.before_union.add(box);
    build_humidity_stage(parameters, result);
    // Ranges axes: temperature 0, continents 2, erosion 3.
    result.below_box = cut_certified(parameters, result, 0) && cut_certified(parameters, result, 2) &&
                       cut_certified(parameters, result, 3);
    return result;
}

const Certificate& certificate() {
    static const Certificate value = [] {
        try {
            return build_certificate();
        } catch (...) {
            // If frozen rules change shape or cease to match this proof, filtering fails open.
            return Certificate{};
        }
    }();
    return value;
}

constexpr std::int64_t distance(std::int32_t value, std::int32_t minimum, std::int32_t maximum) {
    const auto delta = value < minimum   ? std::int64_t(minimum) - value
                       : value > maximum ? std::int64_t(value) - maximum
                                         : 0;
    return delta * delta;
}

constexpr bool climate_in_safe_range(std::int64_t value) {
    return value >= -max_safe_climate && value <= max_safe_climate;
}

constexpr std::int64_t distance(const Box& box, std::int32_t temperature, std::int32_t continents, std::int32_t erosion,
                                std::int32_t ridges, bool with_ridges) {
    return distance(temperature, box.temperature_min, box.temperature_max) +
           distance(continents, box.continents_min, box.continents_max) +
           distance(erosion, box.erosion_min, box.erosion_max) +
           (with_ridges ? distance(ridges, box.ridges_min, box.ridges_max) : 0);
}

// Whether a rival is strictly closer than swamp distance swamp, over the first three axes or all four. Blocks of
// rivals are tested whole so the arithmetic vectorizes; the scan stops after the first block holding a closer one.
inline bool dominated(const Rivals& rivals, std::int64_t swamp, std::int32_t temperature, std::int32_t continents,
                      std::int32_t erosion, std::int32_t ridges, bool with_ridges) {
    if (swamp == 0)
        return false;  // Squared distances cannot be strictly less than zero.
    const std::array<double, 4> values{double(temperature), double(continents), double(erosion), double(ridges)};
    const double bound = static_cast<double>(swamp);
    const std::size_t axes = with_ridges ? 4 : 3, count = rivals.min[0].size();
    constexpr std::size_t block = 16;
    for (std::size_t first = 0; first < count; first += block) {
        const auto last = std::min(count, first + block);
        bool closer = false;
        for (std::size_t i = first; i < last; ++i) {
            double sum = 0.0;
            for (std::size_t a = 0; a < axes; ++a) {
                const auto delta =
                    std::max(std::max(rivals.min[a][i] - values[a], values[a] - rivals.max[a][i]), 0.0);
                sum += delta * delta;
            }
            closer |= sum < bound;
        }
        if (closer)
            return true;
    }
    return false;
}

// The union of points mask[i] over rivals strictly closer than swamp over temperature, continents and erosion.
inline std::uint32_t dominated_points(const Rivals& rivals, const std::vector<std::uint32_t>& mask, std::int64_t swamp,
                                      std::int32_t temperature, std::int32_t continents, std::int32_t erosion) {
    if (swamp == 0)
        return 0;
    const std::array<double, 3> values{double(temperature), double(continents), double(erosion)};
    const double bound = static_cast<double>(swamp);
    std::uint32_t points = 0;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        double sum = 0.0;
        for (std::size_t a = 0; a < values.size(); ++a) {
            const auto delta = std::max(std::max(rivals.min[a][i] - values[a], values[a] - rivals.max[a][i]), 0.0);
            sum += delta * delta;
        }
        points |= sum < bound ? mask[i] : 0U;
    }
    return points;
}

}  // namespace detail

class Filter {
public:
    explicit Filter(std::uint64_t seed) : climate_(seed), certificate_(&detail::certificate()) {
        const auto& program = minecraft::worldgen::Program::get(minecraft::Dimension::overworld);
        for (const auto axis : {"temperature", "vegetation", "continents", "erosion", "ridges"})
            horizontal_ = horizontal_ && program.code[program.output(axis)].y_free;
    }

    // x/z are candidate chunk-anchor block coordinates; false is a proved non-swamp result.
    [[nodiscard]] bool possible(std::int32_t x, std::int32_t z) const {
        sampled_.known = 0;
        if (!horizontal_ || !certificate_->valid)
            return true;
        const auto wrap4 = [](std::int32_t q) {
            return std::bit_cast<std::int32_t>(std::uint32_t(q) * 4U);
        };
        const auto qx = minecraft::floor_div(x, 4), qz = minecraft::floor_div(z, 4);
        const minecraft::BlockPos sample{wrap4(qx), 0, wrap4(qz)};
        sampled_.position = sample;
        const auto sample_axis = [&](std::size_t axis) {
            const auto value = climate_.sample(axis, sample);
            // Sampled::axes holds continents, erosion and ridges, consecutive climate axes.
            if (axis >= Climate::continents) {
                sampled_.values[axis - Climate::continents] = value;
                sampled_.known |= 1U << (axis - Climate::continents);
            }
            return minecraft::worldgen::quantize_climate(static_cast<float>(value));
        };

        const auto temperature_value = sample_axis(Climate::temperature);
        if (!detail::climate_in_safe_range(temperature_value))
            return true;
        const auto temperature = static_cast<std::int32_t>(temperature_value);
        if (temperature > 2000)
            return false;  // The startup certificate proves a strictly closer rival covers every other axis.
        // Below the swamp box on any of these axes a rival is strictly closer (Certificate::below_box), so each is
        // checked as soon as it is sampled, erosion before the costlier continents.
        const auto& box = certificate_->swamp[0].climate;
        const bool below_box = certificate_->below_box;
        if (below_box && temperature < box.temperature_min)
            return false;
        const auto erosion_value = sample_axis(Climate::erosion);
        if (!detail::climate_in_safe_range(erosion_value))
            return true;
        const auto erosion = static_cast<std::int32_t>(erosion_value);
        if (below_box && erosion < box.erosion_min)
            return false;
        const auto continents_value = sample_axis(Climate::continents);
        if (!detail::climate_in_safe_range(continents_value))
            return true;
        const auto continents = static_cast<std::int32_t>(continents_value);
        if (below_box && continents < box.continents_min)
            return false;

        // Ridge-free distances agree for every swamp point; see Certificate::before_union.
        const auto dominated_without_ridges = detail::dominated_points(
            certificate_->before_union, certificate_->before_points,
            detail::distance(certificate_->swamp[0].climate, temperature, continents, erosion, 0, false), temperature,
            continents, erosion);
        std::array<bool, 14> possible_without_ridges{};
        bool any = false;
        for (std::size_t i = 0; i < certificate_->swamp.size(); i += 2) {
            // The paired depth point was certified to have identical horizontal projections.
            possible_without_ridges[i] = !(dominated_without_ridges >> i & 1U);
            any = any || possible_without_ridges[i];
        }
        if (!any)
            return false;

        const auto ridges_value = sample_axis(Climate::ridges);
        if (!detail::climate_in_safe_range(ridges_value))
            return true;
        const auto ridges = static_cast<std::int32_t>(ridges_value);
        // Survivors of the humidity-free proof face the rivals covering their actual humidity.
        const auto humid = [&] {
            if (!certificate_->humidity_valid)
                return true;
            const auto humidity_value = sample_axis(Climate::vegetation);
            if (!detail::climate_in_safe_range(humidity_value))
                return true;
            const auto& bounds = certificate_->humidity_bounds;
            const auto humidity = std::clamp(static_cast<std::int32_t>(humidity_value), bounds.front(), bounds.back());
            const auto band = std::min<std::size_t>(
                std::upper_bound(bounds.begin(), bounds.end(), humidity) - bounds.begin() - 1, bounds.size() - 2);
            auto best_swamp = std::numeric_limits<std::int64_t>::max();
            // Each depth pair shares its climate box, as the certificate checked.
            for (std::size_t i = 0; i < certificate_->swamp.size(); i += 2)
                best_swamp = std::min(best_swamp, detail::distance(certificate_->swamp[i].climate, temperature,
                                                                   continents, erosion, ridges, true));
            // Inside a swamp box nothing is strictly closer.
            return best_swamp == 0 || !detail::dominated(certificate_->humidity_rivals[band], best_swamp, temperature,
                                                         continents, erosion, ridges, true);
        };
        if (certificate_->common_after_ridges) {
            auto best_swamp = std::numeric_limits<std::int64_t>::max();
            for (std::size_t i = 0; i < certificate_->swamp.size(); ++i) {
                if ((i & 1U) != 0 || !possible_without_ridges[i])
                    continue;
                const auto& target = certificate_->swamp[i];
                best_swamp = std::min(best_swamp,
                                      detail::distance(target.climate, temperature, continents, erosion, ridges, true));
            }
            return !detail::dominated(certificate_->after_rivals[0], best_swamp, temperature, continents, erosion,
                                      ridges, true) &&
                   humid();
        }
        for (std::size_t i = 0; i < certificate_->swamp.size(); ++i) {
            if ((i & 1U) != 0 || !possible_without_ridges[i])
                continue;
            const auto& target = certificate_->swamp[i];
            const auto swamp = detail::distance(target.climate, temperature, continents, erosion, ridges, true);
            if (!detail::dominated(certificate_->after_rivals[i], swamp, temperature, continents, erosion, ridges, true))
                return humid();
        }
        return false;
    }


    // The router outputs the last possible() call sampled, with the values a density scope computes for them at
    // position; known has bit i set when axes[i] was sampled.
    struct Sampled {
        static constexpr std::array<std::string_view, 3> axes{"continents", "erosion", "ridges"};
        minecraft::BlockPos position;
        std::array<double, axes.size()> values;
        unsigned known;
    };
    [[nodiscard]] const Sampled& sampled() const {
        return sampled_;
    }

private:
    Climate climate_;
    const detail::Certificate* certificate_;
    mutable Sampled sampled_{};
    bool horizontal_ = true;
};

}  // namespace witch_hut
