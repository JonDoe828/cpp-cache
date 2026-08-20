#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

#include "arc/ArcCache.h"

TEST_CASE("ARC: basic put, get, and update", "[arc]") {
  ArcCache<int, std::string> cache(2);
  cache.put(1, "a");

  std::string value;
  REQUIRE(cache.get(1, value));
  REQUIRE(value == "a");

  cache.put(1, "updated");
  REQUIRE(cache.get(1, value));
  REQUIRE(value == "updated");
}

TEST_CASE("ARC: resident entries never exceed capacity", "[arc]") {
  ArcCache<int, std::string> cache(2);
  std::string value;

  cache.put(1, "one");
  REQUIRE(cache.get(1, value));
  cache.put(2, "two");
  cache.put(3, "three");

  REQUIRE(cache.size() == 2);
  const int hitCount = static_cast<int>(cache.get(1, value)) +
                       static_cast<int>(cache.get(2, value)) +
                       static_cast<int>(cache.get(3, value));
  REQUIRE(hitCount == 2);
}

TEST_CASE("ARC: ghost hits adapt without exceeding capacity", "[arc]") {
  ArcCache<int, std::string> cache(2);
  std::string value;

  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(3, "three");
  REQUIRE_FALSE(cache.get(1, value));

  cache.put(1, "one-new");
  REQUIRE(cache.get(1, value));
  REQUIRE(value == "one-new");
  REQUIRE(cache.size() == 2);
}

TEST_CASE("ARC: zero capacity stores nothing", "[arc][boundary]") {
  ArcCache<int, int> cache(0);
  cache.put(1, 10);

  int value = 0;
  REQUIRE_FALSE(cache.get(1, value));
  REQUIRE(cache.size() == 0);
}

TEST_CASE("ARC: concurrent reads and writes preserve capacity",
          "[arc][thread]") {
  ArcCache<int, int> cache(32);
  std::atomic<bool> start{false};

  auto worker = [&](int offset) {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 2000; ++i) {
      const int key = (i + offset) % 64;
      cache.put(key, i);
      int value = 0;
      (void)cache.get(key, value);
    }
  };

  std::thread first(worker, 0);
  std::thread second(worker, 7);
  std::thread third(worker, 13);
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  third.join();

  REQUIRE(cache.size() <= 32);
}
