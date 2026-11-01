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

  add_executable(metrics_differential test/differential.cc)
  target_link_libraries(metrics_differential PRIVATE metrics Threads::Threads)
  add_test(NAME metrics_differential COMMAND metrics_differential)
  set_tests_properties(metrics_differential PROPERTIES TIMEOUT 300)

  # Exhaustive interleaving model of the atomic counters at 8 bits (single-threaded).
  add_executable(metrics_model test/model.cc)
  target_link_libraries(metrics_model PRIVATE metrics)
  add_test(NAME metrics_model COMMAND metrics_model)
  set_tests_properties(metrics_model PROPERTIES TIMEOUT 300)

  # The counters at 64 bits, started from words around their thresholds through a recording atomic.
  add_executable(metrics_seeded test/seeded.cc)
  target_link_libraries(metrics_seeded PRIVATE metrics Threads::Threads)
  add_test(NAME metrics_seeded COMMAND metrics_seeded)
  set_tests_properties(metrics_seeded PROPERTIES TIMEOUT 300)

  # Benchmarks (off by default): update cost of each counter and histogram (metrics_bench, built
  # with the build type's flags: use Release), and contended adds against a bare fetch_add and a
  # compare-exchange loop (metrics_counter_bench, always -O3):
  #   cmake -B build -DMETRICS_BUILD_BENCH=ON && cmake --build build
  #   ./build/metrics_bench; ./build/metrics_counter_bench
  option(METRICS_BUILD_BENCH "Build bench/bench.cc and bench/counter_bench.cc" OFF)
  if(METRICS_BUILD_BENCH)
    add_executable(metrics_counter_bench bench/counter_bench.cc)
    target_link_libraries(metrics_counter_bench PRIVATE metrics Threads::Threads)
    target_compile_options(metrics_counter_bench PRIVATE -O3 -DNDEBUG)

    add_executable(metrics_bench bench/bench.cc)
    target_link_libraries(metrics_bench PRIVATE metrics)
  endif()
endif()
