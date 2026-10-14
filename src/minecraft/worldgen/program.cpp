#include "program.hpp"
#include "rules.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace minecraft::worldgen {
namespace {
using Value = minecraft::worldgen::Json;

constexpr std::uint32_t in_progress = std::numeric_limits<std::uint32_t>::max();

std::string_view path(std::string_view id) {
    const auto colon = id.find(':');
    return colon == id.npos ? id : id.substr(colon + 1);
}

const Value& settings_for() {
    return field(field(rules(), "noise_settings"), "minecraft:overworld");
}

bool unary(const Op op) {
    switch (op) {
        case Op::clamp:
        case Op::abs:
        case Op::square:
        case Op::cube:
        case Op::half_negative:
        case Op::quarter_negative:
        case Op::invert:
        case Op::squeeze:
            return true;
        default:
            return false;
    }
}

class Compiler {
public:
    explicit Compiler(Program& program) : program_(program) {}

    std::uint32_t compile(const Value& value) {
        if (value.is_number())
            return constant(number(value));
        if (value.is_string())
            return reference(string(value));
        const auto type = path(string(field(value, "type")));
        if (type == "spline")
            return spline(field(value, "spline"));
        if (type == "blend_alpha")
            return constant(1);
        if (type == "blend_offset" || type == "beardifier")
            return constant(0);
        if (type == "constant")
            return constant(number(field(value, "argument")));
        // Caching markers only tell vanilla where to memoize; values are identical without them.
        if (type == "cache_2d" || type == "cache_once" || type == "cache_all_in_cell" || type == "blend_density")
            return compile(field(value, "argument"));

        static const std::map<std::string_view, Op> operations{{"add", Op::add},
                                                               {"mul", Op::mul},
                                                               {"min", Op::minimum},
                                                               {"max", Op::maximum},
                                                               {"clamp", Op::clamp},
                                                               {"abs", Op::abs},
                                                               {"square", Op::square},
                                                               {"cube", Op::cube},
                                                               {"half_negative", Op::half_negative},
                                                               {"quarter_negative", Op::quarter_negative},
                                                               {"invert", Op::invert},
                                                               {"squeeze", Op::squeeze},
                                                               {"y_clamped_gradient", Op::gradient},
                                                               {"noise", Op::noise},
                                                               {"old_blended_noise", Op::blended_noise},
                                                               {"shifted_noise", Op::shifted_noise},
                                                               {"shift", Op::shift},
                                                               {"shift_a", Op::shift_a},
                                                               {"shift_b", Op::shift_b},
                                                               {"weird_scaled_sampler", Op::weird},
                                                               {"range_choice", Op::range_choice},
                                                               {"interpolated", Op::interpolated},
                                                               {"flat_cache", Op::flat_cache},
                                                               {"find_top_surface", Op::find_top_surface}};
        const auto it = operations.find(type);
        if (it == operations.end())
            throw std::runtime_error("Unknown density operator: " + std::string(type));
        Instruction n{it->second};
        std::size_t arity = 0;
        const auto arg = [&](std::string_view key) {
            n.args[arity++] = compile(field(value, key));
        };
        const auto param = [&](std::size_t i, std::string_view key) {
            n.p[i] = number(field(value, key));
        };
        switch (n.op) {
            case Op::add:
            case Op::mul:
            case Op::minimum:
            case Op::maximum:
                arg("argument1");
                arg("argument2");
                break;
            case Op::clamp:
                arg("input");
                param(0, "min");
                param(1, "max");
                break;
            case Op::gradient:
                param(0, "from_y");
                param(1, "to_y");
                param(2, "from_value");
                param(3, "to_value");
                break;
            case Op::noise:
                n.noise = noise(string(field(value, "noise")));
                param(0, "xz_scale");
                param(1, "y_scale");
                break;
            case Op::blended_noise:
                n.noise = blended(value);
                break;
            case Op::shifted_noise:
                arg("shift_x");
                arg("shift_y");
                arg("shift_z");
                n.noise = noise(string(field(value, "noise")));
                param(0, "xz_scale");
                param(1, "y_scale");
                break;
            case Op::shift:
            case Op::shift_a:
            case Op::shift_b:
                n.noise = noise(string(field(value, "argument")));
                break;
            case Op::weird:
                arg("input");
                n.noise = noise(string(field(value, "noise")));
                n.p[0] = string(field(value, "rarity_value_mapper")) == "type_2" ? 2 : 1;
                break;
            case Op::range_choice:
                arg("input");
                arg("when_in_range");
                arg("when_out_of_range");
                param(0, "min_inclusive");
                param(1, "max_exclusive");
                break;
            case Op::find_top_surface:
                arg("density");
                arg("upper_bound");
                param(0, "lower_bound");
                param(1, "cell_height");
                break;
            default:
                arg("argument");
                break;
        }
        return fold(n);
    }

private:
    Program& program_;
    std::map<std::string, std::uint32_t, std::less<>> functions_;
    std::map<std::string, std::uint32_t, std::less<>> noises_;

