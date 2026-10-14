#include "density.hpp"
#include "noise.hpp"
#include "program.hpp"

#include <minecraft/random.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace minecraft::worldgen {
namespace {

enum class Mode { direct, corner, interpolated, filling_cell };

// Evaluations are pure, so a value keyed by everything the instruction can observe may be reused exactly. Every
// context inside one scope shares a cell origin (first_x, first_z), so keys leave it out.
//
// Each memoized instruction owns a line of eight entries chosen by cell parity: the x, z and y cell bits. A column
// revisits the same four corners on two layers, which therefore never evict one another. Any other collision only
// costs a recomputation.
class MemoCache {
public:
    void prepare(const std::uint32_t lines, const int xz_shift, const int y_shift) {
        xz_shift_ = xz_shift;
        y_shift_ = y_shift;
        if (entries_.size() < static_cast<std::size_t>(lines) * ways)
            entries_.assign(static_cast<std::size_t>(lines) * ways, Entry{});
        clear();
    }
    // Forgets every entry without touching them.
    void clear() {
        generation_ += 1U << mode_bits;
        if (generation_ == 0) {
            std::fill(entries_.begin(), entries_.end(), Entry{});
            generation_ = 1U << mode_bits;
        }
    }

    struct Entry {
        std::int32_t x = 0, y = 0, z = 0;
        std::uint32_t stamp = 0;  // generation and mode; zero is never live
        double value = 0.0;
    };
    [[nodiscard]] Entry& line(const std::uint32_t slot, const BlockPos pos) {
        const auto way = (static_cast<std::uint32_t>(pos.x >> xz_shift_) & 1U) |
                         (static_cast<std::uint32_t>(pos.z >> xz_shift_) & 1U) << 1U |
                         (static_cast<std::uint32_t>(pos.y >> y_shift_) & 1U) << 2U;
        return entries_[static_cast<std::size_t>(slot) * ways + way];
    }
    [[nodiscard]] std::uint32_t stamp(const std::uint32_t mode) const {
        return generation_ | mode;
    }

private:
    static constexpr std::uint32_t ways = 8, mode_bits = 2;
    std::vector<Entry> entries_;
    std::uint32_t generation_ = 0;
    int xz_shift_ = 0, y_shift_ = 0;
};

using CellValues = std::array<double, 8>;
struct CellCache {
    bool valid = false;
    std::uint8_t filled = 0;  // corners holding values, by CellValues index; see interpolate
    int y = 0;
    CellValues values{};
};

// The cell corner values of every interpolated instruction over one 16x16 block area of a sampler instance. A corner
// value is a Mode::corner evaluation at a cell corner: inside it every position stays on that corner, which is a
// multiple of four and so always hits the flat cache, so the value observes neither the column nor the cell asking for
// it. The columns of an area therefore share them exactly, as NoiseChunk shares them across a chunk.
class AreaCorners {
public:
    static constexpr int size = 16;

    void reset(const std::uint64_t instance, const int x, const int z, const Program& program) {
        instance_ = instance;
        x_ = x;
        z_ = z;
        width_shift_ = std::countr_zero(static_cast<unsigned>(program.cell_width));
        height_shift_ = std::countr_zero(static_cast<unsigned>(program.cell_height));
        min_y_ = program.min_y;
        across_ = (size >> width_shift_) + 1;
        layers_ = (program.height >> height_shift_) + 1;
        const auto count = static_cast<std::size_t>(program.cell_count) * across_ * across_ * layers_;
        if (stamps_.size() < count) {
            values_.assign(count, 0.0);
            stamps_.assign(count, 0);
            generation_ = 0;
        }
        if (++generation_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0);
            generation_ = 1;
        }
    }
    [[nodiscard]] bool holds(const std::uint64_t instance, const int x, const int z) const {
        return instance_ == instance && x_ == x && z_ == z;
    }
    // The entry of interpolated instruction slot at corner p, or -1 when p lies outside the area.
    [[nodiscard]] std::ptrdiff_t index(const std::uint32_t slot, const BlockPos p) const {
        const int dx = p.x - x_, dz = p.z - z_, dy = p.y - min_y_;
        if (dx < 0 || dx > size || dz < 0 || dz > size || dy < 0)
            return -1;
        const int layer = dy >> height_shift_;
        if (layer >= layers_)
            return -1;
        return ((static_cast<std::ptrdiff_t>(slot) * across_ + (dx >> width_shift_)) * across_ + (dz >> width_shift_)) *
                   layers_ +
               layer;
    }
    [[nodiscard]] const double* find(const std::ptrdiff_t i) const {
        return stamps_[static_cast<std::size_t>(i)] == generation_ ? &values_[static_cast<std::size_t>(i)] : nullptr;
    }
    void store(const std::ptrdiff_t i, const double value) {
        values_[static_cast<std::size_t>(i)] = value;
        stamps_[static_cast<std::size_t>(i)] = generation_;
    }

