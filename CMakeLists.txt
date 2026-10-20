cmake_minimum_required(VERSION 3.14)
project(stash LANGUAGES CXX)

add_library(stash INTERFACE)
add_library(stash::stash ALIAS stash)
target_include_directories(stash INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(stash INTERFACE cxx_std_17)

# Only build the smoke test when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(stash_test test/test.cc)
  target_link_libraries(stash_test PRIVATE stash)
  add_test(NAME stash COMMAND stash_test)
  # A trace hook that throws inside add must not strand a claimed slot.
  add_executable(stash_trace_throw test/trace_throw.cc)
  target_link_libraries(stash_trace_throw PRIVATE stash)
  add_test(NAME stash_trace_throw COMMAND stash_trace_throw)
  # Producers racing the walking cleaner for 2 s with tasks due at now() (no
  # lead, the descent window's worst case); exits nonzero on a lost or
  # double-fired task, so the sanitizer jobs check reclamation too. For a longer,
  # oversubscribed run see test/stress.sh.
  find_package(Threads REQUIRED)
  add_executable(stash_concurrent test/concurrent.cc)
  target_link_libraries(stash_concurrent PRIVATE stash Threads::Threads)
  add_test(NAME stash_concurrent COMMAND stash_concurrent 4 2.0 32 0 0)
  set_tests_properties(stash_concurrent PROPERTIES TIMEOUT 60)

  # A runnable tour of the data structure (not a test): build & run
  # ./build/stash_demo. Header-only like the test, so it just links the
  # interface target.
  add_executable(stash_demo examples/demo.cc)
  target_link_libraries(stash_demo PRIVATE stash)
endif()
