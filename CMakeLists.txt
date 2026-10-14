cmake_minimum_required(VERSION 3.16)
project(metrics LANGUAGES CXX)

# Header-only: metrics.hh is the whole library.
add_library(metrics INTERFACE)
target_include_directories(metrics INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(metrics INTERFACE cxx_std_20)
add_library(metrics::metrics ALIAS metrics)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(metrics_test test/test.cc)
  target_link_libraries(metrics_test PRIVATE metrics Threads::Threads)
  add_test(NAME metrics COMMAND metrics_test)
  set_tests_properties(metrics PROPERTIES TIMEOUT 10)

  add_executable(metrics_bench bench/bench.cc)
  target_link_libraries(metrics_bench PRIVATE metrics)
endif()
