#include <minecraft/biome_sampler.hpp>
#include <minecraft/biome_zoom.hpp>

#include "../worldgen/density.hpp"
#include "../worldgen/rules.hpp"

#include <algorithm>
#include <bitset>
#include <span>
#include <stdexcept>
#include <utility>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace minecraft::biome {
namespace {
struct Point {
    std::string_view biome;
    std::array<std::int64_t, 6> min, max;
    std::int64_t offset;
    std::uint16_t biome_index;
};
struct Node {
    std::array<std::int64_t, 6> min, max;
    std::int64_t offset;  // smallest |offset| inside, so the box distance bounds every point's distance
    std::uint32_t begin, end;  // child nodes, or points when leaf
    bool leaf;
};

// The nearest search below is exhaustive up to ties, so its answer does not depend on how the tree groups points.
// The grouping mirrors vanilla Climate.RTree only because it prunes well.
struct Parameters {
    static constexpr std::size_t fanout = 6;
    static constexpr std::size_t max_biomes = 256;
    std::vector<Point> points;  // tree order: each leaf owns a contiguous range
    std::vector<Node> nodes;
    std::vector<std::string_view> biomes;

    explicit Parameters(std::string_view key) {
        std::vector<Point> input;
        for (const auto& item : std::get<minecraft::worldgen::JsonArray>(worldgen::field(worldgen::rules(), key))) {
            Point p{worldgen::string(worldgen::field(item, "biome_id")), {}, {}, 0, 0};
            const auto& lo = std::get<minecraft::worldgen::JsonArray>(worldgen::field(item, "minimum"));
            const auto& hi = std::get<minecraft::worldgen::JsonArray>(worldgen::field(item, "maximum"));
            for (std::size_t i = 0; i < 6; ++i) {
                p.min[i] = static_cast<std::int64_t>(worldgen::number(lo[i]));
                p.max[i] = static_cast<std::int64_t>(worldgen::number(hi[i]));
            }
            p.offset = static_cast<std::int64_t>(worldgen::number(worldgen::field(item, "offset")));
            input.push_back(p);
            biomes.push_back(p.biome);
        }
        std::sort(biomes.begin(), biomes.end());
        biomes.erase(std::unique(biomes.begin(), biomes.end()), biomes.end());
        if (biomes.size() > max_biomes)
            throw std::runtime_error("Too many biomes in " + std::string(key));
        for (auto& p : input)
            p.biome_index = static_cast<std::uint16_t>(
                std::lower_bound(biomes.begin(), biomes.end(), p.biome) - biomes.begin());
        std::vector<std::uint32_t> order(input.size());
        std::iota(order.begin(), order.end(), 0U);
        points.reserve(input.size());
        nodes.emplace_back();
        std::vector<std::int64_t> keys(input.size());
        build(0, input, order, keys);
    }

    // Axis 6 is the offset.
    static std::int64_t lower(const Point& p, std::size_t axis) {
        return axis < 6 ? p.min[axis] : p.offset;
    }
    static std::int64_t upper(const Point& p, std::size_t axis) {
        return axis < 6 ? p.max[axis] : p.offset;
    }
    static std::int64_t extent(const std::vector<Point>& input, std::span<const std::uint32_t> items) {
        std::int64_t cost = 0;
        for (std::size_t axis = 0; axis < 7; ++axis) {
            auto lo = std::numeric_limits<std::int64_t>::max(), hi = std::numeric_limits<std::int64_t>::min();
            for (const auto i : items) {
                lo = std::min(lo, lower(input[i], axis));
                hi = std::max(hi, upper(input[i], axis));
            }
            cost += hi - lo;
        }
        return cost;
    }

    void build(const std::size_t index, const std::vector<Point>& input, std::span<std::uint32_t> items,
               std::vector<std::int64_t>& keys) {
        Node node{};
        node.min.fill(std::numeric_limits<std::int64_t>::max());
        node.max.fill(std::numeric_limits<std::int64_t>::min());
        node.offset = std::numeric_limits<std::int64_t>::max();
        for (const auto i : items) {
            const auto& p = input[i];
            for (std::size_t j = 0; j < 6; ++j) {
                node.min[j] = std::min(node.min[j], p.min[j]);
                node.max[j] = std::max(node.max[j], p.max[j]);
            }
            node.offset = std::min(node.offset, p.offset < 0 ? -p.offset : p.offset);
        }
        if (items.size() <= fanout) {
            node.leaf = true;
            node.begin = static_cast<std::uint32_t>(points.size());
            for (const auto i : items)
                points.push_back(input[i]);
            node.end = static_cast<std::uint32_t>(points.size());
            nodes[index] = node;
            return;
        }
        // Vanilla buckets into the largest power of six below the size, splitting on the cheapest axis.
        std::size_t bucket = 1;
        while (bucket * fanout < items.size())
            bucket *= fanout;
        const auto sort_axis = [&](std::size_t axis) {
            for (const auto i : items)
                keys[i] = lower(input[i], axis) + upper(input[i], axis);
            std::sort(items.begin(), items.end(), [&](std::uint32_t x, std::uint32_t y) {
                return keys[x] != keys[y] ? keys[x] < keys[y] : x < y;
            });
        };
        auto best_cost = std::numeric_limits<std::int64_t>::max();
        std::size_t best_axis = 0;
        for (std::size_t axis = 0; axis < 7; ++axis) {
            sort_axis(axis);
            std::int64_t cost = 0;
            for (std::size_t begin = 0; begin < items.size(); begin += bucket)
                cost += extent(input, items.subspan(begin, std::min(bucket, items.size() - begin)));
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
            }
        }
        sort_axis(best_axis);
        const auto count = (items.size() + bucket - 1) / bucket;
        node.leaf = false;
        node.begin = static_cast<std::uint32_t>(nodes.size());
        node.end = static_cast<std::uint32_t>(nodes.size() + count);
        nodes[index] = node;
        nodes.resize(nodes.size() + count);
        for (std::size_t child = 0; child < count; ++child) {
            const auto begin = child * bucket;
            build(node.begin + child, input, items.subspan(begin, std::min(bucket, items.size() - begin)), keys);
        }
    }