    // The area holding column (x, z) for this thread. A few recent areas are kept; an area still held by a scope is
    // never reset under it.
    static std::shared_ptr<AreaCorners> acquire(const std::uint64_t instance, const Program& program, const int x,
                                                const int z) {
        struct Slot {
            std::shared_ptr<AreaCorners> area;
            std::uint64_t used = 0;
        };
        thread_local std::array<Slot, 4> slots;
        thread_local std::uint64_t clock = 0;
        const int ax = x & ~(size - 1), az = z & ~(size - 1);
        ++clock;
        Slot* victim = &slots[0];
        for (auto& slot : slots) {
            if (slot.area && slot.area->holds(instance, ax, az)) {
                slot.used = clock;
                return slot.area;
            }
            if (slot.used < victim->used)
                victim = &slot;
        }
        if (!victim->area || victim->area.use_count() > 1)
            victim->area = std::make_shared<AreaCorners>();
        victim->area->reset(instance, ax, az, program);
        victim->used = clock;
        return victim->area;
    }

private:
    std::vector<double> values_;
    std::vector<std::uint32_t> stamps_;
    std::uint32_t generation_ = 0;
    std::uint64_t instance_ = 0;
    int x_ = 0, z_ = 0, min_y_ = 0, width_shift_ = 0, height_shift_ = 0, across_ = 0, layers_ = 0;
};

struct Scope {
    MemoCache memo;
    std::vector<CellCache> cells;          // one per interpolated instruction; column scopes only
    std::shared_ptr<AreaCorners> corners;  // column scopes only, when corner sharing is exact
    // The width of the NoiseChunk the scope stands for, which bounds its flat caches: one cell, or a whole chunk.
    int flat_span = 0;

    void prepare(const Program& program, const bool column) {
        memo.prepare(program.memo_count, std::countr_zero(static_cast<unsigned>(program.cell_width)),
                     std::countr_zero(static_cast<unsigned>(program.cell_height)));
        cells.assign(column ? program.cell_count : 0, CellCache{});
        corners.reset();
        flat_span = program.cell_width;
    }
};

// Scopes are recycled per thread: preparing a recycled one is a generation bump, not an allocation.
class ScopePool {
public:
    static std::unique_ptr<Scope> acquire(const Program& program, const bool column) {
        auto& pool = free_list();
        std::unique_ptr<Scope> scope;
        if (pool.empty()) {
            scope = std::make_unique<Scope>();
        } else {
            scope = std::move(pool.back());
            pool.pop_back();
        }
        scope->prepare(program, column);
        return scope;
    }
    static void release(std::unique_ptr<Scope> scope) {
        auto& pool = free_list();
        if (scope)
            scope->corners.reset();
        if (scope && pool.size() < 8)
            pool.push_back(std::move(scope));
    }

private:
    static std::vector<std::unique_ptr<Scope>>& free_list() {
        thread_local std::vector<std::unique_ptr<Scope>> pool;
        return pool;
    }
};

// A pooled scope held for one evaluation.
class ScopeLease {
public:
    ScopeLease(const Program& program, const bool column) : scope_(ScopePool::acquire(program, column)) {}
    ~ScopeLease() {
        ScopePool::release(std::move(scope_));
    }
    ScopeLease(ScopeLease&&) noexcept = default;
    ScopeLease& operator=(ScopeLease&& other) noexcept {
        ScopePool::release(std::move(scope_));
        scope_ = std::move(other.scope_);
        return *this;
    }
    [[nodiscard]] Scope* get() const {
        return scope_.get();
    }

private:
    std::unique_ptr<Scope> scope_;
};

