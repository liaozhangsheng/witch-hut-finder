#include "search/witch_hut_search.hpp"

#include <iostream>
#include <tuple>

namespace {
void check(bool condition) {
    if (!condition)
        throw std::runtime_error("Threshold search regression");
}

auto positions(const witch_hut::Result& result, int limit) {
    std::vector<std::tuple<int, int, int>> out;
    for (const auto& hut : result.huts)
        if (hut.point.y < limit)
            out.emplace_back(hut.point.x, hut.point.y, hut.point.z);
    std::sort(out.begin(), out.end());
    return out;
}

void check_result(const witch_hut::Result& result, int limit) {
    check(std::is_sorted(result.huts.begin(), result.huts.end(),
                         [](const auto& a, const auto& b) { return a.point.y < b.point.y; }));
    for (const auto& hut : result.huts)
        check(hut.point.y < limit);
}
}  // namespace

int main() {
    try {
        // This fixture has eight huts, including four with floor y == 64.
        const auto all = witch_hut::search(42, 10000, 1, witch_hut::max_floor_limit);
        check(all.scan_complete && all.huts.size() == 8);
        check_result(all, witch_hut::max_floor_limit);
        const auto expected = positions(all, 65);
        check(expected.size() == 4);
        for (unsigned workers : {1U, 2U}) {
            const auto below = witch_hut::search(42, 10000, workers, 65);
            check(below.scan_complete && positions(below, 65) == expected);
            check_result(below, 65);
        }
        check(witch_hut::search(42, 10000, 1, 64).huts.empty());
        const auto low = witch_hut::search(42, 10000, 1, -32);
        check(low.scan_complete && low.huts.empty());
        check(witch_hut::search(42, 10000, 1, witch_hut::min_floor_limit).huts.empty());
        for (int limit : {witch_hut::min_floor_limit - 1, witch_hut::max_floor_limit + 1}) {
            bool rejected = false;
            try {
                (void)witch_hut::search(42, 1, 1, limit);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            check(rejected);
        }
        unsigned snapshots = 0;
        bool stop = false;
        const auto final = witch_hut::search(42, witch_hut::world_edge, 1, 322, [&] { return stop; }, [&](const auto& result, auto) {
            ++snapshots;
            check_result(result, 322);
            stop = true;
        });
        check(!final.scan_complete && snapshots > 0);
        check_result(final, 322);
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
