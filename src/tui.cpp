#include "search/witch_hut_search.hpp"

#include <minecraft/types.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace ftxui;
using Clock = std::chrono::steady_clock;

// One accent, one success tone and two greys; everything else is the terminal's own foreground.
const Color accent = Color::RGB(187, 154, 247);
const Color moss = Color::RGB(158, 206, 106);
const Color amber = Color::RGB(224, 175, 104);
const Color rose = Color::RGB(247, 118, 142);
const Color muted = Color::RGB(118, 124, 148);
const Color line = Color::RGB(65, 72, 104);

constexpr unsigned default_top = 1;

std::string grouped(std::int64_t value) {
    auto digits = std::to_string(value < 0 ? -value : value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<std::size_t>(i), ",");
    return value < 0 ? "-" + digits : digits;
}

std::string compact(double value) {
    constexpr std::array<std::string_view, 5> units{"", "k", "M", "G", "T"};
    std::size_t unit = 0;
    while (value >= 1000 && unit + 1 < units.size()) {
        value /= 1000;
        ++unit;
    }
    if (!unit)
        return std::format("{:.0f}", value);
    return std::format("{:.{}f}{}", value, value < 10 ? 2 : value < 100 ? 1 : 0, units[unit]);
}

std::string duration(double seconds) {
    auto s = static_cast<std::uint64_t>(std::max(seconds, 0.0));
    const auto days = s / 86400, hours = s / 3600 % 24, minutes = s / 60 % 60;
    s %= 60;
    if (days)
        return std::format("{}d {:02}h", days, hours);
    if (hours)
        return std::format("{}h {:02}m", hours, minutes);
    if (minutes)
        return std::format("{}m {:02}s", minutes, s);
    if (seconds < 10)
        return std::format("{:.1f}s", seconds);
    return std::format("{}s", s);
}

std::string percent(double fraction) {
    const double value = fraction * 100;
    if (value == 0)
        return "0%";
    if (value < 0.001)
        return "<0.001%";
    return std::format("{:.{}f}%", value, value > 0 && value < 0.1 ? 3 : value < 100 ? 2 : 0);
}