struct Context {
    BlockPos pos;
    Mode mode;
    int first_x, first_z;
    Scope* scope = nullptr;
};

double lerp(double t, double a, double b) {
    return a + t * (b - a);
}
float lerp(float t, float a, float b) {
    return a + t * (b - a);
}

// The seed-bound half of a sampler: noises (built on first use) and positional forks. Samplers of the same seed and
// dimension share one instance.
class Instance {
public:
    Instance(const std::uint64_t seed, const Dimension dimension)
        : program_(Program::get(dimension)), code_(program_.code.data()), id_(next_id()), seed_(seed),
          random_(random::RandomSource(seed, program_.legacy_random ? random::RandomAlgorithm::legacy
                                                                    : random::RandomAlgorithm::xoroshiro)
                      .fork_positional()),
          aquifer_(random_.from_hash_of("minecraft:aquifer").fork_positional()),
          ore_(random_.from_hash_of("minecraft:ore").fork_positional()),
          noises_(std::make_unique<NoiseSlot[]>(program_.noises.size())) {
        // Built up front, so evaluation reads them without a once-guard per noise operation.
        for (std::uint32_t i = 0; i < program_.noises.size(); ++i)
            create_noise(i);
    }

    static std::shared_ptr<const Instance> get(const std::uint64_t seed, const Dimension dimension) {
        static std::mutex mutex;
        static std::map<std::pair<std::uint64_t, Dimension>, std::weak_ptr<const Instance>> live;
        const std::lock_guard lock(mutex);
        auto& slot = live[{seed, dimension}];
        if (auto existing = slot.lock())
            return existing;
        auto created = std::make_shared<const Instance>(seed, dimension);
        slot = created;
        if (live.size() > 256)
            std::erase_if(live, [](const auto& entry) { return entry.second.expired(); });
        return created;
    }

    [[nodiscard]] const Program& program() const {
        return program_;
    }
    // Unique for the process lifetime, unlike the address.
    [[nodiscard]] std::uint64_t id() const {
        return id_;
    }
    [[nodiscard]] const random::PositionalFactory& aquifer() const {
        return aquifer_;
    }
    [[nodiscard]] const random::PositionalFactory& ore() const {
        return ore_;
    }

    double evaluate(const std::uint32_t index, const Context& c) const {
        const auto& n = code_[index];
        if (n.memo && c.scope)
            return evaluate_memo(n, c);
        return evaluate_node(n, c);
    }

    double sample(const std::uint32_t output, const BlockPos pos, const Mode mode, Scope* scope) const {
        const auto width = program_.cell_width;
        return evaluate(output, {pos, mode, floor_div(pos.x, width) * width, floor_div(pos.z, width) * width, scope});
    }

private:
    struct NoiseSlot {
        std::unique_ptr<NormalNoise> normal;
        std::unique_ptr<BlendedNoise> blended;
    };

    const Program& program_;
    const Instruction* code_;
    std::uint64_t id_;
    std::uint64_t seed_;
    random::PositionalFactory random_, aquifer_, ore_;
    std::unique_ptr<NoiseSlot[]> noises_;

    static std::uint64_t next_id() {
        static std::atomic<std::uint64_t> counter{0};
        return ++counter;
    }

    // Each noise draws from its own fork, so creating them in any order yields the same noises.
    void create_noise(const std::uint32_t index) {
        auto& slot = noises_[index];
        {
            const auto& spec = program_.noises[index];
            switch (spec.kind) {
                case NoiseSpec::Kind::normal:
                    slot.normal = std::make_unique<NormalNoise>(random_.from_hash_of(spec.id), spec.parameters);
                    break;
                case NoiseSpec::Kind::blended: {
                    auto rng = program_.legacy_random ? random::RandomSource(seed_)
                                                      : random_.from_hash_of("minecraft:terrain");
                    const auto& b = spec.blended;
                    slot.blended = std::make_unique<BlendedNoise>(rng, b[0], b[1], b[2], b[3], b[4]);
                    break;
                }
            }
        }
    }
    const NoiseSlot& noise_slot(const std::uint32_t index) const {
        return noises_[index];
    }
    const NormalNoise& normal(const std::uint32_t index) const {
        return *noise_slot(index).normal;
    }

