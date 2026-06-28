# Benchmarks for the stash library.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/bench_structures      # stash vs queues (no external deps)
#   ./build/bench_steady          # bounded-depth arm latency (the queue-depth regime)
#   ./build/bench_scheduler       # stash vs asio::steady_timer vs heap
#
# bench_structures has no dependencies beyond stash.h. bench_steady and
# bench_scheduler also compare against asio::steady_timer, fetched standalone
# (header-only).

cmake_minimum_required(VERSION 3.16)
project(stash_bench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# stash.h and stash_trace.h live one directory up, at the repo root.
set(STASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(bench_structures bench_structures.cc)
target_include_directories(bench_structures PRIVATE ${STASH_ROOT})
target_link_libraries(bench_structures PRIVATE Threads::Threads)

# The scheduler shoot-out needs standalone Asio for asio::steady_timer.
include(FetchContent)
FetchContent_Declare(
	asio
	GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
	GIT_TAG        asio-1-30-2
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)

add_executable(bench_scheduler bench_scheduler.cc)
target_include_directories(bench_scheduler PRIVATE ${STASH_ROOT} ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(bench_scheduler PRIVATE ASIO_STANDALONE)
target_link_libraries(bench_scheduler PRIVATE Threads::Threads)

# Bounded-depth arm-latency benchmark; also compares against asio::steady_timer
# (the file defines ASIO_STANDALONE itself).
add_executable(bench_steady bench_steady.cc)
target_include_directories(bench_steady PRIVATE ${STASH_ROOT} ${asio_SOURCE_DIR}/asio/include)
target_link_libraries(bench_steady PRIVATE Threads::Threads)
