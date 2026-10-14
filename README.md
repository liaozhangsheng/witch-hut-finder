# Witch Hut Finder

Searches a Java Edition 26.1 overworld for the witch huts with the lowest floors. A hut's floor height is the average terrain height under its 63 floor blocks. The search relies on fast heuristic screening: it can miss lower huts, and a finished search does not prove that the global minimum was found.

## Download

Prebuilt binaries for Linux (x86_64), macOS (universal, 13.3 or later) and Windows (x86_64) are attached to each [release](../../releases). Each archive holds the `witch_hut_finder` executable, this README and the license.

The binaries are unsigned. On macOS, clear the quarantine flag before the first run:

```sh
xattr -d com.apple.quarantine witch_hut_finder
```

On Windows, run it in Windows Terminal; the legacy console may not draw the interface's box and block characters.

## Building

Requires CMake 3.20+ and a C++20 compiler whose standard library provides `std::format` and `std::jthread`, such as GCC 13+, Clang 18+, Apple Clang 15+ or MSVC 2022. Initialize the submodules after cloning:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target witch_hut_finder -j
./build/bin/witch_hut_finder
```

## Usage

The program is a terminal interface built with FTXUI:

- **Seed**: a signed 64-bit decimal seed.
- **Radius**: leave empty to search the whole world border `[-29999984,29999984)`; `BLOCKS` searches `[-BLOCKS,BLOCKS)` on both axes.
- **Threads**: leave empty for the hardware thread count, or enter 1 to 256.
- **Top**: how many of the lowest huts to keep; 1 when empty, up to 1000. A larger N leaves fewer candidates to prune, so the search runs slower.

Enter or Start begins a search, Esc stops it, and Ctrl-C quits. The interface shows live progress, the rate and estimated time remaining, the current lowest hut with its details and a teleport command, the share each screening stage rejected, and a ranking of the N lowest huts (floor height, witch height, coordinates, chunk and time found). Huts that entered the ranking in the latest update have their time highlighted.

Beside the ranking, Record lows lists, newest first, every time the lowest floor dropped during the search, with the time, floor height and coordinates. The interface updates every 0.2 seconds, so several drops within one update record only the lowest. On narrower terminals the ranking omits the witch y and chunk columns, which follow from the others; narrower still, Record lows moves below the ranking. A hut marked height uncertain (▲) has floor columns whose terrain could not be determined exactly, so its height may be off.

In the interface, floor y is the floor height, witch y (floor y + 1) is the height of a witch's feet, and gen. y is the height structure generation used for its biome check. The Complete status means every region within the radius was processed, including regions the heuristics skipped; it does not mean every candidate was checked. Finding no hut does not show that the range has none. The program writes nothing to standard output; if the search fails, the error goes to standard error after exit and the exit code is 1, otherwise 0.

## Limitations

- The fast noise screens and the sparse terrain prescreen can exclude lower huts. Every reported hut still passes full biome validation and a 63-column floor height computation.
- Terrain is generated only through the SURFACE and CARVERS stages; earlier features and overlapping structures are not simulated, so heights are estimates to confirm in game.
- When several huts share a floor height, a multithreaded search may order them differently, and differ in which of them make the top N; a single-threaded search is deterministic.