    double evaluate_memo(const Instruction& n, const Context& c) const {
        const BlockPos key{c.pos.x, n.y_free ? 0 : c.pos.y, c.pos.z};
        auto& entry = c.scope->memo.line(n.memo_slot, key);
        const auto stamp = c.scope->memo.stamp(static_cast<std::uint32_t>(c.mode));
        if (entry.stamp == stamp && entry.x == key.x && entry.y == key.y && entry.z == key.z)
            return entry.value;
        // Nested evaluations never touch this instruction's line, so the reference stays valid.
        const auto value = evaluate_node(n, c);
        entry = {key.x, key.y, key.z, stamp, value};
        return value;
    }

    struct Interval {
        double lo, hi;
    };

    // An interval holding evaluate(index, c). Blended noise stands in as its magnitude bound instead of being
    // sampled; subtrees without it are evaluated exactly. The bounded operations round monotonically in each
    // argument, so applying one to its arguments' endpoints bounds it applied to any values between them.
    Interval bounds(const std::uint32_t index, const Context& c) const {
        const auto& n = code_[index];
        const auto exact = [&] {
            const auto v = evaluate(index, c);
            return Interval{v, v};
        };
        if (!n.bounded)
            return exact();
        if (n.memo && c.scope) {
            const BlockPos key{c.pos.x, n.y_free ? 0 : c.pos.y, c.pos.z};
            const auto& entry = c.scope->memo.line(n.memo_slot, key);
            if (entry.stamp == c.scope->memo.stamp(static_cast<std::uint32_t>(c.mode)) && entry.x == key.x &&
                entry.y == key.y && entry.z == key.z)
                return {entry.value, entry.value};
        }
        const auto& p = n.p;
        switch (n.op) {
            case Op::blended_noise: {
                const auto m = noise_slot(n.noise).blended->max_magnitude();
                return {-m, m};
            }
            case Op::add: {
                const auto x = bounds(n.args[0], c), y = bounds(n.args[1], c);
                return {x.lo + y.lo, x.hi + y.hi};
            }
            case Op::mul: {
                const auto x = bounds(n.args[0], c);
                if (x.lo == 0.0 && x.hi == 0.0)
                    return {0.0, 0.0};  // evaluate_node skips the second factor of a zero.
                const auto y = bounds(n.args[1], c);
                const std::array<double, 4> corners{x.lo * y.lo, x.lo * y.hi, x.hi * y.lo, x.hi * y.hi};
                return {*std::min_element(corners.begin(), corners.end()),
                        *std::max_element(corners.begin(), corners.end())};
            }
            case Op::minimum: {
                const auto x = bounds(n.args[0], c), y = bounds(n.args[1], c);
                return {std::min(x.lo, y.lo), std::min(x.hi, y.hi)};
            }
            case Op::maximum: {
                const auto x = bounds(n.args[0], c), y = bounds(n.args[1], c);
                return {std::max(x.lo, y.lo), std::max(x.hi, y.hi)};
            }
            case Op::clamp:
            case Op::half_negative:
            case Op::quarter_negative: {
                const auto x = bounds(n.args[0], c);
                return {apply_unary(n.op, x.lo, p), apply_unary(n.op, x.hi, p)};
            }
            default:
                return exact();
        }
    }

