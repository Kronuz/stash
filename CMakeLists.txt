cmake_minimum_required(VERSION 3.14)
project(stash LANGUAGES CXX)

add_library(stash INTERFACE)
target_include_directories(stash INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(stash INTERFACE cxx_std_17)

# Only build the smoke test when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(stash_test test/test.cc)
  target_link_libraries(stash_test PRIVATE stash)
  add_test(NAME stash COMMAND stash_test)

  # A runnable tour of the data structure (not a test): build & run
  # ./build/stash_demo. Header-only like the test, so it just links the
  # interface target.
  add_executable(stash_demo examples/demo.cc)
  target_link_libraries(stash_demo PRIVATE stash)
endif()
