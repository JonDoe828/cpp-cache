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

# ===== cache headers (header-only) =====
add_library(cache_lib INTERFACE)
add_library(cppcache::cache ALIAS cache_lib)
target_compile_features(cache_lib INTERFACE cxx_std_20)
target_include_directories(cache_lib INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

option(CPP_CACHE_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(CPP_CACHE_ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "CPP_CACHE_ENABLE_TSAN requires GCC or Clang")
  endif()
  target_compile_options(cache_lib INTERFACE
    -fsanitize=thread
    -fno-omit-frame-pointer
  )
  target_link_options(cache_lib INTERFACE -fsanitize=thread)
endif()

add_library(storage_lib STATIC
  src/storage/CachedKvStore.cpp
  src/storage/InMemoryKvStore.cpp
  src/storage/KvCodec.cpp
  src/storage/KvStateMachine.cpp
)
add_library(cppcache::storage ALIAS storage_lib)
target_link_libraries(storage_lib PUBLIC cppcache::cache)

add_library(raft_lib STATIC
  src/raft/FileRaftStorage.cpp
  src/raft/InProcessTransport.cpp
  src/raft/MemoryRaftStorage.cpp
  src/raft/RaftCore.cpp
  src/raft/RaftNode.cpp
  src/raft/TcpKvClient.cpp
  src/raft/TcpProtocol.cpp
  src/raft/TcpSocket.cpp
  src/raft/TcpTransport.cpp
)
add_library(cppcache::raft ALIAS raft_lib)
target_link_libraries(raft_lib PUBLIC cppcache::storage)

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

add_executable(cpp_cache_node app/node_main.cpp)
target_link_libraries(cpp_cache_node PRIVATE cppcache::raft)
set_warnings(cpp_cache_node)

add_executable(cpp_cache_cli app/client_main.cpp)
target_link_libraries(cpp_cache_cli PRIVATE cppcache::raft)
set_warnings(cpp_cache_cli)

option(CPP_CACHE_BUILD_BENCHMARKS "Build benchmark executables" OFF)

if(CPP_CACHE_BUILD_BENCHMARKS)
  add_executable(cache_policy_benchmark
    benchmarks/cache_policy_benchmark.cpp
  )
  target_link_libraries(cache_policy_benchmark PRIVATE cppcache::cache)
  set_warnings(cache_policy_benchmark)

  add_executable(raft_benchmark benchmarks/raft_benchmark.cpp)
  target_link_libraries(raft_benchmark PRIVATE cppcache::raft)
  set_warnings(raft_benchmark)
endif()

if(BUILD_TESTING)
  include(FetchContent)

  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.4
  )

  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  include(Catch)

  add_custom_target(tests)

  function(add_cpp_cache_test test_target test_source project_library)
    add_executable(${test_target} ${test_source})
    target_link_libraries(${test_target} PRIVATE
      ${project_library}
      Catch2::Catch2WithMain
    )
    set_warnings(${test_target})
    catch_discover_tests(${test_target} DISCOVERY_MODE PRE_TEST)
    add_dependencies(tests ${test_target})
  endfunction()

  add_cpp_cache_test(arc tests/arc.test.cpp cppcache::cache)
  add_cpp_cache_test(lfu tests/lfu.test.cpp cppcache::cache)
  add_cpp_cache_test(lru tests/lru.test.cpp cppcache::cache)
  add_cpp_cache_test(storage tests/storage.test.cpp cppcache::storage)
  add_cpp_cache_test(raft tests/raft.test.cpp cppcache::raft)
  add_cpp_cache_test(raft_cluster tests/raft_cluster.test.cpp cppcache::raft)
  add_cpp_cache_test(tcp_cluster tests/tcp_cluster.test.cpp cppcache::raft)
endif()