    double evaluate_node(const Instruction& n, const Context& c) const {
        const auto a = [&](std::size_t i = 0) {
            const auto& arg = code_[n.args[i]];
            return arg.op == Op::constant ? arg.p[0] : evaluate(n.args[i], c);
        };
        const auto& p = n.p;
        switch (n.op) {
            case Op::constant:
                return p[0];
            case Op::add:
                return a() + a(1);
            case Op::mul: {
                const auto d = a();
                return d == 0.0 ? 0.0 : d * a(1);
            }
            case Op::minimum:
                return std::min(a(), a(1));
            case Op::maximum:
                return std::max(a(), a(1));
            case Op::clamp:
                // A bound wholly past one limit settles the result without the exact input. Strict comparisons
                // keep a limit-equal input, whose zero sign could differ, on the exact path.
                if (code_[n.args[0]].bounded) {
                    const auto b = bounds(n.args[0], c);
                    if (b.hi < p[0])
                        return p[0];
                    if (b.lo > p[1])
                        return p[1];
                }
                return apply_unary(n.op, a(), p);
            case Op::abs:
            case Op::square:
            case Op::cube:
            case Op::half_negative:
            case Op::quarter_negative:
            case Op::invert:
            case Op::squeeze:
                return apply_unary(n.op, a(), p);
            case Op::gradient:
                return lerp(std::clamp((c.pos.y - p[0]) / (p[1] - p[0]), 0.0, 1.0), p[2], p[3]);
            case Op::noise:
                return normal(n.noise).sample(c.pos.x * p[0], c.pos.y * p[1], c.pos.z * p[0]);
            case Op::blended_noise:
                return noise_slot(n.noise).blended->sample(c.pos);
            case Op::shifted_noise:
                return normal(n.noise).sample(c.pos.x * p[0] + a(), c.pos.y * p[1] + a(1), c.pos.z * p[0] + a(2));
            case Op::shift:
                return normal(n.noise).sample(c.pos.x * .25, c.pos.y * .25, c.pos.z * .25) * 4;
            case Op::shift_a:
                return normal(n.noise).sample(c.pos.x * .25, 0, c.pos.z * .25) * 4;
            case Op::shift_b:
                return normal(n.noise).sample(c.pos.z * .25, c.pos.x * .25, 0) * 4;
            case Op::weird: {
                const auto d = a();
                const double rarity = p[0] == 1 ? (d < -.5  ? .75
                                                   : d < 0  ? 1
                                                   : d < .5 ? 1.5
                                                            : 2)
                                                : (d < -.75  ? .5
                                                   : d < -.5 ? .75
                                                   : d < .5  ? 1
                                                   : d < .75 ? 2
                                                             : 3);
                return rarity * std::abs(normal(n.noise).sample(c.pos.x / rarity, c.pos.y / rarity, c.pos.z / rarity));
            }
            case Op::spline:
                return spline(n, c);
            case Op::range_choice: {
                // Deep underground the sloped terrain lies far above the cave threshold whatever the blended noise
                // adds, so the branch is known before that noise is sampled.
                if (code_[n.args[0]].bounded) {
                    const auto b = bounds(n.args[0], c);
                    if (b.lo >= p[0] && b.hi < p[1])
                        return a(1);
                    if (b.hi < p[0] || b.lo >= p[1])
                        return a(2);
                }
                const auto d = a();
                return a(d >= p[0] && d < p[1] ? 1 : 2);
            }
            case Op::flat_cache:
                if (c.mode != Mode::direct) {
                    // NoiseChunk.FlatCache holds the quarts of its chunk, edges included.
                    const auto span = c.scope ? c.scope->flat_span : program_.cell_width;
                    const auto origin_x = c.first_x & -span, origin_z = c.first_z & -span;
                    const auto x = floor_div(c.pos.x, 4) * 4;
                    if (x >= origin_x && x <= origin_x + span) {
                        const auto z = floor_div(c.pos.z, 4) * 4;
                        if (z >= origin_z && z <= origin_z + span) {
                            auto corner = c;
                            corner.pos = {x, 0, z};
                            corner.mode = Mode::corner;
                            return evaluate(n.args[0], corner);
                        }
                    }
                }
                return a();
            case Op::interpolated:
                if (c.mode == Mode::direct || c.mode == Mode::corner)
                    return a();
                return interpolate(n, c);
            case Op::find_top_surface: {
                const int top = static_cast<int>(std::floor(a(1) / p[1])) * static_cast<int>(p[1]);
                auto corner = c;
                corner.mode = c.mode == Mode::direct ? Mode::direct : Mode::corner;
                for (int y = top; y >= p[0]; y -= static_cast<int>(p[1])) {
                    corner.pos.y = y;
                    if (evaluate(n.args[0], corner) > 0)
                        return y;
                }
                return p[0];
            }
        }
        throw std::runtime_error("Invalid density instruction");
    }

