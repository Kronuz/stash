cmake_minimum_required(VERSION 3.14)
project(stash LANGUAGES CXX)

add_library(stash INTERFACE)
target_include_directories(stash INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(stash INTERFACE cxx_std_17)

enable_testing()
add_executable(stash_test test/test.cc)
target_link_libraries(stash_test PRIVATE stash)
add_test(NAME stash COMMAND stash_test)