    std::uint32_t emit(const Instruction& n) {
        program_.code.push_back(n);
        return static_cast<std::uint32_t>(program_.code.size() - 1);
    }
    std::uint32_t constant(const double value) {
        Instruction n{Op::constant};
        n.p[0] = value;
        return emit(n);
    }
    const Instruction& at(const std::uint32_t index) const {
        return program_.code[index];
    }
    bool is_constant(const std::uint32_t index) const {
        return at(index).op == Op::constant;
    }

    // Folds operations whose inputs are all constants, using the evaluator's own arithmetic.
    std::uint32_t fold(const Instruction& n) {
        if (unary(n.op) && is_constant(n.args[0]))
            return constant(apply_unary(n.op, at(n.args[0]).p[0], n.p));
        switch (n.op) {
            case Op::add:
            case Op::mul:
            case Op::minimum:
            case Op::maximum:
                if (is_constant(n.args[0]) && is_constant(n.args[1])) {
                    const auto a = at(n.args[0]).p[0], b = at(n.args[1]).p[0];
                    return constant(n.op == Op::add       ? a + b
                                    : n.op == Op::mul     ? (a == 0.0 ? 0.0 : a * b)
                                    : n.op == Op::minimum ? std::min(a, b)
                                                          : std::max(a, b));
                }
                break;
            case Op::range_choice:
                if (is_constant(n.args[0])) {
                    const auto d = at(n.args[0]).p[0];
                    return d >= n.p[0] && d < n.p[1] ? n.args[1] : n.args[2];
                }
                break;
            case Op::flat_cache:
                // Every mode returns the child's value, and a constant has the same value everywhere.
                if (is_constant(n.args[0]))
                    return n.args[0];
                break;
            default:
                break;
        }
        return emit(n);
    }

    std::uint32_t reference(std::string_view id) {
        if (const auto it = functions_.find(id); it != functions_.end()) {
            if (it->second == in_progress)
                throw std::runtime_error("Cyclic density reference: " + std::string(id));
            return it->second;
        }
        functions_.emplace(id, in_progress);
        const auto index = compile(field(field(rules(), "density_functions"), id));
        functions_.find(id)->second = index;
        return index;
    }

    std::uint32_t spline(const Value& value) {
        if (value.is_number())
            return constant(number(value));
        Instruction n{Op::spline};
        n.args[0] = compile(field(value, "coordinate"));
        std::vector<SplinePoint> points;
        for (const auto& point : std::get<minecraft::worldgen::JsonArray>(field(value, "points")))
            points.push_back({static_cast<float>(number(field(point, "location"))),
                              static_cast<float>(number(field(point, "derivative"))), spline(field(point, "value"))});
        if (points.empty())
            throw std::runtime_error("Empty density spline");
        n.points_begin = static_cast<std::uint32_t>(program_.points.size());
        program_.points.insert(program_.points.end(), points.begin(), points.end());
        n.points_end = static_cast<std::uint32_t>(program_.points.size());
        return emit(n);
    }

