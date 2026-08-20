cmake_minimum_required(VERSION 3.16)

project(cpp_cache LANGUAGES CXX)

if(WIN32)
  message(FATAL_ERROR "cpp-cache currently requires POSIX sockets and fsync")
endif()

include(CTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
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
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# ===== cache headers (header-only) =====
add_library(cache_lib INTERFACE)
target_include_directories(cache_lib INTERFACE
  ${CMAKE_SOURCE_DIR}/include
)

add_library(storage_lib STATIC
  src/storage/CachedKvStore.cpp
  src/storage/InMemoryKvStore.cpp
  src/storage/KvCodec.cpp
  src/storage/KvStateMachine.cpp
)
target_link_libraries(storage_lib PUBLIC cache_lib)

add_library(raft_lib STATIC
  src/raft/FileRaftStorage.cpp
  src/raft/InProcessTransport.cpp
  src/raft/MemoryRaftStorage.cpp
  src/raft/RaftCore.cpp
  src/raft/RaftNode.cpp
  src/raft/TcpTransport.cpp
)
target_link_libraries(raft_lib PUBLIC storage_lib)

# ===== Common warnings helper =====
function(set_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

set_warnings(storage_lib)
set_warnings(raft_lib)

# ===== (1) Keep your big comparison test =====
add_executable(cache_tests
  tests/testAllCachePolicy.cpp
)

target_link_libraries(cache_tests PRIVATE
  cache_lib
)

set_warnings(cache_tests)
add_test(NAME cache_policy_benchmark_smoke COMMAND cache_tests)

add_executable(cpp_cache_node app/node_main.cpp)
target_link_libraries(cpp_cache_node PRIVATE raft_lib)
set_warnings(cpp_cache_node)

add_executable(cpp_cache_cli app/client_main.cpp)
target_link_libraries(cpp_cache_cli PRIVATE raft_lib)
set_warnings(cpp_cache_cli)


file(GLOB_RECURSE UNIT_TEST_FILES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/tests/*.test.cpp"
)

# Aggregate target: build all unit tests
add_custom_target(tests)

foreach(test_file IN LISTS UNIT_TEST_FILES)
  # file name without extensions: lru.test.cpp -> lru
  get_filename_component(test_name_we "${test_file}" NAME_WE)

  set(test_target "${test_name_we}")
  string(REPLACE "-" "_" test_target "${test_target}")

  add_executable(${test_target} ${test_file})

  target_link_libraries(${test_target} PRIVATE
    raft_lib
    Catch2::Catch2WithMain
  )

  set_warnings(${test_target})
  catch_discover_tests(${test_target})

  add_dependencies(tests ${test_target})
endforeach()