    template <class P> static std::int64_t distance(const P& p, const ClimateTarget& target) {
        auto d = p.offset * p.offset;
        for (std::size_t i = 0; i < 6; ++i) {
            const auto v = target.values[i], delta = v < p.min[i] ? p.min[i] - v : v > p.max[i] ? v - p.max[i] : 0;
            d += delta * delta;
        }
        return d;
    }

    // Every biome owning a point at the best distance; ties and membership are settled once the search ends.
    struct Search {
        const ClimateTarget& target;
        std::int64_t best = std::numeric_limits<std::int64_t>::max();
        std::bitset<max_biomes> at_best;
        std::uint32_t best_leaf = 0;
    };
    void visit_leaf(const std::uint32_t index, Search& search) const {
        const auto& node = nodes[index];
        for (auto i = node.begin; i < node.end; ++i) {
            const auto& p = points[i];
            const auto d = distance(p, search.target);
            if (d < search.best) {
                search.best = d;
                search.at_best.reset();
                search.at_best.set(p.biome_index);
                search.best_leaf = index;
            } else if (d == search.best) {
                search.at_best.set(p.biome_index);
            }
        }
    }
    void visit(const std::uint32_t index, Search& search) const {
        const auto& node = nodes[index];
        if (node.leaf) {
            visit_leaf(index, search);
            return;
        }
        std::array<std::pair<std::int64_t, std::uint32_t>, fanout> children;
        std::size_t count = 0;
        for (auto i = node.begin; i < node.end; ++i) {
            const std::pair child{distance(nodes[i], search.target), i};
            auto at = count++;
            for (; at > 0 && children[at - 1].first > child.first; --at)
                children[at] = children[at - 1];
            children[at] = child;
        }
        // A point at exactly the best distance may still tie, so only strictly farther boxes are skipped.
        for (std::size_t i = 0; i < count && children[i].first <= search.best; ++i)
            visit(children[i].second, search);
    }

    struct Match {
        std::optional<std::string_view> biome;
        bool tied = false, member = false, membership_tied = false;
    };
    Match nearest(const ClimateTarget& target, std::span<const std::string_view> allowed = {}) const {
        // Consecutive queries are usually neighbouring samples, so the leaf that won the previous search gives a
        // tight starting bound. The traversal still visits every box within the final best distance, and seeing the
        // hinted leaf twice only re-marks biomes already marked, so the answer is unchanged.
        thread_local struct {
            const Parameters* owner = nullptr;
            std::uint32_t leaf = 0;
        } hint;
        Search search{target, std::numeric_limits<std::int64_t>::max(), {}, 0};
        if (hint.owner == this)
            visit_leaf(hint.leaf, search);
        visit(0, search);
        hint = {this, search.best_leaf};
        Match result;
        bool first = true;
        for (std::size_t i = 0; i < biomes.size(); ++i) {
            if (!search.at_best.test(i))
                continue;
            const bool member = std::find(allowed.begin(), allowed.end(), biomes[i]) != allowed.end();
            if (first) {
                result = {biomes[i], false, member, false};
                first = false;
            } else {
                result.tied = true;
                result.membership_tied = result.membership_tied || member != result.member;
            }
        }
        return result;
    }
    std::optional<std::string_view> match(const ClimateTarget& target) const {
        const auto result = nearest(target);
        return result.tied ? std::nullopt : result.biome;
    }
    std::optional<bool> matches(const ClimateTarget& target, std::span<const std::string_view> allowed) const {
        const auto result = nearest(target, allowed);
        return result.membership_tied ? std::nullopt : std::optional(result.member);
    }
};
const Parameters& parameters() {
    static const Parameters p("overworld_parameters");
    return p;
}
}  // namespace

struct Sampler::Impl {
    std::uint64_t zoom_seed;
    worldgen::DensitySampler density;
    Impl(std::uint64_t seed, Dimension d)
        : zoom_seed(obfuscate_seed(seed)), density(seed, d, true) {}
};
Sampler::Sampler(std::uint64_t seed, Dimension dimension) : impl_(std::make_unique<Impl>(seed, dimension)) {}
Sampler::~Sampler() = default;
Sampler::Sampler(Sampler&&) noexcept = default;
Sampler& Sampler::operator=(Sampler&&) noexcept = default;
Sample Sampler::noise(QuartPos p) const {
    const auto climate = impl_->density.climate(p);
    return {climate, parameters().match(climate)};
}
QuartPos Sampler::block_quart(BlockPos p) const {
    return zoom_quart(impl_->zoom_seed, p);
}
QuartPos Sampler::stored_block_quart(BlockPos p) const {
    // The Overworld stores 384 blocks, from Y=-64 through Y=319.
    auto quart = block_quart(p);
    quart.y = std::clamp(quart.y, -16, 79);
    return quart;
}
std::optional<bool> Sampler::matches(QuartPos p, std::span<const std::string_view> allowed) const {
    return parameters().matches(impl_->density.climate(p), allowed);
}
std::span<const std::string_view> catalog() {
    return parameters().biomes;
}
}  // namespace minecraft::biome