    std::uint32_t noise(std::string_view id) {
        if (const auto it = noises_.find(id); it != noises_.end())
            return it->second;
        const auto& parameters = field(field(rules(), "noise_parameters"), id);
        NoiseSpec spec{NoiseSpec::Kind::normal, std::string(id), {}, {}};
        spec.parameters.first_octave = static_cast<int>(number(field(parameters, "firstOctave")));
        for (const auto& amplitude : std::get<minecraft::worldgen::JsonArray>(field(parameters, "amplitudes")))
            spec.parameters.amplitudes.push_back(number(amplitude));
        program_.noises.push_back(std::move(spec));
        const auto index = static_cast<std::uint32_t>(program_.noises.size() - 1);
        noises_.emplace(id, index);
        return index;
    }

    std::uint32_t blended(const Value& value) {
        NoiseSpec spec{NoiseSpec::Kind::blended, {}, {}, {}};
        spec.blended = {number(field(value, "xz_scale")), number(field(value, "y_scale")),
                        number(field(value, "xz_factor")), number(field(value, "y_factor")),
                        number(field(value, "smear_scale_multiplier"))};
        program_.noises.push_back(std::move(spec));
        return static_cast<std::uint32_t>(program_.noises.size() - 1);
    }
};

std::size_t arity(const Instruction& n) {
    switch (n.op) {
        case Op::constant:
        case Op::gradient:
        case Op::noise:
        case Op::blended_noise:
        case Op::shift:
        case Op::shift_a:
        case Op::shift_b:
            return 0;
        case Op::add:
        case Op::mul:
        case Op::minimum:
        case Op::maximum:
        case Op::find_top_surface:
            return 2;
        case Op::shifted_noise:
        case Op::range_choice:
            return 3;
        default:
            return 1;
    }
}

template <class Visit> void for_each_child(const Program& program, const Instruction& n, Visit&& visit) {
    for (std::size_t i = 0; i < arity(n); ++i)
        visit(n.args[i]);
    for (auto i = n.points_begin; i < n.points_end; ++i)
        visit(program.points[i].value);
}

// Derives what evaluation scopes may memoize. y_free must be conservative: a false positive would reuse a value
// computed at a different block y.
void analyze(Program& program) {
    auto& code = program.code;
    std::vector<int> references(code.size());
    std::vector<bool> reachable(code.size());
    std::vector<std::uint32_t> stack;
    for (const auto& [name, root] : program.outputs) {
        ++references[root];
        stack.push_back(root);
    }
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        if (reachable[index])
            continue;
        reachable[index] = true;
        for_each_child(program, code[index], [&](std::uint32_t child) {
            ++references[child];
            stack.push_back(child);
        });
    }

    // Children precede their parents, so one forward pass sees every child first.
    for (std::uint32_t index = 0; index < code.size(); ++index) {
        auto& n = code[index];
        bool args_y_free = true, args_costly = false;
        for_each_child(program, n, [&](std::uint32_t child) {
            args_y_free = args_y_free && code[child].y_free;
            args_costly = args_costly || code[child].costly;
        });
        switch (n.op) {
            case Op::constant:
                n.y_free = true;
                break;
            case Op::noise:
                // y * 0.0 is a signed zero, which the noise origin offset absorbs.
                n.y_free = n.p[1] == 0.0;
                n.costly = true;
                break;
            case Op::blended_noise:
            case Op::shift:
            case Op::weird:
                n.costly = true;
                break;
            case Op::shifted_noise:
                n.y_free = args_y_free && n.p[1] == 0.0;
                n.costly = true;
                break;
            case Op::shift_a:
            case Op::shift_b:
                n.y_free = true;
                n.costly = true;
                break;
            case Op::gradient:
                break;
            case Op::interpolated:
                n.costly = args_costly;
                if (reachable[index])
                    n.cell_slot = program.cell_count++;
                break;
            case Op::find_top_surface:
                // The scan replaces y, so only the upper bound sees the caller's position.
                n.y_free = code[n.args[1]].y_free;
                n.costly = true;
                break;
            default:
                n.y_free = args_y_free;
                n.costly = args_costly;
                break;
        }
        n.memo = n.costly && n.op != Op::interpolated && (n.y_free || references[index] > 1);
        // Operations whose rounded result is monotone in each argument, so endpoint intervals map to intervals.
        switch (n.op) {
            case Op::blended_noise:
                n.bounded = true;
                break;
            case Op::add:
            case Op::mul:
            case Op::minimum:
            case Op::maximum:
            case Op::clamp:
            case Op::half_negative:
            case Op::quarter_negative:
                for_each_child(program, n, [&](std::uint32_t child) { n.bounded = n.bounded || code[child].bounded; });
                break;
            default:
                break;
        }
    }

