#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>

#include "LruCache.h"

TEST_CASE("LRU: put/get basic hit-miss", "[lru]") {
  LruCache<int, std::string> cache(2);

  std::string out;
  REQUIRE_FALSE(cache.get(1, out));

  cache.put(1, "a");
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");

  cache.put(2, "b");
  REQUIRE(cache.get(2, out));
  REQUIRE(out == "b");
}

TEST_CASE("LRU: eviction removes least recently used (get refreshes recency)",
          "[lru]") {
  LruCache<int, std::string> cache(2);

  cache.put(1, "a");
  cache.put(2, "b");

  // get(1) => 2 becomes LRU
  std::string out;
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");

  cache.put(3, "c"); // should evict key=2

  REQUIRE_FALSE(cache.get(2, out));
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");
  REQUIRE(cache.get(3, out));
  REQUIRE(out == "c");
}

TEST_CASE("LRU: put existing key updates value and refreshes recency",
          "[lru]") {
  LruCache<int, std::string> cache(2);

  cache.put(1, "a");
  cache.put(2, "b");

  // update key=1 => key=2 becomes LRU
  cache.put(1, "a2");

  cache.put(3, "c"); // should evict key=2

  std::string out;
  REQUIRE_FALSE(cache.get(2, out));
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a2");
  REQUIRE(cache.get(3, out));
  REQUIRE(out == "c");
}

TEST_CASE("LRU: capacity 1 always keeps most recent", "[lru]") {
  LruCache<int, std::string> cache(1);

  cache.put(1, "a");
  cache.put(2, "b"); // evict 1

  std::string out;
  REQUIRE_FALSE(cache.get(1, out));
  REQUIRE(cache.get(2, out));
  REQUIRE(out == "b");

  // access 2, then insert 3 => evict 2 anyway because cap=1
  REQUIRE(cache.get(2, out));
  cache.put(3, "c");

  REQUIRE_FALSE(cache.get(2, out));
  REQUIRE(cache.get(3, out));
  REQUIRE(out == "c");
}

TEST_CASE("LRU: zero capacity stores nothing", "[lru]") {
  LruCache<int, std::string> cache(0);

  cache.put(1, "a");

  std::string out;
  REQUIRE_FALSE(cache.get(1, out));

  // get(key) 返回默认值（string 就是空）
  REQUIRE(cache.get(1).empty());
}

// 可选：随机操作烟雾测试（不做严格断言，只保证不崩溃且命中率在合理范围）
// 你如果嫌慢可以删掉这个 case
TEST_CASE("LRU: random workload smoke test", "[lru][smoke]") {
  const int CAP = 50;
  const int OPS = 20000;
  const int KEY_RANGE = 500;

  LruCache<int, std::string> cache(CAP);

  std::mt19937 gen(123);
  int hits = 0, gets = 0;

  for (int i = 0; i < OPS; ++i) {
    const int key = static_cast<int>(gen() % KEY_RANGE);
    const bool isPut = (gen() % 100) < 30;

    if (isPut) {
      cache.put(key, "v" + std::to_string(key));
    } else {
      std::string out;
      ++gets;
      if (cache.get(key, out))
        ++hits;
    }
  }

  // 只做宽松检查：至少执行了 get
  REQUIRE(gets > 0);
  // 命中率不会是 0（正常情况下应该有命中）
  REQUIRE(hits > 0);
}

TEST_CASE("LRU-K: put does not immediately enter main cache, get promotes at k",
          "[lruk]") {
  // main cap=2, history cap=10, k=2
  LruKCache<int, std::string> cache(2, 10, 2);

  cache.put(1, "a");

  // 第一次 get：historyCount 从 1->2 达到 k，且有 historyValueMap 值 =>
  // 直接提升并返回
  REQUIRE(cache.get(1) == "a");

  // 已在主缓存，后续 get 仍是 a
  REQUIRE(cache.get(1) == "a");
}

TEST_CASE("LRU-K: key never put cannot be promoted (returns default value)",
          "[lruk]") {
  LruKCache<int, std::string> cache(2, 10, 2);

  // 从没 put 过 => historyValueMap 没值
  REQUIRE(cache.get(42).empty());
  REQUIRE(cache.get(42).empty()); // 即使次数够了也无法提升
}

TEST_CASE("LRU-K: promotion triggers main cache eviction by LRU order",
          "[lruk]") {
  // 主缓存容量=1，方便观察驱逐
  LruKCache<int, std::string> cache(1, 10, 2);

  cache.put(1, "a");
  REQUIRE(cache.get(1) == "a"); // 提升 1 进入主缓存（k=2）

  cache.put(2, "b");
  REQUIRE(cache.get(2) == "b"); // 提升 2，会挤掉主缓存里的 1（cap=1）

  // 1 被挤出主缓存；并且 1 在提升时 historyValueMap 已删除，无法再恢复值
  REQUIRE(cache.get(1).empty());
  REQUIRE(cache.get(2) == "b");
}

TEST_CASE("LRU-K: polymorphic get uses history promotion", "[lruk]") {
  LruKCache<int, std::string> cache(2, 10, 2);
  ICachePolicy<int, std::string> *policy = &cache;

  policy->put(1, "a");
  std::string value;
  REQUIRE(policy->get(1, value));
  REQUIRE(value == "a");
  REQUIRE(policy->get(1, value));
}

TEST_CASE("LRU-K: evicted history does not retain stale values", "[lruk]") {
  LruKCache<int, std::string> cache(2, 1, 2);
  cache.put(1, "a");
  cache.put(2, "b");

  std::string value;
  REQUIRE_FALSE(cache.get(1, value));
}

TEST_CASE("LRU-K: polymorphic remove and purge clear history", "[lruk]") {
  LruKCache<int, std::string> cache(2, 10, 2);
  LruCache<int, std::string> *base = &cache;

  cache.put(1, "one");
  base->remove(1);
  std::string value;
  REQUIRE_FALSE(cache.get(1, value));

  cache.put(2, "two");
  base->purge();
  REQUIRE_FALSE(cache.get(2, value));
}

TEST_CASE("Sharded LRU: basic put/get works", "[sharded-lru]") {
  KHashLruCaches<int, std::string> cache(/*capacity*/ 4, /*sliceNum*/ 2);

  cache.put(1, "a");
  cache.put(2, "b");

  std::string out;
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");
  REQUIRE(cache.get(2, out));
  REQUIRE(out == "b");

  // get(key) 版本
  REQUIRE(cache.get(1) == "a");
  REQUIRE(cache.get(999).empty());
}

TEST_CASE("Sharded LRU: eviction happens within shard (sliceNum=1)",
          "[sharded-lru]") {
  // sliceNum=1 => 就是一个 LRU，容量=2
  KHashLruCaches<int, std::string> cache(/*capacity*/ 2, /*sliceNum*/ 1);

  cache.put(1, "a");
  cache.put(2, "b");

  // 访问 1，使 2 成为 LRU
  std::string out;
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");

  cache.put(3, "c"); // 应该驱逐 2

  REQUIRE_FALSE(cache.get(2, out));
  REQUIRE(cache.get(1, out));
  REQUIRE(out == "a");
  REQUIRE(cache.get(3, out));
  REQUIRE(out == "c");
}

TEST_CASE("Sharded LRU: non-positive slice count has a safe fallback",
          "[sharded-lru][boundary]") {
  KHashLruCaches<int, std::string> cache(2, 0);
  cache.put(1, "a");

  std::string value;
  REQUIRE(cache.get(1, value));
  REQUIRE(value == "a");
}