    double spline(const Instruction& n, const Context& c) const {
        const auto* begin = program_.points.data() + n.points_begin;
        const auto* end = program_.points.data() + n.points_end;
        const float coordinate = static_cast<float>(evaluate(n.args[0], c));
        const auto* upper =
            std::upper_bound(begin, end, coordinate, [](float f, const SplinePoint& pt) { return f < pt.location; });
        if (upper == begin || upper == end) {
            const auto& pt = upper == begin ? *begin : *(end - 1);
            const float value = static_cast<float>(evaluate(pt.value, c));
            return pt.derivative == 0 ? value : value + pt.derivative * (coordinate - pt.location);
        }
        const auto& l = *(upper - 1);
        const auto& r = *upper;
        const float t = (coordinate - l.location) / (r.location - l.location);
        const float vl = static_cast<float>(evaluate(l.value, c));
        const float vr = static_cast<float>(evaluate(r.value, c));
        const float dl = l.derivative * (r.location - l.location) - (vr - vl);
        const float dr = -r.derivative * (r.location - l.location) + (vr - vl);
        return lerp(t, vl, vr) + t * (1.0F - t) * lerp(t, dl, dr);
    }

    double corner(const Instruction& n, const BlockPos p, const Context& c) const {
        const Context corner{p, Mode::corner, c.first_x, c.first_z, c.scope};
        auto* area = c.scope ? c.scope->corners.get() : nullptr;
        const auto i = area ? area->index(n.cell_slot, p) : -1;
        if (i < 0)
            return evaluate(n.args[0], corner);
        if (const auto* known = area->find(i))
            return *known;
        const auto value = evaluate(n.args[0], corner);
        area->store(i, value);
        return value;
    }

    double interpolate(const Instruction& n, const Context& c) const {
        const auto width = program_.cell_width, cell_height = program_.cell_height;
        const int y = floor_div(c.pos.y, cell_height) * cell_height;
        const double fx = static_cast<double>(c.pos.x - c.first_x) / width;
        const double fy = static_cast<double>(c.pos.y - y) / cell_height;
        const double fz = static_cast<double>(c.pos.z - c.first_z) / width;
        // Column scopes pin first_x/first_z, so the cell base y identifies the cell. A scan moving one cell down or
        // up shares a corner layer with the previous cell.
        //
        // Along an axis whose fraction is zero, lerp(0, lower, upper) is the lower value for any finite upper value
        // (up to the sign of a zero, which no density comparison sees). Corners off such axes are therefore left out
        // until a position that weights them asks: a cell-aligned column needs one corner column instead of four.
        // Corner index bits: x 1, y 2, z 4, matching CellValues (z * 4 + y * 2 + x).
        const unsigned axes = (fx != 0.0 ? 1U : 0U) | (fy != 0.0 ? 2U : 0U) | (fz != 0.0 ? 4U : 0U);
        // The corners whose index uses only weighted axes, per axes value.
        static constexpr std::array<std::uint8_t, 8> weighted{0x01, 0x03, 0x05, 0x0F, 0x11, 0x33, 0x55, 0xFF};
        const unsigned needed = weighted[axes];
        CellCache local;
        auto& cell = c.scope && !c.scope->cells.empty() ? c.scope->cells[n.cell_slot] : local;
        if (!cell.valid || cell.y != y) {
            std::uint8_t kept = 0;
            if (cell.valid && cell.y == y + cell_height) {
                // The old lower layer becomes the upper one.
                for (int z = 0; z < 2; ++z)
                    for (int x = 0; x < 2; ++x)
                        cell.values[z * 4 + 2 + x] = cell.values[z * 4 + x];
                kept = static_cast<std::uint8_t>((cell.filled & 0x33U) << 2U);
            } else if (cell.valid && cell.y == y - cell_height) {
                for (int z = 0; z < 2; ++z)
                    for (int x = 0; x < 2; ++x)
                        cell.values[z * 4 + x] = cell.values[z * 4 + 2 + x];
                kept = static_cast<std::uint8_t>((cell.filled & 0xCCU) >> 2U);
            }
            cell.valid = true;
            cell.filled = kept;
            cell.y = y;
        }
        if (const unsigned missing = needed & ~static_cast<unsigned>(cell.filled)) {
            for (unsigned i = 0; i < 8; ++i)
                if (missing >> i & 1U)
                    cell.values[i] = corner(n,
                                            {c.first_x + static_cast<int>(i & 1U) * width,
                                             y + static_cast<int>(i >> 1U & 1U) * cell_height,
                                             c.first_z + static_cast<int>(i >> 2U) * width},
                                            c);
            cell.filled = static_cast<std::uint8_t>(cell.filled | missing);
        }
        // A corner never computed carries no weight here; give it its weighted neighbour's value so that the
        // lerps below stay finite. It stays unfilled, so a later position that weights it computes it.
        if (cell.filled != 0xFFU)
            for (unsigned i = 0; i < 8; ++i)
                if (!(cell.filled >> i & 1U))
                    cell.values[i] = cell.values[i & axes];
        const auto& v = cell.values;
        if (c.mode == Mode::filling_cell)
            return lerp(fz, lerp(fy, lerp(fx, v[0], v[1]), lerp(fx, v[2], v[3])),
                        lerp(fy, lerp(fx, v[4], v[5]), lerp(fx, v[6], v[7])));
        return lerp(fz, lerp(fx, lerp(fy, v[0], v[2]), lerp(fy, v[1], v[3])),
                    lerp(fx, lerp(fy, v[4], v[6]), lerp(fy, v[5], v[7])));
    }
};

