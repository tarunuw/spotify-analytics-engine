cmake_minimum_required(VERSION 3.16)
project(sae VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# The engine itself, so the CLI, the tests and the benchmark all link the same
# code rather than recompiling it three ways.
add_library(sae_core STATIC
  src/json.cpp
  src/ingest.cpp
  src/analytics.cpp
)
target_include_directories(sae_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(sae src/main.cpp)
target_link_libraries(sae PRIVATE sae_core)

enable_testing()

add_executable(sae_tests
  tests/test_main.cpp
  tests/test_json.cpp
  tests/test_time.cpp
  tests/test_topk.cpp
  tests/test_ingest.cpp
  tests/test_analytics.cpp
)
target_link_libraries(sae_tests PRIVATE sae_core)
target_include_directories(sae_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME unit COMMAND sae_tests)

add_executable(sae_bench bench/bench.cpp)
target_link_libraries(sae_bench PRIVATE sae_core)
