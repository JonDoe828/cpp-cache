cmake_minimum_required(VERSION 3.16)
project(CacheSystem)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cache_lib INTERFACE)
target_include_directories(cache_lib INTERFACE include)

add_executable(main tests/testAllCachePolicy.cpp)
target_link_libraries(main PRIVATE cache_lib)