constexpr std::array<std::string_view, 6> climate_outputs{"temperature", "vegetation", "continents",
                                                          "erosion",     "depth",      "ridges"};

}  // namespace

struct DensitySampler::Impl {
    std::shared_ptr<const Instance> instance;
    const Program& program;
    bool climate_only;
    std::array<std::uint32_t, 6> climate_roots{};  // in ClimateTarget order

    Impl(std::uint64_t seed, Dimension dimension, bool climate_only)
        : instance(Instance::get(seed, dimension)), program(instance->program()), climate_only(climate_only) {
        for (std::size_t i = 0; i < climate_outputs.size(); ++i)
            climate_roots[i] = program.output(climate_outputs[i]);
    }

    std::uint32_t output(std::string_view name) const {
        if (climate_only && std::find(climate_outputs.begin(), climate_outputs.end(), name) == climate_outputs.end())
            throw std::out_of_range("Unknown density router output: " + std::string(name));
        return program.output(name);
    }
};

DensitySampler::DensitySampler(std::uint64_t seed, Dimension dimension, bool climate_only)
    : impl_(std::make_unique<Impl>(seed, dimension, climate_only)) {}
DensitySampler::~DensitySampler() = default;
DensitySampler::DensitySampler(DensitySampler&&) noexcept = default;
DensitySampler& DensitySampler::operator=(DensitySampler&&) noexcept = default;
double DensitySampler::direct(std::string_view output, BlockPos p) const {
    const ScopeLease scope(impl_->program, false);
    return impl_->instance->sample(impl_->output(output), p, Mode::direct, scope.get());
}
std::uint32_t DensitySampler::output_index(std::string_view output) const {
    return impl_->output(output);
}

