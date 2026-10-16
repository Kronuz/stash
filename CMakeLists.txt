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
  # Producers racing the walking cleaner for 2 s; exits nonzero on a lost or
  # double-fired task, so the sanitizer jobs check reclamation too. Tasks lead now()
  # by 20 ms: with no lead, a producer preempted mid-insert can miss its task's
  # slot (the descent window), which this test is not about.
  find_package(Threads REQUIRED)
  add_executable(stash_concurrent test/concurrent.cc)
  target_link_libraries(stash_concurrent PRIVATE stash Threads::Threads)
  add_test(NAME stash_concurrent COMMAND stash_concurrent 4 2.0 32 0 20)
  set_tests_properties(stash_concurrent PROPERTIES TIMEOUT 60)

  # A runnable tour of the data structure (not a test): build & run
  # ./build/stash_demo. Header-only like the test, so it just links the
  # interface target.
  add_executable(stash_demo examples/demo.cc)
  target_link_libraries(stash_demo PRIVATE stash)
endif()
