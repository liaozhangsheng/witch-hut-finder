cmake_minimum_required(VERSION 3.20)
project(witch_hut_finder LANGUAGES CXX)

find_package(Threads REQUIRED)
add_subdirectory(3rdparty/FTXUI EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/simdjson EXCLUDE_FROM_ALL)

include(cmake/embed_json.cmake)
minecraft_embed_json("${CMAKE_CURRENT_SOURCE_DIR}/data/versions/26.1/worldgen.json"
    worldgen_json "${CMAKE_CURRENT_BINARY_DIR}/generated/minecraft/worldgen_data.hpp")
minecraft_embed_json("${CMAKE_CURRENT_SOURCE_DIR}/data/versions/26.1/features.json"
    feature_json "${CMAKE_CURRENT_BINARY_DIR}/generated/minecraft/feature_data.hpp")

add_library(minecraft_worldgen STATIC
    src/minecraft/types.cpp
    src/minecraft/random/random.cpp
    src/minecraft/biome/zoom.cpp
    src/minecraft/biome/sampler.cpp
    src/minecraft/worldgen/noise.cpp
    src/minecraft/worldgen/rules.cpp
    src/minecraft/worldgen/density.cpp
    src/minecraft/worldgen/program.cpp
    src/minecraft/terrain/terrain.cpp
    src/minecraft/terrain/veins.cpp
    src/minecraft/structure/profile.cpp
    src/minecraft/feature/rules.cpp
    src/minecraft/feature/world.cpp
    src/minecraft/feature/surface.cpp
    src/minecraft/feature/carver.cpp
)
target_link_libraries(minecraft_worldgen PRIVATE simdjson::simdjson PUBLIC Threads::Threads)
target_compile_features(minecraft_worldgen PUBLIC cxx_std_20)
target_compile_options(minecraft_worldgen PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-fast-math -ffp-contract=off>
)
target_compile_options(minecraft_worldgen PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
target_include_directories(minecraft_worldgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(witch_hut_finder src/tui.cpp)
target_link_libraries(witch_hut_finder PRIVATE minecraft_worldgen ftxui::component)
set_target_properties(witch_hut_finder PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CTest)
if(BUILD_TESTING)
    add_executable(witch_hut_search_test tests/witch_hut_search_test.cpp)
    target_link_libraries(witch_hut_search_test PRIVATE minecraft_worldgen)
    add_test(NAME witch_hut_search_threshold COMMAND witch_hut_search_test)
endif()