struct DensitySampler::ColumnScope::State {
    int x, z, first_x, first_z;
    ScopeLease scope;
};
DensitySampler::ColumnScope::ColumnScope(const DensitySampler& sampler, int x, int z, NoiseScope noise_scope)
    : sampler_(&sampler) {
    const auto& program = sampler.impl_->program;
    const auto width = program.cell_width;
    state_ = std::make_unique<State>(
        State{x, z, floor_div(x, width) * width, floor_div(z, width) * width, ScopeLease(program, true)});
    if (noise_scope == NoiseScope::chunk)
        state_->scope.get()->flat_span = std::max(width, 16);
    // Corners stay on multiples of four only when cells are; see AreaCorners.
    if (program.cell_count > 0 && width % 4 == 0 && width <= AreaCorners::size)
        state_->scope.get()->corners = AreaCorners::acquire(sampler.impl_->instance->id(), program, x, z);
}
DensitySampler::ColumnScope::~ColumnScope() = default;
DensitySampler::ColumnScope::ColumnScope(ColumnScope&&) noexcept = default;
DensitySampler::ColumnScope& DensitySampler::ColumnScope::operator=(ColumnScope&&) noexcept = default;
int DensitySampler::ColumnScope::x() const {
    return state_->x;
}
int DensitySampler::ColumnScope::z() const {
    return state_->z;
}
void DensitySampler::ColumnScope::visit(std::string_view output, bool filling,
                                        const std::function<bool(BlockPos, double)>& visitor) {
    const auto& impl = *sampler_->impl_;
    const auto root = impl.output(output);
    auto& s = *state_;
    for (int i = impl.program.height - 1; i >= 0; --i) {
        const BlockPos p{s.x, impl.program.min_y + i, s.z};
        const double value = impl.instance->evaluate(
            root, {p, filling ? Mode::filling_cell : Mode::interpolated, s.first_x, s.first_z, s.scope.get()});
        if (!visitor(p, value))
            break;
    }
}
double DensitySampler::ColumnScope::column(std::string_view output, int y, bool filling) {
    const auto& impl = *sampler_->impl_;
    auto& s = *state_;
    return impl.instance->evaluate(impl.output(output), {{s.x, y, s.z},
                                                         filling ? Mode::filling_cell : Mode::interpolated,
                                                         s.first_x,
                                                         s.first_z,
                                                         s.scope.get()});
}
double DensitySampler::ColumnScope::direct(std::string_view output, BlockPos p) {
    return direct(sampler_->impl_->output(output), p);
}
double DensitySampler::ColumnScope::direct(std::uint32_t output, BlockPos p) {
    const auto& impl = *sampler_->impl_;
    auto& s = *state_;
    return impl.instance->evaluate(output, {p, Mode::corner, s.first_x, s.first_z, s.scope.get()});
}
void DensitySampler::ColumnScope::remember(std::uint32_t output, BlockPos p, double value) {
    const auto& impl = *sampler_->impl_;
    const auto& n = impl.program.code[output];
    if (!n.memo || !n.y_free)
        return;
    auto& memo = state_->scope.get()->memo;
    // The key evaluate_memo looks up for a corner-mode evaluation at p, as direct() makes.
    const BlockPos key{p.x, 0, p.z};
    memo.line(n.memo_slot, key) = {key.x, key.y, key.z, memo.stamp(static_cast<std::uint32_t>(Mode::corner)), value};
}
int DensitySampler::ColumnScope::preliminary_surface(int x, int z) {
    return static_cast<int>(direct("preliminary_surface_level", {floor_div(x, 4) * 4, 0, floor_div(z, 4) * 4}));
}
std::int64_t quantize_climate(float value) noexcept {
    const float scaled = value * 10000.0F;
    // Java f2l maps NaN to zero and saturates; C++ casts outside the range are undefined.
    if (std::isnan(scaled))
        return 0;
    if (scaled >= 0x1p63F)
        return std::numeric_limits<std::int64_t>::max();
    if (scaled <= -0x1p63F)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(scaled);
}
biome::ClimateTarget DensitySampler::climate(QuartPos p) const {
    const auto& outputs = impl_->climate_roots;
    biome::ClimateTarget target{};
    const auto to_block = [](std::int32_t q) {
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(q) * 4U);
    };
    const BlockPos block{to_block(p.x), to_block(p.y), to_block(p.z)};
    const ScopeLease scope(impl_->program, false);
    for (std::size_t i = 0; i < outputs.size(); ++i)
        target.values[i] =
            quantize_climate(static_cast<float>(impl_->instance->sample(outputs[i], block, Mode::direct, scope.get())));
    return target;
}
int DensitySampler::preliminary_surface(int x, int z) const {
    return preliminary_surface(x, z, x, z);
}
int DensitySampler::preliminary_surface(int x, int z, int cx, int cz) const {
    return ColumnScope(*this, cx, cz).preliminary_surface(x, z);
}
bool DensitySampler::preliminary_surface_column_free() const {
    return impl_->program.column_free(impl_->program.output("preliminary_surface_level"));
}
int DensitySampler::min_y() const {
    return impl_->program.min_y;
}
int DensitySampler::height() const {
    return impl_->program.height;
}
int DensitySampler::cell_width() const {
    return impl_->program.cell_width;
}
int DensitySampler::cell_height() const {
    return impl_->program.cell_height;
}
int DensitySampler::sea_level() const {
    return impl_->program.sea_level;
}
bool DensitySampler::aquifers_enabled() const {
    return impl_->program.aquifers_enabled;
}
bool DensitySampler::ore_veins_enabled() const {
    return impl_->program.ore_veins_enabled;
}
std::string_view DensitySampler::default_block() const {
    return impl_->program.default_block;
}
std::string_view DensitySampler::default_fluid() const {
    return impl_->program.default_fluid;
}
const random::PositionalFactory& DensitySampler::aquifer_random() const {
    return impl_->instance->aquifer();
}
const random::PositionalFactory& DensitySampler::ore_random() const {
    return impl_->instance->ore();
}
}  // namespace minecraft::worldgen
