cmake_minimum_required(VERSION 3.16)

project(cpp_cache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== Catch2 =====
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.4
)

FetchContent_MakeAvailable(Catch2)

# ===== cache headers (header-only) =====
add_library(cache_lib INTERFACE)
target_include_directories(cache_lib INTERFACE
  ${CMAKE_SOURCE_DIR}/include
)

# ===== tests =====
add_executable(cache_tests
  tests/testAllCachePolicy.cpp
)

target_link_libraries(cache_tests PRIVATE
  cache_lib
  Catch2::Catch2WithMain
)

# Warnings
if(MSVC)
  target_compile_options(cache_tests PRIVATE /W4)
else()
  target_compile_options(cache_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