    // A single-use child of a y-free parent runs only when the parent misses, so caching it cannot hit. flat_cache
    // is excluded because it moves its child to a snapped position that other parent keys share.
    for (const auto& n : code)
        if (n.y_free && n.op != Op::flat_cache)
            for_each_child(program, n, [&](std::uint32_t child) {
                if (references[child] == 1)
                    code[child].memo = false;
            });
    for (std::uint32_t index = 0; index < code.size(); ++index)
        if (code[index].memo && reachable[index])
            code[index].memo_slot = program.memo_count++;
        else
            code[index].memo = false;
}

Program compile() {
    Program program;
    const auto& settings = settings_for();
    const auto& noise = field(settings, "noise");
    program.cell_width = static_cast<int>(number(field(noise, "size_horizontal"))) * 4;
    program.cell_height = static_cast<int>(number(field(noise, "size_vertical"))) * 4;
    program.min_y = static_cast<int>(number(field(noise, "min_y")));
    program.height = static_cast<int>(number(field(noise, "height")));
    program.sea_level = static_cast<int>(number(field(settings, "sea_level")));
    program.legacy_random = std::get<bool>(field(settings, "legacy_random_source"));
    program.aquifers_enabled = std::get<bool>(field(settings, "aquifers_enabled"));
    program.ore_veins_enabled = std::get<bool>(field(settings, "ore_veins_enabled"));
    program.default_block = string(field(field(settings, "default_block"), "Name"));
    program.default_fluid = string(field(field(settings, "default_fluid"), "Name"));
    // Scopes index memo lines by cell parity, which needs power-of-two cells.
    if (!std::has_single_bit(static_cast<unsigned>(program.cell_width)) ||
        !std::has_single_bit(static_cast<unsigned>(program.cell_height)))
        throw std::runtime_error("Noise cells must have power-of-two sizes");
    Compiler compiler(program);
    for (const auto& [id, value] : std::get<minecraft::worldgen::JsonObject>(field(settings, "noise_router")))
        program.outputs.emplace(id, compiler.compile(value));
    analyze(program);
    return program;
}

}  // namespace

double apply_unary(const Op op, const double value, const std::array<double, 4>& p) noexcept {
    switch (op) {
        case Op::clamp:
            return std::clamp(value, p[0], p[1]);
        case Op::abs:
            return std::abs(value);
        case Op::square:
            return value * value;
        case Op::cube:
            return value * value * value;
        case Op::half_negative:
            return value > 0 ? value : value * .5;
        case Op::quarter_negative:
            return value > 0 ? value : value * .25;
        case Op::invert:
            return 1 / value;
        case Op::squeeze: {
            const auto d = std::clamp(value, -1.0, 1.0);
            return d / 2 - d * d * d / 24;
        }
        default:
            return value;
    }
}

const Program& Program::get(const Dimension dimension) {
    if (dimension != Dimension::overworld)
        throw std::invalid_argument("Only Overworld density programs are supported");
    static const Program program = compile();
    return program;
}

std::uint32_t Program::output(const std::string_view name) const {
    const auto it = outputs.find(name);
    if (it == outputs.end())
        throw std::out_of_range("Unknown density router output: " + std::string(name));
    return it->second;
}

bool Program::column_free(const std::uint32_t root) const {
    // Only interpolated cells and flat_cache range checks read the cell origin. A flat_cache over a y-free child
    // yields the same value in or out of range at quart-aligned positions.
    std::vector<bool> seen(code.size());
    std::vector<std::uint32_t> stack{root};
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        if (seen[index])
            continue;
        seen[index] = true;
        const auto& n = code[index];
        if (n.op == Op::interpolated || (n.op == Op::flat_cache && !code[n.args[0]].y_free))
            return false;
        for_each_child(*this, n, [&](std::uint32_t child) {
            stack.push_back(child);
        });
    }
    return true;
}

}  // namespace minecraft::worldgen