std::string sparkline(const std::deque<double>& samples) {
    constexpr std::array<std::string_view, 8> bars{"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const double peak = samples.empty() ? 0 : *std::max_element(samples.begin(), samples.end());
    std::string out;
    for (const double sample : samples)
        out += bars[peak > 0 ? std::min<std::size_t>(7, static_cast<std::size_t>(sample / peak * 8)) : 0];
    return out;
}

// A thin horizontal bar in the style of modern CLI progress meters: heavy line, half-cell precision,
// and a track drawn in the same glyph so the bar reads as one continuous rule.
class Meter : public Node {
public:
    Meter(float fraction, Color fill, Color track) : fraction_(std::clamp(fraction, 0.0F, 1.0F)), fill_(fill), track_(track) {}

    void ComputeRequirement() override {
        requirement_ = {};
        requirement_.min_x = 1;
        requirement_.min_y = 1;
        requirement_.flex_grow_x = 1;
        requirement_.flex_shrink_x = 1;
    }

    void Render(Screen& screen) override {
        const int width = box_.x_max - box_.x_min + 1;
        const int halves = static_cast<int>(std::lround(fraction_ * static_cast<float>(width) * 2));
        for (int y = box_.y_min; y <= box_.y_max; ++y)
            for (int i = 0; i < width; ++i) {
                auto& cell = screen.CellAt(box_.x_min + i, y);
                const bool full = i < halves / 2, half = i == halves / 2 && halves % 2;
                cell.character = half ? "╸" : "━";
                cell.foreground_color = full || half ? fill_ : track_;
            }
    }

private:
    float fraction_;
    Color fill_, track_;
};

Element meter(double fraction, Color fill = accent) {
    return std::make_shared<Meter>(static_cast<float>(fraction), fill, line);
}

// A rounded box whose border and title are tinted while its content keeps the terminal's colors.
Element panel(std::string_view title, Element content, Color edge = line, Color heading = muted) {
    return dbox({
        std::move(content) | borderStyled(ROUNDED, edge),
        // Keep title styles on the top row instead of repainting the content below.
        vbox({hbox({text("╭") | color(edge), text(std::format(" {} ", title)) | bold | color(heading)}), filler()}),
    });
}

Element pair(std::string_view label, std::string value, int width = 10) {
    return hbox({text(std::string(label)) | color(muted) | size(WIDTH, EQUAL, width), text(std::move(value))});
}

struct Record {
    double seconds;
    witch_hut::Hut hut;
};

// One scan and the thread running it. Destroying a Scan cancels and joins the search.
struct Scan {
    std::uint64_t seed = 0;
    int edge = witch_hut::world_edge;
    unsigned workers = 1;
    unsigned top = default_top;
    Clock::time_point started = Clock::now();

    std::mutex mutex;
    witch_hut::Result result;
    std::uint64_t total = 0;
    std::map<std::pair<int, int>, double> found;  // Seconds into the scan each listed hut first appeared, by x and z.
    std::vector<Record> records;                  // Each time the lowest floor dropped, oldest first.
    std::deque<double> rates;                     // regions per second, one sample per second
    std::string error;
    Clock::time_point finished;

    std::atomic<bool> stop{false}, done{false};
    std::jthread thread;  // Last, so it joins before anything it touches is destroyed.

    ~Scan() {
        stop = true;
    }

    void run() {
        auto sampled_at = started;
        std::uint64_t sampled_regions = 0;
        const auto record = [&](const witch_hut::Result& latest, std::uint64_t regions_total) {
            const auto now = Clock::now();
            const std::lock_guard lock(mutex);
            const double seconds = std::chrono::duration<double>(now - started).count();
            for (const auto& hut : latest.huts)
                found.try_emplace({hut.point.x, hut.point.z}, seconds);
            if (!latest.huts.empty() && (records.empty() || latest.huts.front().point.y < records.back().hut.point.y))
                records.push_back({seconds, latest.huts.front()});
            result = latest;
            if (regions_total)
                total = regions_total;
            if (const double dt = std::chrono::duration<double>(now - sampled_at).count(); dt >= 1) {
                rates.push_back(static_cast<double>(latest.regions - sampled_regions) / dt);
                if (rates.size() > 48)
                    rates.pop_front();
                sampled_at = now;
                sampled_regions = latest.regions;
            }
        };
        try {
            const auto final = witch_hut::search(seed, edge, workers, top, [this] { return stop.load(); }, record);
            record(final, 0);
        } catch (const std::exception& failure) {
            const std::lock_guard lock(mutex);
            error = failure.what();
        }
        {
            const std::lock_guard lock(mutex);
            finished = Clock::now();
        }
        done = true;
    }
};

// A consistent copy of a Scan for drawing one frame.
struct View {
    witch_hut::Result result;
    std::uint64_t total = 0;
    std::map<std::pair<int, int>, double> found;
    std::vector<Record> records;
    std::deque<double> rates;
    std::string error;
    double elapsed = 0;
    bool done = false, stopping = false;

    explicit View(Scan& scan) {
        const std::lock_guard lock(scan.mutex);
        result = scan.result;
        found = scan.found;
        records = scan.records;
        rates = scan.rates;
        error = scan.error;
        done = scan.done;
        stopping = scan.stop && !done;
        // A scan that finishes before its first progress report still has a known size.
        total = scan.total ? scan.total : result.scan_complete ? result.regions : 0;
        elapsed = std::chrono::duration<double>((done ? scan.finished : Clock::now()) - scan.started).count();
    }
};

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

template <class T>
bool parse(std::string_view text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

Element status_chip(const Scan* scan, const View* view) {
    const auto chip = [](std::string_view label, Color tone) {
        return hbox({text("● ") | color(tone), text(std::string(label)) | color(tone) | bold});
    };
    if (!scan)
        return chip("Ready", muted);
    if (!view->error.empty())
        return chip("Error", rose);
    if (view->stopping)
        return chip("Stopping", amber);
    if (!view->done)
        return chip("Scanning", accent);
    return view->result.scan_complete ? chip("Complete", moss) : chip("Stopped", amber);
}

Element progress_panel(const Scan* scan, const View* view) {
    if (!scan)
        return panel("Progress", vbox({
                                     text("Enter a seed and press Start.") | color(muted),
                                     meter(0),
                                     text("Leave radius empty to scan the whole world border.") | color(muted),
                                 }));
    const auto& r = view->result;
    const double fraction = view->total ? static_cast<double>(r.regions) / static_cast<double>(view->total) : 0;
    const double rate = view->elapsed > 0 ? static_cast<double>(r.regions) / view->elapsed : 0;

    constexpr std::array<std::string_view, 10> frames{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    const auto glyph = view->done ? text(r.scan_complete ? "✓" : "■") | color(r.scan_complete ? moss : amber)
                                  : text(std::string(frames[static_cast<std::size_t>(view->elapsed * 12) % frames.size()])) |
                                        color(accent);
    const auto heading = hbox({
        glyph,
        text(std::format(" seed {}", std::bit_cast<std::int64_t>(scan->seed))) | bold,
        text(std::format("  ·  radius {}  ·  {} threads", grouped(scan->edge), scan->workers)) | color(muted),
        filler(),
        text(view->total ? percent(fraction) : "—") | bold,
    });

    std::string eta = "—";
    if (!view->done && rate > 0 && view->total > r.regions)
        eta = duration(static_cast<double>(view->total - r.regions) / rate);
    const auto stat = [](std::string_view label, std::string value) {
        return hbox({text(std::string(label)) | color(muted), text(" " + value), text("    ")});
    };
    const auto stats = hbox({
        stat("regions", view->total ? compact(static_cast<double>(r.regions)) + " / " +
                                          compact(static_cast<double>(view->total))
                                    : compact(static_cast<double>(r.regions))),
        stat("rate", compact(view->rates.empty() ? rate : view->rates.back()) + "/s"),
        stat("elapsed", duration(view->elapsed)),
        stat("eta", eta),
        filler(),
        text(sparkline(view->rates)) | color(accent),
    });
    return panel("Progress", vbox({heading, meter(fraction, view->done && r.scan_complete ? moss : accent), stats}));
}

Element best_panel(const Scan* scan, const View* view) {
    Element body;
    if (!scan || view->result.huts.empty()) {
        body = vbox({
            text(scan && !view->done ? "Searching…" : "No hut yet") | color(muted),
            text(""),
            paragraph("The lowest verified swamp hut floor will appear here.") | color(muted),
        });
    } else {
        const auto& best = view->result.huts.front();
        const auto& p = best.point;
        const auto& g = best.generation_point;
        Elements rows{
            hbox({text("floor y") | color(muted) | size(WIDTH, EQUAL, 10), text(std::to_string(p.y)) | bold | color(accent)}),
            text(""),
            pair("witch y", std::to_string(p.y + 1)),
            pair("x  z", std::format("{}  {}", p.x, p.z)),
            pair("chunk", std::format("{}  {}", minecraft::floor_div(p.x, 16), minecraft::floor_div(p.z, 16))),
            pair("gen. y", std::to_string(g.y)),
            text(""),
            text(std::format("/tp @s {} {} {}", p.x, p.y + 1, p.z)) | color(muted),
        };
        if (best.height_uncertain)
            rows.push_back(text("▲ height uncertain") | color(amber));
        body = vbox(std::move(rows));
    }
    return panel("Lowest floor", body) | size(WIDTH, EQUAL, 36);
}

Element funnel_panel(const Scan* scan, const View* view) {
    const witch_hut::Result empty;
    const auto& r = scan ? view->result : empty;
    const double base = static_cast<double>(std::max<std::uint64_t>(r.candidates, 1));
    const auto row = [&](std::string_view label, std::uint64_t count, Color tone) {
        return hbox({
            text(std::string(label)) | color(muted) | size(WIDTH, EQUAL, 16),
            meter(r.candidates ? static_cast<double>(count) / base : 0, tone),
            text(r.candidates ? percent(static_cast<double>(count) / base) : "") | align_right | size(WIDTH, EQUAL, 9),
            text(compact(static_cast<double>(count))) | align_right | size(WIDTH, EQUAL, 8),
        });
    };
    return panel("Screening", vbox({
                                  pair("candidates", grouped(static_cast<std::int64_t>(r.candidates)), 16),
                                  pair("tiles skipped",
                                       std::format("{}  ({} regions)", grouped(static_cast<std::int64_t>(r.tiles_skipped)),
                                                   compact(static_cast<double>(r.regions_skipped))),
                                       16),
                                  text(""),
                                  row("proxy screen", r.proxy_rejected, accent),
                                  row("climate", r.filtered, accent),
                                  row("center probe", r.direct_bounded, accent),
                                  row("floor preview", r.floor_preview_rejected, accent),
                                  row("floor bound", r.bounded, accent),
                                  row("height checks", r.heights, moss),
                              })) |
           flex;
}

Element cell(std::string value, int width) {
    return text(std::move(value)) | align_right | size(WIDTH, EQUAL, width);
}

Element uncertainty(bool uncertain) {
    return uncertain ? text("  ▲") | color(amber) : text("   ");
}

// Columns: rank, floor y, [witch y], x, z, [chunk], found. full adds the bracketed ones, which follow from the rest.
Element ranking_panel(const Scan* scan, const View* view, unsigned top, bool full) {
    const auto row = [&](std::string rank, std::string floor, std::string witch, std::string x, std::string z,
                         std::string chunk, Element found) {
        Elements cells{cell(std::move(rank), 4), cell(std::move(floor), 9)};
        if (full)
            cells.push_back(cell(std::move(witch), 9));
        cells.push_back(cell(std::move(x), 11));
        cells.push_back(cell(std::move(z), 11));
        if (full)
            cells.push_back(cell(std::move(chunk), 20));
        cells.push_back(std::move(found) | align_right | size(WIDTH, EQUAL, 9));
        return hbox(std::move(cells));
    };
    Elements rows{hbox({row("#", "floor y", "witch y", "x", "z", "chunk", text("found")), uncertainty(false)}) |
                  color(muted)};
    const auto& huts = scan ? view->result.huts : std::vector<witch_hut::Hut>{};
    const auto found_at = [&](const witch_hut::Hut& hut) {
        const auto it = view->found.find({hut.point.x, hut.point.z});
        return it == view->found.end() ? -1.0 : it->second;
    };
    // Huts from the latest update that changed the list get an accented time.
    double newest = -1;
    for (const auto& hut : huts)
        newest = std::max(newest, found_at(hut));
    for (std::size_t i = 0; i < huts.size(); ++i) {
        const auto& p = huts[i].point;
        const double seconds = found_at(huts[i]);
        auto found = text(seconds < 0 ? "" : duration(seconds)) | color(seconds == newest ? accent : muted);
        auto line = row(std::to_string(i + 1), std::to_string(p.y), std::to_string(p.y + 1), std::to_string(p.x),
                        std::to_string(p.z), std::format("{} {}", minecraft::floor_div(p.x, 16), minecraft::floor_div(p.z, 16)),
                        std::move(found));
        if (i == 0)
            line |= bold;
        rows.push_back(hbox({line, uncertainty(huts[i].height_uncertain)}));
    }
    if (huts.empty())
        rows.push_back(paragraph(std::format("The {} lowest huts found will be listed here.", top)) | color(muted));
    const auto title = scan ? std::format("Lowest {}  ·  {} found", top, huts.size()) : std::format("Lowest {}", top);
    return panel(title, vbox(std::move(rows)) | yframe);
}

// Every time the lowest floor dropped during the scan, newest first.
Element records_panel(const Scan* scan, const View* view) {
    Elements rows{hbox({cell("time", 8), cell("floor y", 9), cell("x", 11), cell("z", 11), uncertainty(false)}) |
                  color(muted)};
    if (scan)
        for (auto it = view->records.rbegin(); it != view->records.rend(); ++it) {
            const bool latest = it == view->records.rbegin();
            const auto& p = it->hut.point;
            rows.push_back(hbox({
                cell(duration(it->seconds), 8) | color(latest ? accent : muted),
                cell(std::to_string(p.y), 9) | (latest ? bold : nothing),
                cell(std::to_string(p.x), 11),
                cell(std::to_string(p.z), 11),
                uncertainty(it->hut.height_uncertain),
            }));
        }
    if (rows.size() == 1)
        rows.push_back(paragraph("Each new lowest floor is recorded here as the scan finds it.") | color(muted));
    const auto title = scan ? std::format("Record lows  ·  {}", view->records.size()) : std::string("Record lows");
    return panel(title, vbox(std::move(rows)) | yframe);
}

Element key(std::string_view name, std::string_view action) {
    return hbox({text(std::string(name)) | bold, text(std::format(" {}   ", action)) | color(muted)});
}

Component field(std::string label, std::string* value, std::string placeholder, int width,
                std::function<void()> submit, std::function<bool()> locked) {
    InputOption option;
    option.content = value;
    option.placeholder = std::move(placeholder);
    option.multiline = false;
    option.on_enter = std::move(submit);
    option.transform = [](InputState state) {
        if (state.is_placeholder)
            state.element |= color(muted);
        return state.element;
    };
    // Settings stay visible but frozen while a scan uses them.
    auto input = Input(option) | CatchEvent([locked](const Event& event) {
                     return locked() && (event.is_character() || event == Event::Backspace || event == Event::Delete);
                 });
    return Renderer(input, [input, locked, width, label = std::move(label)] {
        const bool active = input->Focused() && !locked();
        auto body = input->Render();
        if (locked())
            body |= dim;
        auto box = panel(label, body, active ? accent : line, active ? accent : muted);
        return width > 0 ? box | size(WIDTH, EQUAL, width) : box | flex;
    });
}
}  // namespace

int main() {
    const unsigned hardware = std::clamp(std::thread::hardware_concurrency(), 1U, 256U);
    std::string seed_text, radius_text, threads_text, top_text;
    std::string notice;

    auto screen = ScreenInteractive::Fullscreen();
    std::unique_ptr<Scan> scan;  // Declared after the screen, so its thread stops before the screen goes away.

    const auto running = [&] { return scan && !scan->done; };
    const auto start = [&] {
        if (running())
            return;
        const auto seed = minecraft::parse_seed(trim(seed_text));
        int edge = witch_hut::world_edge;
        unsigned workers = hardware, top = default_top;
        const auto radius = trim(radius_text), threads = trim(threads_text), count = trim(top_text);
        if (!seed)
            notice = "Seed must be a decimal signed 64-bit integer.";
        else if (!radius.empty() && !(parse(radius, edge) && edge >= 1 && edge <= witch_hut::world_edge))
            notice = std::format("Radius must be between 1 and {}.", grouped(witch_hut::world_edge));
        else if (!threads.empty() && !(parse(threads, workers) && workers >= 1 && workers <= 256))
            notice = "Threads must be between 1 and 256.";
        else if (!count.empty() && !(parse(count, top) && top >= 1 && top <= witch_hut::max_top))
            notice = std::format("Top must be between 1 and {}.", witch_hut::max_top);
        else
            notice.clear();
        if (!notice.empty())
            return;
        scan.reset();
        scan = std::make_unique<Scan>();
        scan->seed = *seed;
        scan->edge = edge;
        scan->workers = workers;
        scan->top = top;
        scan->thread = std::jthread([&screen, s = scan.get()] {
            s->run();
            screen.PostEvent(Event::Custom);
        });
    };
    const auto stop = [&] {
        if (running())
            scan->stop = true;
    };

    auto seed_field = field("Seed", &seed_text, "e.g. 42", 0, start, running);
    auto radius_field = field("Radius", &radius_text, "world", 14, start, running);
    auto threads_field = field("Threads", &threads_text, std::to_string(hardware), 11, start, running);
    auto top_field = field("Top", &top_text, std::to_string(default_top), 9, start, running);
    ButtonOption button_option;
    button_option.on_click = [&] { running() ? stop() : start(); };
    button_option.transform = [&](const EntryState& state) {
        const bool busy = running();
        const Color tone = busy ? amber : accent;
        auto label = text(busy ? "Stop" : "Start") | bold | center | size(WIDTH, EQUAL, 10);
        if (state.focused)
            label |= inverted;
        return label | borderRounded | color(tone);
    };
    auto button = Button(button_option);

    auto controls = Container::Horizontal({seed_field, radius_field, threads_field, top_field, button});
    auto app = Renderer(controls, [&] {
        std::optional<View> view;
        if (scan)
            view.emplace(*scan);
        const View* v = view ? &*view : nullptr;
        const auto header = hbox({
            text(" ✦ ") | color(accent),
            text("Witch Hut Finder") | bold,
            text("   Java 26.1 · Overworld") | color(muted),
            filler(),
            status_chip(scan.get(), v),
            text(" "),
        });
        std::string message = notice.empty() && v ? v->error : notice;
        const auto footer = hbox({
            text(" "),
            key("tab", "next"),
            key("enter", "start"),
            key("esc", "stop"),
            key("ctrl-c", "quit"),
            filler(),
            text("estimates · fast screening may miss lower huts ") | color(muted) | xflex_shrink,
        });
        Elements layout{header, text(""), controls->Render()};
        if (!message.empty())
            layout.push_back(text(" " + message) | color(rose));
        layout.push_back(progress_panel(scan.get(), v));
        layout.push_back(hbox({best_panel(scan.get(), v), funnel_panel(scan.get(), v)}));
        unsigned top = default_top;
        if (scan)
            top = scan->top;
        else if (unsigned typed; parse(trim(top_text), typed) && typed >= 1 && typed <= witch_hut::max_top)
            top = typed;
        // The ranking keeps every column when the record lows fit beside it, drops the derived ones when only the
        // two narrow tables fit side by side, and below that the record lows go under the ranking.
        const int width = Terminal::Size().dimx;
        const bool full = width >= 116;
        auto ranking = ranking_panel(scan.get(), v, top, full);
        auto records = records_panel(scan.get(), v);
        if (width >= 88)
            layout.push_back(hbox({std::move(ranking), std::move(records) | flex}) | flex);
        else
            layout.push_back(vbox({std::move(ranking) | flex, std::move(records) | flex}) | flex);
        layout.push_back(footer);
        return vbox(std::move(layout));
    });
    app |= CatchEvent([&](const Event& event) {
        if (event == Event::Escape) {
            stop();
            return true;
        }
        if (event == Event::CtrlC) {
            screen.Exit();
            return true;
        }
        return false;
    });

    // Redraws the spinner, elapsed time and counters while a scan runs.
    std::jthread ticker([&](std::stop_token token) {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (running())
                screen.PostEvent(Event::Custom);
        }
    });

    screen.ForceHandleCtrlC(false);
    screen.Loop(app);
    ticker.request_stop();
    ticker.join();

    if (!scan)
        return 0;
    scan->stop = true;
    scan->thread.join();
    // The screen is gone by now, so a failed search is still reported.
    if (const View view(*scan); !view.error.empty()) {
        std::cerr << view.error << '\n';
        return 1;
    }
    return 0;
}
