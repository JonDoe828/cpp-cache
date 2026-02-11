// lfu_cache.test.cpp
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

#include "LfuCache.h"

// 说明：
// 你的实现里：
// - 新节点 freq=1
// - get/put(已有key) 会 removeFromFreqList -> freq++ -> addToFreqList
// - kickOut() 从 minFreq_ 对应链表取 getFirstNode()（head_->next）删除
// - addNode 是加到 tail 前（尾插）
// => 同频次下被淘汰的是链表头部那个（更早进入该频次链表的元素）
//    这在很多实现里等价于“同频次按 LRU/FIFO
//    的一种近似”（取决于你是否在同频次下也会因为访问而移动位置；你这里访问会升频，所以同频次内部顺序基本就是进入该频次的时间顺序）。

TEST_CASE("LFU: put/get basic works", "[lfu]") {
  LfuCache<int, std::string> cache(3);

  cache.put(1, "a");
  cache.put(2, "b");

  std::string v;
  REQUIRE(cache.get(1, v));
  REQUIRE(v == "a");

  REQUIRE(cache.get(2, v));
  REQUIRE(v == "b");

  REQUIRE_FALSE(cache.get(3, v));
}

TEST_CASE("LFU: put overwrites value and key remains present", "[lfu]") {
  LfuCache<int, std::string> cache(2);

  cache.put(1, "a");
  cache.put(1, "a2");

  std::string v;
  REQUIRE(cache.get(1, v));
  REQUIRE(v == "a2");
}

TEST_CASE("LFU: eviction removes least-frequent key", "[lfu]") {
  LfuCache<int, std::string> cache(2);

  cache.put(1, "a");
  cache.put(2, "b");

  // 提升 key=1 的频次
  REQUIRE(cache.get(1) == "a"); // 1 的 freq 变 2

  // 插入新元素触发淘汰：应该淘汰 freq 最小的 key=2
  cache.put(3, "c");

  std::string v;
  REQUIRE(cache.get(1, v));
  REQUIRE(v == "a");

  REQUIRE_FALSE(cache.get(2, v));
  REQUIRE(cache.get(3, v));
  REQUIRE(v == "c");
}

TEST_CASE("LFU: eviction tie-breaker within same frequency evicts "
          "oldest-in-that-freq-list",
          "[lfu]") {
  // 这个用例专门验证“同 freq 时踢谁”
  // 你的实现：同 freq 的链表里，最先进入该 freq 链表的节点在 head_->next，
  // kickOut 取 getFirstNode()，所以踢的是“更早进入该 freq 桶”的那个。

  LfuCache<int, std::string> cache(2);

  cache.put(1, "a"); // freq=1, order: [1]
  cache.put(2, "b"); // freq=1, order: [1,2]

  // 两个都是 freq=1，插入 3 会踢掉 1（更早进 freq=1 桶）
  cache.put(3, "c");

  std::string v;
  REQUIRE_FALSE(cache.get(1, v));
  REQUIRE(cache.get(2, v));
  REQUIRE(v == "b");
  REQUIRE(cache.get(3, v));
  REQUIRE(v == "c");
}

TEST_CASE("LFU: capacity 0 does nothing", "[lfu]") {
  LfuCache<int, int> cache(0);

  cache.put(1, 10);
  int v = 0;
  REQUIRE_FALSE(cache.get(1, v));
  REQUIRE(cache.get(1) == 0); // 你的 get(Key) 会返回默认构造 Value
}

TEST_CASE("LFU: purge clears all entries", "[lfu]") {
  LfuCache<int, std::string> cache(3);

  cache.put(1, "a");
  cache.put(2, "b");
  cache.put(3, "c");

  cache.purge();

  std::string v;
  REQUIRE_FALSE(cache.get(1, v));
  REQUIRE_FALSE(cache.get(2, v));
  REQUIRE_FALSE(cache.get(3, v));
}

TEST_CASE("KHashLFU: basic put/get works across slices", "[khashlfu]") {
  // sliceNum=2，容量总 10（每片 ceil(10/2)=5）
  KHashLfuCache<int, std::string> cache(/*capacity*/ 10, /*sliceNum*/ 2,
                                        /*maxAverageNum*/ 1000);

  for (int i = 1; i <= 8; ++i) {
    cache.put(i, "v" + std::to_string(i));
  }

  for (int i = 1; i <= 8; ++i) {
    std::string v;
    REQUIRE(cache.get(i, v));
    REQUIRE(v == "v" + std::to_string(i));
  }
}

TEST_CASE("LFU: concurrent smoke test (no crash, values readable)",
          "[lfu][thread]") {
  LfuCache<int, int> cache(50);

  std::atomic<bool> start{false};
  auto writer = [&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 2000; ++i) {
      cache.put(i % 100, i);
    }
  };

  auto reader = [&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    int out = 0;
    for (int i = 0; i < 2000; ++i) {
      (void)cache.get(i % 100, out);
    }
  };

  std::thread t1(writer), t2(writer), t3(reader), t4(reader);
  start.store(true, std::memory_order_release);

  t1.join();
  t2.join();
  t3.join();
  t4.join();

  // 最后简单验证：至少能读到一些 key（不要求确定值，避免时序不稳定）
  int v = 0;
  bool ok = cache.get(0, v) || cache.get(1, v) || cache.get(2, v);
  REQUIRE(ok);
}
